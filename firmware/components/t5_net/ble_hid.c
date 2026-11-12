/* SPDX-License-Identifier: Apache-2.0
 * HID over GATT (the HID part of t5_net/ble.h): the Tab5 as a Bluetooth
 * keyboard, mouse, media remote, gamepad or absolute pointer.
 *
 * The services (HID, battery, device information) are registered with the
 * stack every time it starts, before any app's dynamic service, so their
 * handles are the same on every connection: a host caches a bonded
 * device's attribute table. For the same reason the report map never
 * changes: t5_hid's descriptor of every part. An app opening HID only
 * makes the device advertise as one and lets its reports through.
 *
 * Reports reach a host as notifications on its report characteristics,
 * which need an encrypted (paired) link; the host pairs when it first reads
 * them, and ble.c's pairing handler asks the person to confirm the code.
 */
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "t5_battery/battery.h"
#include "t5_core/event.h"
#include "t5_core/io_hold.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_hid/hid.h"
#include "t5_net/ble.h"
#include "ble_priv.h"

static const char *TAG = "t5.blehid";

#define HID_MAGIC 0x42484944u   /* "BHID" */
#define MAX_HOSTS 3
#define EVQ 16

/* Report slots: input reports by part, and the keyboard's LED output. */
enum { R_KBD, R_MOUSE, R_CC, R_PAD, R_ABS, R_KBD_OUT, R_COUNT };
static const struct {
    uint32_t part;
    uint8_t id, type, len;     /* type: 1 input, 2 output */
} SLOTS[R_COUNT] = {
    [R_KBD] = { T5_HID_PART_KEYBOARD, T5_HID_ID_KEYBOARD, 1, T5_HID_KEYBOARD_LEN },
    [R_MOUSE] = { T5_HID_PART_MOUSE, T5_HID_ID_MOUSE, 1, T5_HID_MOUSE_LEN },
    [R_CC] = { T5_HID_PART_CONSUMER, T5_HID_ID_CONSUMER, 1, T5_HID_CONSUMER_LEN },
    [R_PAD] = { T5_HID_PART_GAMEPAD, T5_HID_ID_GAMEPAD, 1, T5_HID_GAMEPAD_LEN },
    [R_ABS] = { T5_HID_PART_ABSOLUTE, T5_HID_ID_ABSOLUTE, 1, T5_HID_ABSOLUTE_LEN },
    [R_KBD_OUT] = { T5_HID_PART_KEYBOARD, T5_HID_ID_KEYBOARD, 2, T5_HID_KEYBOARD_OUT_LEN },
};

/* What an attribute's access callback is for (its arg). */
enum {
    A_INFO = 100, A_MAP, A_CTRL, A_PROTO, A_BATT, A_PNP, A_MFR,
    A_REF = 200,                /* + slot: a report's Report Reference */
};

typedef struct {
    t5_ble_hid_ev_t ev;
    uint8_t len;
    uint8_t data[8];
} hid_item_t;

struct t5_ble_hid {
    uint32_t magic;
    char app_id[32];
    bool app_owned;
    volatile bool revoked;
    QueueHandle_t q;
};

static struct {
    uint16_t conn;
    bool used, enc, told;
    uint8_t subs;              /* by slot: notifications on */
    uint8_t addr[6];
} s_hosts[MAX_HOSTS];

static t5_ble_hid_t *s_open;
static uint8_t s_map[T5_HID_DESC_MAX];
static int s_map_len;
static uint8_t s_last[R_COUNT][T5_HID_GAMEPAD_LEN];
static uint8_t s_proto = 1;    /* report protocol */
static uint16_t s_val[R_COUNT], s_batt_val;

/* ---- the attribute table ---------------------------------------------------- */
static int access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);

#define U16(v) BLE_UUID16_DECLARE(v)
#define REF_DSC(slot) \
    { { .uuid = U16(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, .access_cb = access, \
        .arg = (void *)(A_REF + (slot)) }, { 0 } }

static struct ble_gatt_dsc_def s_ref[R_COUNT][2] = {
    REF_DSC(R_KBD), REF_DSC(R_MOUSE), REF_DSC(R_CC), REF_DSC(R_PAD), REF_DSC(R_ABS), REF_DSC(R_KBD_OUT),
};

#define IN_REPORT(slot)                                                                                   \
    { .uuid = U16(0x2a4d), .access_cb = access, .arg = (void *)(slot), .descriptors = s_ref[slot],       \
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_val[slot] }

static const struct ble_gatt_chr_def s_hid_chrs[] = {
    { .uuid = U16(0x2a4a), .access_cb = access, .arg = (void *)A_INFO, .flags = BLE_GATT_CHR_F_READ },
    { .uuid = U16(0x2a4b), .access_cb = access, .arg = (void *)A_MAP,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
    { .uuid = U16(0x2a4c), .access_cb = access, .arg = (void *)A_CTRL, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = U16(0x2a4e), .access_cb = access, .arg = (void *)A_PROTO,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
    IN_REPORT(R_KBD),
    IN_REPORT(R_MOUSE),
    IN_REPORT(R_CC),
    IN_REPORT(R_PAD),
    IN_REPORT(R_ABS),
    { .uuid = U16(0x2a4d), .access_cb = access, .arg = (void *)R_KBD_OUT, .descriptors = s_ref[R_KBD_OUT],
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
               BLE_GATT_CHR_F_WRITE_NO_RSP,
      .val_handle = &s_val[R_KBD_OUT] },
    { 0 },
};

static const struct ble_gatt_chr_def s_batt_chrs[] = {
    { .uuid = U16(0x2a19), .access_cb = access, .arg = (void *)A_BATT,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_batt_val },
    { 0 },
};

static const struct ble_gatt_chr_def s_dis_chrs[] = {
    { .uuid = U16(0x2a50), .access_cb = access, .arg = (void *)A_PNP, .flags = BLE_GATT_CHR_F_READ },
    { .uuid = U16(0x2a29), .access_cb = access, .arg = (void *)A_MFR, .flags = BLE_GATT_CHR_F_READ },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x1812), .characteristics = s_hid_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x180f), .characteristics = s_batt_chrs },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x180a), .characteristics = s_dis_chrs },
    { 0 },
};

static int append(struct ble_gatt_access_ctxt *ctxt, const void *p, size_t n)
{
    return os_mbuf_append(ctxt->om, p, (uint16_t)n) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static void push_locked(const t5_ble_hid_ev_t *ev, const void *data, size_t len)
{
    if (!s_open) return;
    hid_item_t it = { .ev = *ev, .len = (uint8_t)(len > sizeof it.data ? sizeof it.data : len) };
    if (it.len) memcpy(it.data, data, it.len);
    xQueueSend(s_open->q, &it, 0);
}

static int host_index(uint16_t conn)
{
    for (int i = 0; i < MAX_HOSTS; i++) {
        if (s_hosts[i].used && s_hosts[i].conn == conn) return i;
    }
    return -1;
}

/* NimBLE's host task. */
static int access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    int a = (int)(intptr_t)arg;
    bool read = ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR || ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC;
    if (a >= A_REF) {
        int slot = a - A_REF;
        const uint8_t ref[2] = { SLOTS[slot].id, SLOTS[slot].type };
        return append(ctxt, ref, sizeof ref);
    }
    if (a < R_COUNT) {
        if (read) return append(ctxt, s_last[a], SLOTS[a].len);
        uint8_t buf[8];
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &n)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (a == R_KBD_OUT) {
            t5_mutex_lock(ble_mx());
            t5_ble_hid_ev_t ev = { .type = T5_BLE_HID_OUTPUT, .report_id = T5_HID_ID_KEYBOARD };
            int i = host_index(conn);
            if (i >= 0) memcpy(ev.addr, s_hosts[i].addr, 6);
            push_locked(&ev, buf, n);
            t5_mutex_unlock(ble_mx());
        }
        return 0;
    }
    switch (a) {
    case A_INFO: {
        const uint8_t info[4] = { 0x11, 0x01, 0x00, 0x02 };   /* HID 1.11, no country, normally connectable */
        return append(ctxt, info, sizeof info);
    }
    case A_MAP: return append(ctxt, s_map, (size_t)s_map_len);
    case A_PROTO:
        if (read) return append(ctxt, &s_proto, 1);
        ble_hs_mbuf_to_flat(ctxt->om, &s_proto, 1, NULL);
        return 0;
    case A_CTRL: {
        uint8_t v = 0;
        ble_hs_mbuf_to_flat(ctxt->om, &v, 1, NULL);
        t5_mutex_lock(ble_mx());
        t5_ble_hid_ev_t ev = { .type = T5_BLE_HID_SUSPEND, .on = v == 0 };
        int i = host_index(conn);
        if (i >= 0) memcpy(ev.addr, s_hosts[i].addr, 6);
        push_locked(&ev, NULL, 0);
        t5_mutex_unlock(ble_mx());
        return 0;
    }
    case A_BATT: {
        int p = t5_battery_percent();
        uint8_t v = (uint8_t)(p < 0 ? 100 : p > 100 ? 100 : p);
        return append(ctxt, &v, 1);
    }
    case A_PNP: {
        /* USB-IF vendor source, Espressif's VID, the HID PID, version 1.0 */
        const uint8_t pnp[7] = { 0x02, 0x3a, 0x30, 0x04, 0x40, 0x00, 0x01 };
        return append(ctxt, pnp, sizeof pnp);
    }
    case A_MFR: return append(ctxt, "Tab5 OS", 7);
    default: return BLE_ATT_ERR_UNLIKELY;
    }
}

int ble_hid_register(void)
{
    if (!s_map_len) s_map_len = t5_hid_descriptor(T5_HID_PART_ALL, s_map, sizeof s_map);
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    return rc;
}

/* ---- hosts ------------------------------------------------------------------ */
static void maybe_connected_locked(int i)
{
    if (s_hosts[i].told || !s_hosts[i].enc || !(s_hosts[i].subs & (1u << R_KBD | 1u << R_MOUSE))) return;
    s_hosts[i].told = true;
    t5_ble_hid_ev_t ev = { .type = T5_BLE_HID_CONNECTED };
    memcpy(ev.addr, s_hosts[i].addr, 6);
    push_locked(&ev, NULL, 0);
    T5_LOGI(TAG, "host %02x:%02x:%02x:%02x:%02x:%02x listening", ev.addr[5], ev.addr[4], ev.addr[3], ev.addr[2],
            ev.addr[1], ev.addr[0]);
}

void ble_hid_gap_locked(const struct ble_gap_event *e)
{
    struct ble_gap_conn_desc d;
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (e->connect.status != 0) return;
        for (int i = 0; i < MAX_HOSTS; i++) {
            if (s_hosts[i].used) continue;
            s_hosts[i] = (typeof(s_hosts[i])){ .conn = e->connect.conn_handle, .used = true };
            if (ble_gap_conn_find(e->connect.conn_handle, &d) == 0) {
                memcpy(s_hosts[i].addr, d.peer_id_addr.val, 6);
                s_hosts[i].enc = d.sec_state.encrypted;
            }
            break;
        }
        return;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        int i = host_index(e->disconnect.conn.conn_handle);
        if (i < 0) return;
        if (s_hosts[i].told) {
            t5_ble_hid_ev_t ev = { .type = T5_BLE_HID_DISCONNECTED };
            memcpy(ev.addr, s_hosts[i].addr, 6);
            push_locked(&ev, NULL, 0);
        }
        s_hosts[i].used = false;
        return;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        int i = host_index(e->enc_change.conn_handle);
        if (i < 0 || e->enc_change.status != 0) return;
        if (ble_gap_conn_find(e->enc_change.conn_handle, &d) == 0) {
            s_hosts[i].enc = d.sec_state.encrypted;
            memcpy(s_hosts[i].addr, d.peer_id_addr.val, 6);   /* the identity address, now known */
        }
        maybe_connected_locked(i);
        return;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: {
        int i = host_index(e->subscribe.conn_handle);
        if (i < 0) return;
        for (int s = 0; s < R_COUNT; s++) {
            if (s_val[s] != e->subscribe.attr_handle) continue;
            if (e->subscribe.cur_notify) s_hosts[i].subs |= (uint8_t)(1u << s);
            else s_hosts[i].subs &= (uint8_t)~(1u << s);
        }
        maybe_connected_locked(i);
        return;
    }
    default: return;
    }
}

void ble_hid_down_locked(void)
{
    for (int i = 0; i < MAX_HOSTS; i++) {
        if (s_hosts[i].used && s_hosts[i].told) {
            t5_ble_hid_ev_t ev = { .type = T5_BLE_HID_DISCONNECTED };
            memcpy(ev.addr, s_hosts[i].addr, 6);
            push_locked(&ev, NULL, 0);
        }
        s_hosts[i].used = false;
    }
}

/* ---- sending ---------------------------------------------------------------- */
static int slot_of(uint32_t part)
{
    for (int s = 0; s < R_KBD_OUT; s++) {
        if (SLOTS[s].part == part) return s;
    }
    return -1;
}

static t5_err_t notify_all(int slot, const void *report, uint32_t timeout_ms)
{
    uint16_t conns[MAX_HOSTS];
    int n = 0;
    t5_mutex_lock(ble_mx());
    memcpy(s_last[slot], report, SLOTS[slot].len);
    for (int i = 0; i < MAX_HOSTS; i++) {
        if (s_hosts[i].used && s_hosts[i].enc && (s_hosts[i].subs & (1u << slot))) conns[n++] = s_hosts[i].conn;
    }
    uint16_t h = s_val[slot];
    t5_mutex_unlock(ble_mx());
    if (!n || !ble_synced()) return T5_ERR_STATE;
    int64_t until = t5_time_us() + (int64_t)timeout_ms * 1000;
    t5_err_t e = T5_OK;
    for (int k = 0; k < n; k++) {
        for (;;) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(report, SLOTS[slot].len);
            int rc = om ? ble_gatts_notify_custom(conns[k], h, om) : BLE_HS_ENOMEM;
            if (rc == 0) break;
            if ((rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) || t5_time_us() >= until) {
                e = rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY ? T5_ERR_TIMEOUT : T5_ERR_IO;
                break;
            }
            vTaskDelay(1);
        }
    }
    return e;
}

static bool valid(const t5_ble_hid_t *h)
{
    return h && h->magic == HID_MAGIC;
}

t5_err_t t5_ble_hid_send(t5_ble_hid_t *h, uint32_t part, const void *report, size_t len, uint32_t timeout_ms)
{
    int slot = slot_of(part);
    if (!valid(h) || slot < 0 || !report || len != SLOTS[slot].len) return T5_ERR_INVALID_ARG;
    if (h->revoked || !t5_core_app_foreground()) return T5_ERR_STATE;
    return notify_all(slot, report, timeout_ms);
}

bool t5_ble_hid_connected(t5_ble_hid_t *h)
{
    if (!valid(h) || h->revoked) return false;
    bool any = false;
    t5_mutex_lock(ble_mx());
    for (int i = 0; i < MAX_HOSTS; i++) any |= s_hosts[i].used && s_hosts[i].told;
    t5_mutex_unlock(ble_mx());
    return any;
}

/* Every input report back to all-released: nothing stays held on a host. */
static void release_all(void)
{
    static const uint8_t zero[T5_HID_GAMEPAD_LEN];
    for (int s = 0; s < R_KBD_OUT; s++) {
        uint8_t r[T5_HID_GAMEPAD_LEN];
        memcpy(r, zero, sizeof r);
        if (s == R_PAD) r[8] = T5_HID_HAT_NONE;
        T5_IGNORE(notify_all(s, r, 50));
    }
}

static void on_app_event(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)id; (void)arg;
    const t5_ev_app_t *ev = data;
    t5_mutex_lock(ble_mx());
    bool mine = s_open && s_open->app_owned && len >= sizeof *ev && !strncmp(ev->id, s_open->app_id, sizeof ev->id);
    t5_mutex_unlock(ble_mx());
    if (mine) release_all();
}

/* ---- open and close --------------------------------------------------------- */
static void revoke(void)
{
    t5_mutex_lock(ble_mx());
    t5_ble_hid_t *h = s_open;
    if (h && !h->revoked) {
        h->revoked = true;
        s_open = NULL;
        ble_adv_sys_stop();
        ble_svc_gap_device_appearance_set(0);
        t5_io_hold_set(T5_IO_BLE_HID, NULL, NULL);
        T5_LOGI(TAG, "HID stopped for %s", h->app_owned ? h->app_id : "the system");
    }
    t5_mutex_unlock(ble_mx());
}

/* The person's Stop (Quick Settings): let go of every key first. */
static void stop_for_person(void)
{
    release_all();
    revoke();
}

static void release_handle(void *h)
{
    T5_IGNORE(t5_ble_hid_close(h));
}

t5_err_t t5_ble_hid_open(const t5_ble_hid_cfg_t *cfg, t5_ble_hid_t **out)
{
    if (!cfg || !out || cfg->size < sizeof *cfg) return T5_ERR_INVALID_ARG;
    if (cfg->name && (!cfg->name[0] || strlen(cfg->name) > 20)) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!ble_mx()) return T5_ERR_STATE;
    int pe = t5_core_perm_check(T5_CORE_CAP_BLE_HID);
    if (pe != T5_OK) return (t5_err_t)pe;
    static bool s_subscribed;
    if (!s_subscribed) s_subscribed = t5_event_subscribe(T5_EV_APP_SUSPENDED, on_app_event, NULL) == T5_OK;

    t5_ble_hid_t *h = calloc(1, sizeof *h);
    if (!h) return T5_ERR_NO_MEM;
    h->magic = HID_MAGIC;
    h->q = xQueueCreate(EVQ, sizeof(hid_item_t));
    const t5_app_ctx_hdr_t *app = t5_app_ctx_get();
    char who[32] = "";
    if (app && app->magic == T5_APP_CTX_MAGIC) {
        h->app_owned = true;
        snprintf(h->app_id, sizeof h->app_id, "%s", app->id);
        snprintf(who, sizeof who, "%s", app->name && app->name[0] ? app->name : app->id);
    }
    if (!h->q) {
        free(h);
        return T5_ERR_NO_MEM;
    }
    t5_mutex_lock(ble_mx());
    t5_err_t e = s_open ? T5_ERR_BUSY : T5_OK;
    uint16_t appearance = cfg->appearance ? cfg->appearance : T5_BLE_APPEARANCE_HID;
    if (e == T5_OK) e = ble_adv_sys_start(cfg->name ? cfg->name : "Tab5", appearance, "0x1812");
    if (e == T5_OK) {
        ble_svc_gap_device_appearance_set(appearance);
        memset(s_last, 0, sizeof s_last);
        s_last[R_PAD][8] = T5_HID_HAT_NONE;
        s_open = h;
        /* Hosts already connected and listening (paired before) count now. */
        for (int i = 0; i < MAX_HOSTS; i++) {
            if (s_hosts[i].used && s_hosts[i].told) {
                t5_ble_hid_ev_t ev = { .type = T5_BLE_HID_CONNECTED };
                memcpy(ev.addr, s_hosts[i].addr, 6);
                push_locked(&ev, NULL, 0);
            }
        }
    }
    t5_mutex_unlock(ble_mx());
    if (e != T5_OK) {
        vQueueDelete(h->q);
        free(h);
        return e;
    }
    t5_io_hold_set(T5_IO_BLE_HID, who, stop_for_person);
    t5_res_add(T5_RES_OTHER, h, release_handle);
    T5_LOGI(TAG, "HID open (appearance 0x%04x)", appearance);
    *out = h;
    return T5_OK;
}

int t5_ble_hid_next(t5_ble_hid_t *h, t5_ble_hid_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (!valid(h) || !ev || (cap && !buf)) return T5_ERR_INVALID_ARG;
    hid_item_t it;
    TickType_t t = timeout_ms == T5_BLE_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(h->q, &it, t) != pdTRUE) return h->revoked ? T5_ERR_STATE : T5_ERR_TIMEOUT;
    *ev = it.ev;
    size_t k = it.len < cap ? it.len : cap;
    if (k) memcpy(buf, it.data, k);
    return (int)k;
}

t5_err_t t5_ble_hid_close(t5_ble_hid_t *h)
{
    if (!valid(h)) return T5_ERR_INVALID_ARG;
    t5_res_remove(h);
    t5_mutex_lock(ble_mx());
    bool mine = s_open == h;
    t5_mutex_unlock(ble_mx());
    if (mine) {
        release_all();
        revoke();
    }
    h->magic = 0;
    vQueueDelete(h->q);
    free(h);
    return T5_OK;
}
