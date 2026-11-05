/* SPDX-License-Identifier: Apache-2.0
 * BLE central on NimBLE, controller on the C6 (esp_hosted HCI).
 */
#include <stdlib.h>
#include <string.h>
#include "esp_hosted_bt_host_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_net/ble.h"
#include "t5_net/net.h"
#include "t5_power/power.h"

static const char *TAG = "t5.ble";

#define OP_TIMEOUT_MS 10000

static bool s_enabled;          /* the user wants BLE */
static bool s_link;             /* the C6 link is up */
static volatile bool s_running; /* stack started */
static volatile bool s_synced;  /* host and controller in sync */
static uint8_t s_own_addr_type;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static t5_wakelock_t *s_lock;

static t5_mutex_t s_mtx;
static t5_ble_dev_t s_scan[T5_BLE_MAX_SCAN];
static int s_nscan;
static t5_ble_chr_t s_chr[T5_BLE_MAX_CHARS];
static int s_nchr;

/* One synchronous GATT operation at a time. */
static SemaphoreHandle_t s_op_done;
static SemaphoreHandle_t s_op_lock;
static volatile int s_op_status;
static uint8_t *s_op_buf;
static size_t s_op_cap, s_op_len;
static struct { uint16_t start, end; char uuid[40]; } s_svcs[16];
static int s_nsvc;

static t5_ble_notify_cb_t s_notify_cb;
static void *s_notify_arg;

static void post(t5_event_id_t id, int32_t err)
{
    t5_ev_net_t ev = { .err = err, .count = (uint8_t)s_nscan };
    t5_event_post(id, &ev, sizeof ev);
}

/* ---- GAP ----------------------------------------------------------------- */

static void add_scan(const struct ble_gap_disc_desc *d)
{
    t5_mutex_lock(s_mtx);
    int i;
    for (i = 0; i < s_nscan; i++) {
        if (!memcmp(s_scan[i].addr, d->addr.val, 6)) break;
    }
    if (i == s_nscan) {
        if (s_nscan == T5_BLE_MAX_SCAN) {
            t5_mutex_unlock(s_mtx);
            return;
        }
        memset(&s_scan[i], 0, sizeof s_scan[i]);
        memcpy(s_scan[i].addr, d->addr.val, 6);
        s_scan[i].addr_type = d->addr.type;
        s_nscan++;
    }
    s_scan[i].rssi = d->rssi;
    char name[T5_BLE_NAME_LEN];
    if (t5_ble_adv_name(d->data, d->length_data, name, sizeof name)) memcpy(s_scan[i].name, name, sizeof name);
    t5_mutex_unlock(s_mtx);
}

static int gap_event(struct ble_gap_event *e, void *arg)
{
    (void)arg;
    switch (e->type) {
    case BLE_GAP_EVENT_DISC:
        add_scan(&e->disc);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        post(T5_EV_BLE_SCAN_DONE, T5_OK);
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            s_conn = e->connect.conn_handle;
            if (!s_lock) s_lock = t5_wakelock_acquire(T5_LOCK_NET, "ble");
            T5_LOGI(TAG, "connected (handle %u)", s_conn);
            post(T5_EV_BLE_CONNECTED, T5_OK);
        } else {
            T5_LOGW(TAG, "connect failed: %d", e->connect.status);
            post(T5_EV_BLE_DISCONNECTED, e->connect.status);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        T5_LOGI(TAG, "disconnected: reason 0x%x", e->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        if (s_lock) {
            t5_wakelock_release(s_lock);
            s_lock = NULL;
        }
        post(T5_EV_BLE_DISCONNECTED, e->disconnect.reason);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t buf[256];
        uint16_t n = OS_MBUF_PKTLEN(e->notify_rx.om);
        if (n > sizeof buf) n = sizeof buf;
        os_mbuf_copydata(e->notify_rx.om, 0, n, buf);
        if (s_notify_cb) s_notify_cb(e->notify_rx.attr_handle, buf, n, s_notify_arg);
        return 0;
    }
    default:
        return 0;
    }
}

/* ---- stack lifecycle ----------------------------------------------------- */

static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) == 0) {
        s_synced = true;
        T5_LOGI(TAG, "ready");
        post(T5_EV_BLE_READY, T5_OK);
    }
}

static void on_reset(int reason)
{
    s_synced = false;
    T5_LOGW(TAG, "host reset: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void stack_start(void)
{
    if (s_running || !s_enabled || !s_link) return;
    esp_hosted_bt_host_stack_cfg_t cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    /* C6 firmware before esp_hosted 2.x (the factory 1.4.1 on Unit A) brings
     * its controller up at boot and doesn't know the controller RPCs. */
    t5_net_status_t ns;
    t5_net_status(&ns);
    if (ns.c6_version[0] && atoi(ns.c6_version) < 2) cfg.bring_up_controller = false;
    if (esp_hosted_bt_host_stack_setup(&cfg) != ESP_OK) {
        T5_LOGE(TAG, "controller on the C6 didn't start");
        return;
    }
    if (nimble_port_init() != ESP_OK) {
        T5_LOGE(TAG, "nimble_port_init failed");
        esp_hosted_bt_host_stack_teardown();
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    s_running = true;
    nimble_port_freertos_init(host_task);
}

static void stack_stop(void)
{
    if (!s_running) return;
    /* Stopping NimBLE while its host task is still in the start-up sync
     * frees timers that the sync then uses (seen as a crash in
     * ble_hs_timer_sched when Sleep powered the C6 off right after boot).
     * Let the start finish first. */
    for (int i = 0; i < 30 && !s_synced; i++) t5_delay_ms(100);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        post(T5_EV_BLE_DISCONNECTED, -1);
    }
    if (s_lock) {
        t5_wakelock_release(s_lock);
        s_lock = NULL;
    }
    nimble_port_stop();
    nimble_port_deinit();
    esp_hosted_bt_host_stack_teardown();
    s_running = false;
    s_synced = false;
}

static void ensure_init(void)
{
    if (!s_mtx) {
        s_mtx = t5_mutex_create();
        s_op_done = xSemaphoreCreateBinary();
        s_op_lock = xSemaphoreCreateMutex();
        nvs_handle_t h;
        uint8_t v = 0;
        if (nvs_open("t5net", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "ble", &v);
            nvs_close(h);
        }
        s_enabled = v;
    }
}

void t5_ble_on_link(bool up)
{
    ensure_init();
    s_link = up;
    if (up && s_enabled) stack_start();
    else stack_stop();
}

t5_err_t t5_ble_enable(bool on)
{
    ensure_init();
    s_enabled = on;
    nvs_handle_t h;
    if (nvs_open("t5net", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ble", on);
        nvs_commit(h);
        nvs_close(h);
    }
    /* The stack starts and stops on the net task, which owns the C6. */
    t5_net_ble_changed();
    return on && !s_link ? T5_ERR_STATE : T5_OK;   /* starts when the C6 is up */
}

bool t5_ble_ready(void)
{
    return s_synced;
}

/* ---- scanning and connecting --------------------------------------------- */

t5_err_t t5_ble_scan(uint32_t ms)
{
    if (!s_synced) return T5_ERR_STATE;
    t5_mutex_lock(s_mtx);
    s_nscan = 0;
    t5_mutex_unlock(s_mtx);
    struct ble_gap_disc_params p = { .passive = 0, .filter_duplicates = 1 };
    int rc = ble_gap_disc(s_own_addr_type, (int32_t)ms, &p, gap_event, NULL);
    return rc == 0 ? T5_OK : rc == BLE_HS_EALREADY ? T5_ERR_BUSY : T5_ERR_FAIL;
}

int t5_ble_scan_results(t5_ble_dev_t *out, int max)
{
    if (!s_mtx) return 0;
    t5_mutex_lock(s_mtx);
    int n = s_nscan < max ? s_nscan : max;
    memcpy(out, s_scan, (size_t)n * sizeof *out);
    t5_mutex_unlock(s_mtx);
    return n;
}

t5_err_t t5_ble_connect(const uint8_t addr[6], uint8_t addr_type)
{
    if (!s_synced) return T5_ERR_STATE;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) return T5_ERR_BUSY;
    ble_gap_disc_cancel();
    ble_addr_t a = { .type = addr_type };
    memcpy(a.val, addr, 6);
    int rc = ble_gap_connect(s_own_addr_type, &a, 10000, NULL, gap_event, NULL);
    return rc == 0 ? T5_OK : T5_ERR_FAIL;
}

t5_err_t t5_ble_disconnect(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return T5_ERR_STATE;
    return ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM) == 0 ? T5_OK : T5_ERR_FAIL;
}

bool t5_ble_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

/* ---- GATT ---------------------------------------------------------------- */

static bool op_begin(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return false;
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    xSemaphoreTake(s_op_done, 0);
    s_op_status = 0;
    return true;
}

static t5_err_t op_wait(int rc)
{
    t5_err_t err;
    if (rc != 0) {
        err = T5_ERR_FAIL;
    } else if (xSemaphoreTake(s_op_done, pdMS_TO_TICKS(OP_TIMEOUT_MS)) != pdTRUE) {
        err = T5_ERR_TIMEOUT;
    } else {
        err = s_op_status == 0 ? T5_OK : T5_ERR_IO;
    }
    xSemaphoreGive(s_op_lock);
    return err;
}

static void op_finish(int status)
{
    s_op_status = status;
    xSemaphoreGive(s_op_done);
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == 0 && s_nsvc < (int)(sizeof s_svcs / sizeof s_svcs[0])) {
        s_svcs[s_nsvc].start = svc->start_handle;
        s_svcs[s_nsvc].end = svc->end_handle;
        ble_uuid_to_str(&svc->uuid.u, s_svcs[s_nsvc].uuid);
        s_nsvc++;
    } else if (err->status) {
        op_finish(err->status == BLE_HS_EDONE ? 0 : err->status);
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn;
    int svc = (int)(intptr_t)arg;
    if (err->status == 0) {
        if (s_nchr < T5_BLE_MAX_CHARS) {
            t5_ble_chr_t *c = &s_chr[s_nchr++];
            memcpy(c->svc_uuid, s_svcs[svc].uuid, sizeof c->svc_uuid);
            ble_uuid_to_str(&chr->uuid.u, c->uuid);
            c->val_handle = chr->val_handle;
            c->def_handle = chr->def_handle;
            c->props = chr->properties;
        }
    } else {
        op_finish(err->status == BLE_HS_EDONE ? 0 : err->status);
    }
    return 0;
}

t5_err_t t5_ble_discover(void)
{
    if (!op_begin()) return T5_ERR_STATE;
    s_nsvc = 0;
    t5_err_t err = op_wait(ble_gattc_disc_all_svcs(s_conn, on_svc, NULL));
    if (err != T5_OK) return err;
    t5_mutex_lock(s_mtx);
    s_nchr = 0;
    t5_mutex_unlock(s_mtx);
    for (int i = 0; i < s_nsvc; i++) {
        if (!op_begin()) return T5_ERR_STATE;
        err = op_wait(ble_gattc_disc_all_chrs(s_conn, s_svcs[i].start, s_svcs[i].end, on_chr, (void *)(intptr_t)i));
        if (err != T5_OK) return err;
    }
    return T5_OK;
}

int t5_ble_chars(t5_ble_chr_t *out, int max)
{
    if (!s_mtx) return 0;
    t5_mutex_lock(s_mtx);
    int n = s_nchr < max ? s_nchr : max;
    memcpy(out, s_chr, (size_t)n * sizeof *out);
    t5_mutex_unlock(s_mtx);
    return n;
}

static int on_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)arg;
    if (err->status == 0 && attr) {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        s_op_len = n < s_op_cap ? n : s_op_cap;
        os_mbuf_copydata(attr->om, 0, (int)s_op_len, s_op_buf);
    }
    op_finish(err->status);
    return 0;
}

t5_err_t t5_ble_read(uint16_t handle, uint8_t *buf, size_t cap, size_t *len)
{
    if (!op_begin()) return T5_ERR_STATE;
    s_op_buf = buf;
    s_op_cap = cap;
    s_op_len = 0;
    t5_err_t err = op_wait(ble_gattc_read(s_conn, handle, on_read, NULL));
    if (len) *len = s_op_len;
    return err;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    op_finish(err->status);
    return 0;
}

t5_err_t t5_ble_write(uint16_t handle, const uint8_t *data, size_t len, bool response)
{
    if (!response) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) return T5_ERR_STATE;
        return ble_gattc_write_no_rsp_flat(s_conn, handle, data, (uint16_t)len) == 0 ? T5_OK : T5_ERR_FAIL;
    }
    if (!op_begin()) return T5_ERR_STATE;
    return op_wait(ble_gattc_write_flat(s_conn, handle, data, (uint16_t)len, on_write, NULL));
}

t5_err_t t5_ble_subscribe(uint16_t val_handle, bool on)
{
    const uint8_t v[2] = { on ? 1 : 0, 0 };
    return t5_ble_write((uint16_t)(val_handle + 1), v, 2, true);
}

void t5_ble_set_notify_cb(t5_ble_notify_cb_t cb, void *arg)
{
    s_notify_arg = arg;
    s_notify_cb = cb;
}
