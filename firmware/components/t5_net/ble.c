/* SPDX-License-Identifier: Apache-2.0
 * Bluetooth LE on NimBLE, controller on the C6 (esp_hosted HCI): the app
 * API (t5_net/ble.h) and the system's (t5_net/ble_sys.h) on one layer.
 *
 * Threads: NimBLE calls the GAP and GATT callbacks on its host task. They
 * reach scanners, links and servers only through the tables below, under
 * s_mx, and the API removes an object from its table (under s_mx) before
 * freeing it, so a callback that arrives late finds nothing. Blocking API
 * calls wait on per-object semaphores the callbacks give.
 *
 * Ownership: an app's scanner, link, advertiser or server is its resource
 * (t5_res), closed when it exits; at most T5_BLE_APP_MAX per app. While any
 * app handle is open the stack holds a NET wake lock ("ble.io"), and while
 * a link is up another ("ble"), so Sleep keeps the C6 up.
 */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "esp_hosted_bt_host_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_net/ble_sys.h"
#include "t5_net/net.h"
#include "t5_power/power.h"
#include "esp_random.h"
#include "host/ble_store.h"
#include "ble_priv.h"
#include "net_priv.h"

static const char *TAG = "t5.ble";

/* NimBLE's NVS bond store; no header declares it (IDF's examples do this too). */
void ble_store_config_init(void);

#define SYNC_WAIT_MS 3000
#define CLOSE_WAIT_MS 2000
#define SCAN_QUEUE 32
#define NOTE_QUEUE 16
#define SERVER_QUEUE 16
#define MAX_SVCS 16
#define MAX_SERVERS 4
#define MAX_OWNERS 8
#define DEVICE_NAME "Tab5"
#define SCAN_MAGIC 0x42534341u
#define LINK_MAGIC 0x424c4e4bu
#define ADV_MAGIC 0x42414456u
#define SRV_MAGIC 0x42535256u

/* ---- stack state -------------------------------------------------------- */
static bool s_enabled;          /* the user wants BLE */
static bool s_link_up;          /* the C6 link is up */
static volatile bool s_running; /* stack started */
static volatile bool s_synced;  /* host and controller in sync */
static SemaphoreHandle_t s_sync;
static uint8_t s_own_addr_type;
static t5_mutex_t s_mx;
static t5_wakelock_t *s_link_lock, *s_io_lock;

static void post(t5_event_id_t id, int32_t err, int count)
{
    t5_ev_net_t ev = { .err = err, .count = (uint8_t)count };
    t5_event_post(id, &ev, sizeof ev);
}

static t5_err_t map_rc(int rc)
{
    switch (rc) {
    case 0: return T5_OK;
    case BLE_HS_ENOTCONN: return T5_ERR_STATE;
    case BLE_HS_ETIMEOUT: return T5_ERR_TIMEOUT;
    case BLE_HS_ENOMEM: return T5_ERR_NO_MEM;
    case BLE_HS_EALREADY:
    case BLE_HS_EBUSY: return T5_ERR_BUSY;
    case BLE_HS_EINVAL: return T5_ERR_INVALID_ARG;
    default: return T5_ERR_IO;
    }
}

static TickType_t ticks(uint32_t ms)
{
    return ms == T5_BLE_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(ms);
}

/* ---- per-app handle accounting and the io wake lock ---------------------- */
static struct {
    void *app;
    int n;
} s_owners[MAX_OWNERS];

static int app_handles_locked(void)
{
    int n = 0;
    for (int i = 0; i < MAX_OWNERS; i++) n += s_owners[i].app ? s_owners[i].n : 0;
    return n;
}

static void io_lock_update_locked(void)
{
    bool want = app_handles_locked() > 0;
    if (want && !s_io_lock) s_io_lock = t5_wakelock_acquire_system(T5_LOCK_NET, "ble.io");
    if (!want && s_io_lock) {
        t5_wakelock_release(s_io_lock);
        s_io_lock = NULL;
    }
}

/* Count a handle for the calling app (system callers aren't limited). */
static bool owner_take_locked(void *app)
{
    if (!app) return true;
    int at = -1;
    for (int i = 0; i < MAX_OWNERS; i++) {
        if (s_owners[i].n && s_owners[i].app == app) {
            at = i;
            break;
        }
        if (at < 0 && !s_owners[i].n) at = i;
    }
    if (at < 0 || (s_owners[at].app == app && s_owners[at].n >= T5_BLE_APP_MAX)) return false;
    s_owners[at].app = app;
    s_owners[at].n++;
    io_lock_update_locked();
    return true;
}

static void owner_give_locked(void *app)
{
    if (!app) return;
    for (int i = 0; i < MAX_OWNERS; i++) {
        if (s_owners[i].n && s_owners[i].app == app) {
            if (!--s_owners[i].n) s_owners[i].app = NULL;
            break;
        }
    }
    io_lock_update_locked();
}

/* ---- links -------------------------------------------------------------- */
typedef struct {
    uint16_t handle;
    uint16_t len;
    uint8_t data[T5_BLE_VALUE_MAX];
} note_t;

struct t5_ble_conn {
    uint32_t magic;
    void *app;
    volatile uint16_t handle;           /* BLE_HS_CONN_HANDLE_NONE while not up */
    volatile bool up;
    volatile bool connecting;
    bool central;                       /* we connected out; else a central reached our server */
    bool sys;                           /* Settings' link: freed when it drops */
    uint8_t addr[6];
    uint8_t addr_type;
    SemaphoreHandle_t done;             /* connect result, disconnect, or a GATT op */
    volatile int status;
    SemaphoreHandle_t op_lock;          /* one GATT operation at a time */
    int slot;                           /* in s_links */
    uint32_t op_seq;                    /* the operation callbacks may finish (OP_ARG) */
    bool op_active;
    uint8_t *op_buf;                    /* a read's result, NULL once its caller has gone */
    size_t op_cap, op_len;
    int disc_svc;                       /* the service being walked */
    struct {
        uint16_t start, end;
        char uuid[T5_BLE_UUID_LEN];
    } svcs[MAX_SVCS];
    int nsvc;
    t5_ble_chr_t *chrs;                 /* T5_BLE_MAX_CHARS, PSRAM */
    int nchr;
    QueueHandle_t notes;
};

static t5_ble_conn_t *s_links[T5_BLE_MAX_LINKS];
static t5_ble_conn_t *s_sys_link;

static bool link_valid_locked(const t5_ble_conn_t *c)
{
    for (int i = 0; i < T5_BLE_MAX_LINKS; i++) {
        if (c && s_links[i] == c) return true;
    }
    return false;
}

static int links_up_locked(void)
{
    int n = 0;
    for (int i = 0; i < T5_BLE_MAX_LINKS; i++) n += s_links[i] && s_links[i]->up;
    return n;
}

static void link_lock_update_locked(void)
{
    bool want = links_up_locked() > 0;
    if (want && !s_link_lock) s_link_lock = t5_wakelock_acquire_system(T5_LOCK_NET, "ble");
    if (!want && s_link_lock) {
        t5_wakelock_release(s_link_lock);
        s_link_lock = NULL;
    }
}

static t5_ble_conn_t *link_by_handle_locked(uint16_t h)
{
    for (int i = 0; i < T5_BLE_MAX_LINKS; i++) {
        if (s_links[i] && s_links[i]->up && s_links[i]->handle == h) return s_links[i];
    }
    return NULL;
}

static void link_free(t5_ble_conn_t *c)
{
    if (!c) return;
    c->magic = 0;
    if (c->done) vSemaphoreDelete(c->done);
    if (c->op_lock) vSemaphoreDelete(c->op_lock);
    if (c->notes) vQueueDeleteWithCaps(c->notes);
    free(c->chrs);
    free(c);
}

/* A link record in the table (NULL: the table is full or no memory). */
static t5_ble_conn_t *link_new_locked(void *app, bool central)
{
    int slot = -1;
    for (int i = 0; i < T5_BLE_MAX_LINKS && slot < 0; i++) {
        if (!s_links[i]) slot = i;
    }
    if (slot < 0) return NULL;
    t5_ble_conn_t *c = heap_caps_calloc(1, sizeof *c, MALLOC_CAP_SPIRAM);
    if (!c) return NULL;
    c->magic = LINK_MAGIC;
    c->app = app;
    c->central = central;
    c->handle = BLE_HS_CONN_HANDLE_NONE;
    c->done = xSemaphoreCreateBinary();
    if (central) {
        c->op_lock = xSemaphoreCreateRecursiveMutex();
        c->chrs = heap_caps_calloc(T5_BLE_MAX_CHARS, sizeof *c->chrs, MALLOC_CAP_SPIRAM);
        c->notes = xQueueCreateWithCaps(NOTE_QUEUE, sizeof(note_t), MALLOC_CAP_SPIRAM);
    }
    if (!c->done || (central && (!c->op_lock || !c->chrs || !c->notes))) {
        link_free(c);
        return NULL;
    }
    c->slot = slot;
    s_links[slot] = c;
    return c;
}

static void link_remove_locked(t5_ble_conn_t *c)
{
    for (int i = 0; i < T5_BLE_MAX_LINKS; i++) {
        if (s_links[i] == c) s_links[i] = NULL;
    }
    if (s_sys_link == c) s_sys_link = NULL;
}

/* ---- servers (declared here: the advertiser's events reach them) --------- */
typedef struct {
    t5_ble_server_ev_t ev;
    uint16_t len;
    uint8_t data[T5_BLE_VALUE_MAX];
} srv_item_t;

struct t5_ble_server {
    uint32_t magic;
    void *app;
    int slot;
    int n;
    bool registered;
    ble_uuid_any_t svc_uuid;
    ble_uuid_any_t chr_uuid[T5_BLE_SERVER_MAX_CHARS];
    struct ble_gatt_svc_def svc[2];
    struct ble_gatt_chr_def chr[T5_BLE_SERVER_MAX_CHARS + 1];
    uint16_t val_handle[T5_BLE_SERVER_MAX_CHARS];
    uint8_t props[T5_BLE_SERVER_MAX_CHARS];
    struct {
        uint16_t len;
        uint8_t data[T5_BLE_VALUE_MAX];
    } val[T5_BLE_SERVER_MAX_CHARS];
    QueueHandle_t events;
};

static t5_ble_server_t *s_servers[MAX_SERVERS];

static void servers_post_locked(t5_ble_server_ev_type_t type, uint16_t peer, const uint8_t addr[6])
{
    srv_item_t it = { .ev = { .type = type, .chr = -1, .peer = peer } };
    if (addr) memcpy(it.ev.addr, addr, 6);
    for (int i = 0; i < MAX_SERVERS; i++) {
        if (s_servers[i]) xQueueSend(s_servers[i]->events, &it, 0);
    }
}

/* ---- the advertiser ------------------------------------------------------ */
struct t5_ble_advertiser {
    uint32_t magic;
    void *app;
    t5_ble_adv_cfg_t cfg;               /* pointers below are the copies */
    char name[T5_BLE_NAME_LEN];
    uint8_t mfr[31];
    ble_uuid_any_t uuid;
    bool has_uuid;
    uint16_t appearance;                /* the system's (HID): 0 for none */
};

static t5_ble_advertiser_t *s_adv;
static int adv_event(struct ble_gap_event *e, void *arg);

static void adv_fields(const t5_ble_advertiser_t *a, struct ble_hs_adv_fields *f, bool with_name)
{
    memset(f, 0, sizeof *f);
    f->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    if (a->appearance) {
        f->appearance = a->appearance;
        f->appearance_is_present = 1;
    }
    if (with_name && a->name[0]) {
        f->name = (const uint8_t *)a->name;
        f->name_len = (uint8_t)strlen(a->name);
        f->name_is_complete = 1;
    }
    if (a->cfg.mfr_len) {
        f->mfg_data = a->mfr;
        f->mfg_data_len = a->cfg.mfr_len;
    }
    if (a->has_uuid && a->uuid.u.type == BLE_UUID_TYPE_16) {
        f->uuids16 = &a->uuid.u16;
        f->num_uuids16 = 1;
        f->uuids16_is_complete = 1;
    } else if (a->has_uuid && a->uuid.u.type == BLE_UUID_TYPE_128) {
        f->uuids128 = &a->uuid.u128;
        f->num_uuids128 = 1;
        f->uuids128_is_complete = 1;
    }
}

/* Bytes the advertising packet needs (flags, mfr data, uuid, name). */
static size_t adv_size(const t5_ble_advertiser_t *a, bool with_name)
{
    size_t n = 3 + (a->cfg.mfr_len ? 2u + a->cfg.mfr_len : 0u) + (a->appearance ? 4u : 0u);
    if (a->has_uuid) n += a->uuid.u.type == BLE_UUID_TYPE_16 ? 4u : 18u;
    if (with_name && a->name[0]) n += 2 + strlen(a->name);
    return n;
}

/* On the air with a's settings, if the stack is up and a link is free. */
static int adv_apply_locked(t5_ble_advertiser_t *a)
{
    if (!a || !s_synced) return 0;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    if (a->cfg.connectable) {
        int free_slots = 0;
        for (int i = 0; i < T5_BLE_MAX_LINKS; i++) free_slots += !s_links[i];
        if (!free_slots) return 0;          /* back on the air when one closes */
    }
    bool name_in_adv = adv_size(a, true) <= 31;
    struct ble_hs_adv_fields f;
    adv_fields(a, &f, name_in_adv);
    int rc = ble_gap_adv_set_fields(&f);
    if (rc == 0 && !name_in_adv && a->name[0]) {
        struct ble_hs_adv_fields r = { .name = (const uint8_t *)a->name, .name_len = (uint8_t)strlen(a->name),
                                       .name_is_complete = 1 };
        rc = ble_gap_adv_rsp_set_fields(&r);
    }
    if (rc) return rc;
    uint32_t ms = a->cfg.interval_ms ? a->cfg.interval_ms : 100;
    uint16_t itvl = (uint16_t)(ms * 1000 / 625);
    struct ble_gap_adv_params p = {
        .conn_mode = a->cfg.connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = itvl,
        .itvl_max = itvl,
    };
    return ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, adv_event, NULL);
}

/* Pairing and bonding, for every link. The person confirms a code on the
 * Tab5 (the shell's dialog, through T5_EV_BLE_PAIRING) or reads one to
 * type on the other device; a device that wants the Tab5 to type one is
 * refused. True with *rc set when the event is handled here. */
static bool sec_event(struct ble_gap_event *e, int *rc)
{
    *rc = 0;
    struct ble_gap_conn_desc d;
    switch (e->type) {
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        t5_ev_ble_pair_t ev = { .conn = e->passkey.conn_handle };
        if (ble_gap_conn_find(ev.conn, &d) == 0) memcpy(ev.addr, d.peer_ota_addr.val, 6);
        if (e->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            ev.action = 1;
            ev.passkey = e->passkey.params.numcmp;
            t5_event_post(T5_EV_BLE_PAIRING, &ev, sizeof ev);
        } else if (e->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000 };
            ev.action = 2;
            ev.passkey = io.passkey;
            ble_sm_inject_io(ev.conn, &io);
            t5_event_post(T5_EV_BLE_PAIRING, &ev, sizeof ev);
        } else {
            ble_gap_terminate(ev.conn, BLE_ERR_AUTH_FAIL);
        }
        return true;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The other device forgot us and pairs again: drop the old bond. */
        if (ble_gap_conn_find(e->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        *rc = BLE_GAP_REPEAT_PAIRING_RETRY;
        return true;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        t5_ev_ble_pair_t ev = { .conn = e->enc_change.conn_handle,
                                .err = e->enc_change.status ? T5_ERR_DENIED : T5_OK };
        if (ble_gap_conn_find(ev.conn, &d) == 0) memcpy(ev.addr, d.peer_id_addr.val, 6);
        t5_event_post(T5_EV_BLE_PAIRED, &ev, sizeof ev);
        return false;                       /* HID wants it too */
    }
    default: return false;
    }
}

/* GAP events for links centrals made to us, through the advertiser. */
static int adv_event(struct ble_gap_event *e, void *arg)
{
    (void)arg;
    int rc;
    if (sec_event(e, &rc)) return rc;
    t5_mutex_lock(s_mx);
    ble_hid_gap_locked(e);
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            t5_ble_conn_t *c = link_new_locked(NULL, false);
            struct ble_gap_conn_desc d;
            if (!c) {
                ble_gap_terminate(e->connect.conn_handle, BLE_ERR_CONN_LIMIT);
            } else {
                c->handle = e->connect.conn_handle;
                c->up = true;
                if (ble_gap_conn_find(c->handle, &d) == 0) {
                    memcpy(c->addr, d.peer_id_addr.val, 6);
                    c->addr_type = d.peer_id_addr.type;
                }
                link_lock_update_locked();
                servers_post_locked(T5_BLE_SERVER_CONNECTED, c->handle, c->addr);
                post(T5_EV_BLE_CONNECTED, T5_OK, links_up_locked());
            }
        }
        adv_apply_locked(s_adv);            /* room for another central? */
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        t5_ble_conn_t *c = link_by_handle_locked(e->disconnect.conn.conn_handle);
        if (c) {
            servers_post_locked(T5_BLE_SERVER_DISCONNECTED, c->handle, c->addr);
            link_remove_locked(c);
            link_free(c);
        }
        link_lock_update_locked();
        post(T5_EV_BLE_DISCONNECTED, e->disconnect.reason, links_up_locked());
        adv_apply_locked(s_adv);
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        for (int i = 0; i < MAX_SERVERS; i++) {
            t5_ble_server_t *s = s_servers[i];
            for (int k = 0; s && k < s->n; k++) {
                if (s->val_handle[k] != e->subscribe.attr_handle) continue;
                srv_item_t it = { .ev = { .type = T5_BLE_SERVER_SUBSCRIBED, .chr = k,
                                          .peer = e->subscribe.conn_handle,
                                          .on = e->subscribe.cur_notify || e->subscribe.cur_indicate } };
                t5_ble_conn_t *c = link_by_handle_locked(e->subscribe.conn_handle);
                if (c) memcpy(it.ev.addr, c->addr, 6);
                xQueueSend(s->events, &it, 0);
            }
        }
        break;
    default:
        break;
    }
    t5_mutex_unlock(s_mx);
    return 0;
}

/* ---- scanning ------------------------------------------------------------ */
struct t5_ble_scanner {
    uint32_t magic;
    void *app;
    bool sys;                           /* Settings' scan: keeps s_devs */
    volatile bool ended;
    QueueHandle_t q;
};

static t5_ble_scanner_t *s_scanner;
static t5_ble_dev_t s_devs[T5_BLE_MAX_SCAN];
static int s_ndevs;

static void sys_add_locked(const t5_ble_report_t *r)
{
    int i = 0;
    while (i < s_ndevs && memcmp(s_devs[i].addr, r->addr, 6)) i++;
    if (i == s_ndevs) {
        if (s_ndevs == T5_BLE_MAX_SCAN) return;
        memset(&s_devs[i], 0, sizeof s_devs[i]);
        memcpy(s_devs[i].addr, r->addr, 6);
        s_devs[i].addr_type = r->addr_type;
        s_ndevs++;
    }
    s_devs[i].rssi = r->rssi;
    if (r->name[0]) memcpy(s_devs[i].name, r->name, sizeof s_devs[i].name);
}

static int scan_event(struct ble_gap_event *e, void *arg)
{
    (void)arg;
    t5_mutex_lock(s_mx);
    t5_ble_scanner_t *s = s_scanner;
    if (s && e->type == BLE_GAP_EVENT_DISC) {
        t5_ble_report_t r = { .addr_type = e->disc.addr.type, .rssi = e->disc.rssi };
        memcpy(r.addr, e->disc.addr.val, 6);
        r.connectable = e->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                        e->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
        r.scan_rsp = e->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
        r.data_len = e->disc.length_data > sizeof r.data ? sizeof r.data : e->disc.length_data;
        memcpy(r.data, e->disc.data, r.data_len);
        t5_ble_adv_name(r.data, r.data_len, r.name, sizeof r.name);
        if (s->sys) sys_add_locked(&r);
        else xQueueSend(s->q, &r, 0);
    } else if (s && e->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        s->ended = true;
        if (s->sys) post(T5_EV_BLE_SCAN_DONE, T5_OK, s_ndevs);
    }
    t5_mutex_unlock(s_mx);
    return 0;
}

static t5_err_t scan_begin_locked(t5_ble_scanner_t *s, const t5_ble_scan_cfg_t *cfg)
{
    if (!s_synced) return T5_ERR_STATE;
    if (s_scanner && !s_scanner->ended) return T5_ERR_BUSY;
    struct ble_gap_disc_params p = { .passive = !cfg->active, .filter_duplicates = !cfg->duplicates };
    int32_t dur = cfg->duration_ms ? (int32_t)cfg->duration_ms : BLE_HS_FOREVER;
    if (s_scanner) ble_gap_disc_cancel();          /* an ended one: make sure */
    s->ended = false;
    s_scanner = s;
    int rc = ble_gap_disc(s_own_addr_type, dur, &p, scan_event, NULL);
    if (rc) s_scanner = NULL;
    return map_rc(rc);
}

static void scan_release(void *h)
{
    T5_IGNORE(t5_ble_scan_stop(h));        /* exit cleanup: nothing to report to */
}

t5_err_t t5_ble_scan_start(const t5_ble_scan_cfg_t *cfg, t5_ble_scanner_t **out)
{
    if (!cfg || !out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!s_mx) return T5_ERR_STATE;
    t5_ble_scanner_t *s = heap_caps_calloc(1, sizeof *s, MALLOC_CAP_SPIRAM);
    if (!s) return T5_ERR_NO_MEM;
    s->magic = SCAN_MAGIC;
    s->app = t5_app_ctx_get();
    s->q = xQueueCreateWithCaps(SCAN_QUEUE, sizeof(t5_ble_report_t), MALLOC_CAP_SPIRAM);
    t5_mutex_lock(s_mx);
    t5_err_t e = !s->q ? T5_ERR_NO_MEM : owner_take_locked(s->app) ? T5_OK : T5_ERR_BUSY;
    if (e == T5_OK) {
        e = scan_begin_locked(s, cfg);
        if (e != T5_OK) owner_give_locked(s->app);
        else if (s->app) t5_res_add(T5_RES_OTHER, s, scan_release);
    }
    t5_mutex_unlock(s_mx);
    if (e != T5_OK) {
        if (s->q) vQueueDeleteWithCaps(s->q);
        free(s);
        return e;
    }
    *out = s;
    return T5_OK;
}

t5_err_t t5_ble_scan_next(t5_ble_scanner_t *s, t5_ble_report_t *out, uint32_t timeout_ms)
{
    if (!s || s->magic != SCAN_MAGIC || !out) return T5_ERR_INVALID_ARG;
    /* Wait in short steps so the end of the scan is noticed. */
    uint32_t t0 = t5_time_ms();
    for (;;) {
        if (xQueueReceive(s->q, out, 0) == pdTRUE) return T5_OK;
        if (s->ended || !s_synced) return T5_ERR_STATE;
        uint32_t spent = t5_time_ms() - t0;
        if (timeout_ms != T5_BLE_FOREVER && spent >= timeout_ms) return T5_ERR_TIMEOUT;
        uint32_t step = timeout_ms == T5_BLE_FOREVER || timeout_ms - spent > 100 ? 100 : timeout_ms - spent;
        if (xQueueReceive(s->q, out, pdMS_TO_TICKS(step)) == pdTRUE) return T5_OK;
    }
}

t5_err_t t5_ble_scan_stop(t5_ble_scanner_t *s)
{
    if (!s || s->magic != SCAN_MAGIC) return T5_ERR_INVALID_ARG;
    t5_res_remove(s);
    t5_mutex_lock(s_mx);
    if (s_scanner == s) {
        s_scanner = NULL;
        if (s_synced) ble_gap_disc_cancel();
    }
    owner_give_locked(s->app);
    s->magic = 0;
    t5_mutex_unlock(s_mx);
    vQueueDeleteWithCaps(s->q);
    free(s);
    return T5_OK;
}

/* ---- the GATT client ------------------------------------------------------ */
static int link_event(struct ble_gap_event *e, void *arg);

/* GATT callbacks get the link's slot and the operation's number, not a
 * pointer: one that comes after its caller timed out finds a different
 * number (or no link) and is ignored. */
#define OP_ARG(c) ((void *)(uintptr_t)(((uint32_t)(c)->slot << 24) | ((c)->op_seq & 0xffffffu)))

static t5_ble_conn_t *op_link_locked(void *arg)
{
    uint32_t v = (uint32_t)(uintptr_t)arg;
    uint32_t slot = v >> 24;
    t5_ble_conn_t *c = slot < T5_BLE_MAX_LINKS ? s_links[slot] : NULL;
    return c && c->op_active && (c->op_seq & 0xffffffu) == (v & 0xffffffu) ? c : NULL;
}

static void op_finish_locked(t5_ble_conn_t *c, int status)
{
    c->status = status;
    c->op_active = false;
    xSemaphoreGive(c->done);
}

/* Run one GATT operation: start(c) issues it; its callback calls
 * op_finish_locked. */
static t5_err_t op_run(t5_ble_conn_t *c, int (*start)(t5_ble_conn_t *c, void *arg), void *arg, uint32_t timeout_ms)
{
    if (xSemaphoreTakeRecursive(c->op_lock, ticks(timeout_ms)) != pdTRUE) return T5_ERR_TIMEOUT;
    t5_mutex_lock(s_mx);
    t5_err_t e = c->up ? T5_OK : T5_ERR_STATE;
    if (e == T5_OK) {
        xSemaphoreTake(c->done, 0);
        c->status = 0;
        c->op_seq++;
        c->op_active = true;
        int rc = start(c, arg);
        if (rc) c->op_active = false;
        e = map_rc(rc);
    }
    t5_mutex_unlock(s_mx);
    if (e == T5_OK) {
        bool got = xSemaphoreTake(c->done, ticks(timeout_ms)) == pdTRUE;
        t5_mutex_lock(s_mx);
        if (!got) {
            c->op_active = false;           /* a late answer is ignored */
            e = T5_ERR_TIMEOUT;
        } else {
            e = c->status == 0 ? T5_OK : c->status == BLE_HS_ENOTCONN ? T5_ERR_STATE : T5_ERR_IO;
        }
        t5_mutex_unlock(s_mx);
    }
    xSemaphoreGiveRecursive(c->op_lock);
    return e;
}

static int on_svc(uint16_t h, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    (void)h;
    t5_mutex_lock(s_mx);
    t5_ble_conn_t *c = op_link_locked(arg);
    if (c) {
        if (err->status == 0 && svc && c->nsvc < MAX_SVCS) {
            c->svcs[c->nsvc].start = svc->start_handle;
            c->svcs[c->nsvc].end = svc->end_handle;
            ble_uuid_to_str(&svc->uuid.u, c->svcs[c->nsvc].uuid);
            c->nsvc++;
        } else if (err->status) {
            op_finish_locked(c, err->status == BLE_HS_EDONE ? 0 : err->status);
        }
    }
    t5_mutex_unlock(s_mx);
    return 0;
}

static int on_chr(uint16_t h, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    (void)h;
    t5_mutex_lock(s_mx);
    t5_ble_conn_t *c = op_link_locked(arg);
    if (c) {
        if (err->status == 0 && chr) {
            if (c->nchr < T5_BLE_MAX_CHARS) {
                t5_ble_chr_t *o = &c->chrs[c->nchr++];
                memset(o, 0, sizeof *o);
                memcpy(o->svc_uuid, c->svcs[c->disc_svc].uuid, sizeof o->svc_uuid);
                ble_uuid_to_str(&chr->uuid.u, o->uuid);
                o->val_handle = chr->val_handle;
                o->def_handle = chr->def_handle;
                o->props = chr->properties;
            }
        } else {
            op_finish_locked(c, err->status == BLE_HS_EDONE ? 0 : err->status);
        }
    }
    t5_mutex_unlock(s_mx);
    return 0;
}

static int on_dsc(uint16_t h, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    (void)h;
    t5_mutex_lock(s_mx);
    t5_ble_conn_t *c = op_link_locked(arg);
    if (c) {
        if (err->status == 0 && dsc) {
            if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
                for (int i = 0; i < c->nchr; i++) {
                    if (c->chrs[i].val_handle == chr_val) c->chrs[i].cccd_handle = dsc->handle;
                }
            }
        } else {
            op_finish_locked(c, err->status == BLE_HS_EDONE ? 0 : err->status);
        }
    }
    t5_mutex_unlock(s_mx);
    return 0;
}

static int start_svcs(t5_ble_conn_t *c, void *arg)
{
    (void)arg;
    c->nsvc = 0;
    c->nchr = 0;
    return ble_gattc_disc_all_svcs(c->handle, on_svc, OP_ARG(c));
}

static int start_chrs(t5_ble_conn_t *c, void *arg)
{
    c->disc_svc = (int)(intptr_t)arg;
    return ble_gattc_disc_all_chrs(c->handle, c->svcs[c->disc_svc].start, c->svcs[c->disc_svc].end, on_chr, OP_ARG(c));
}

/* The descriptors between a characteristic's value and the next one. */
static int start_dscs(t5_ble_conn_t *c, void *arg)
{
    int i = (int)(intptr_t)arg;
    uint16_t end = 0xffff;
    for (int s = 0; s < c->nsvc; s++) {
        if (c->chrs[i].val_handle >= c->svcs[s].start && c->chrs[i].val_handle <= c->svcs[s].end) end = c->svcs[s].end;
    }
    for (int k = 0; k < c->nchr; k++) {
        if (c->chrs[k].def_handle > c->chrs[i].val_handle && c->chrs[k].def_handle - 1 < end) {
            end = c->chrs[k].def_handle - 1;
        }
    }
    return ble_gattc_disc_all_dscs(c->handle, c->chrs[i].val_handle, end, on_dsc, OP_ARG(c));
}

static bool link_ok(t5_ble_conn_t *c)
{
    return c && c->magic == LINK_MAGIC && c->central;
}

t5_err_t t5_ble_gatt_discover(t5_ble_conn_t *c, uint32_t timeout_ms)
{
    if (!link_ok(c)) return T5_ERR_INVALID_ARG;
    T5_RETURN_ON_ERR(op_run(c, start_svcs, NULL, timeout_ms));
    for (int i = 0; i < c->nsvc; i++) T5_RETURN_ON_ERR(op_run(c, start_chrs, (void *)(intptr_t)i, timeout_ms));
    for (int i = 0; i < c->nchr; i++) {
        if (c->chrs[i].props & (T5_BLE_PROP_NOTIFY | T5_BLE_PROP_INDICATE)) {
            T5_RETURN_ON_ERR(op_run(c, start_dscs, (void *)(intptr_t)i, timeout_ms));
        }
    }
    return T5_OK;
}

int t5_ble_gatt_chars(t5_ble_conn_t *c, t5_ble_chr_t *out, int max)
{
    if (!link_ok(c) || !out || max < 0) return T5_ERR_INVALID_ARG;
    t5_mutex_lock(s_mx);
    int n = c->nchr < max ? c->nchr : max;
    memcpy(out, c->chrs, (size_t)n * sizeof *out);
    t5_mutex_unlock(s_mx);
    return n;
}

static bool uuid_eq(const char *a, const char *b)
{
    ble_uuid_any_t ua, ub;
    if (ble_uuid_from_str(&ua, a) || ble_uuid_from_str(&ub, b)) return !strcasecmp(a, b);
    return ble_uuid_cmp(&ua.u, &ub.u) == 0;
}

t5_err_t t5_ble_gatt_find(t5_ble_conn_t *c, const char *svc_uuid, const char *chr_uuid, t5_ble_chr_t *out)
{
    if (!link_ok(c) || !chr_uuid || !out) return T5_ERR_INVALID_ARG;
    t5_err_t e = T5_ERR_NOT_FOUND;
    t5_mutex_lock(s_mx);
    for (int i = 0; i < c->nchr && e != T5_OK; i++) {
        if (uuid_eq(c->chrs[i].uuid, chr_uuid) && (!svc_uuid || uuid_eq(c->chrs[i].svc_uuid, svc_uuid))) {
            *out = c->chrs[i];
            e = T5_OK;
        }
    }
    t5_mutex_unlock(s_mx);
    return e;
}

static int on_read(uint16_t h, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)h;
    t5_mutex_lock(s_mx);
    t5_ble_conn_t *c = op_link_locked(arg);
    if (c) {
        if (err->status == 0 && attr && c->op_buf) {
            uint16_t n = OS_MBUF_PKTLEN(attr->om);
            c->op_len = n < c->op_cap ? n : c->op_cap;
            os_mbuf_copydata(attr->om, 0, (int)c->op_len, c->op_buf);
        }
        op_finish_locked(c, err->status);
    }
    t5_mutex_unlock(s_mx);
    return 0;
}

typedef struct {
    uint16_t handle;
    uint8_t *buf;
    size_t cap;
} read_t;

static int start_read(t5_ble_conn_t *c, void *arg)
{
    const read_t *r = arg;
    c->op_buf = r->buf;
    c->op_cap = r->cap;
    c->op_len = 0;
    return ble_gattc_read(c->handle, r->handle, on_read, OP_ARG(c));
}

int t5_ble_gatt_read(t5_ble_conn_t *c, uint16_t handle, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (!link_ok(c) || (!buf && cap)) return T5_ERR_INVALID_ARG;
    /* Into a buffer of our own: after a timeout the caller's may be gone. */
    uint8_t *tmp = heap_caps_malloc(cap ? cap : 1, MALLOC_CAP_SPIRAM);
    if (!tmp) return T5_ERR_NO_MEM;
    read_t r = { handle, tmp, cap };
    /* Held across op_run (recursive) so another read can't reuse op_len first. */
    if (xSemaphoreTakeRecursive(c->op_lock, ticks(timeout_ms)) != pdTRUE) {
        free(tmp);
        return T5_ERR_TIMEOUT;
    }
    t5_err_t e = op_run(c, start_read, &r, timeout_ms);
    t5_mutex_lock(s_mx);
    size_t n = c->op_len;
    c->op_buf = NULL;
    t5_mutex_unlock(s_mx);
    xSemaphoreGiveRecursive(c->op_lock);
    if (e == T5_OK) memcpy(buf, tmp, n);
    free(tmp);
    return e == T5_OK ? (int)n : e;
}

static int on_write(uint16_t h, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)h;
    (void)attr;
    t5_mutex_lock(s_mx);
    t5_ble_conn_t *c = op_link_locked(arg);
    if (c) op_finish_locked(c, err->status);
    t5_mutex_unlock(s_mx);
    return 0;
}

typedef struct {
    uint16_t handle;
    const void *data;
    size_t len;
} write_t;

static int start_write(t5_ble_conn_t *c, void *arg)
{
    const write_t *w = arg;
    return ble_gattc_write_flat(c->handle, w->handle, w->data, (uint16_t)w->len, on_write, OP_ARG(c));
}

t5_err_t t5_ble_gatt_write(t5_ble_conn_t *c, uint16_t handle, const void *data, size_t len, bool response,
                           uint32_t timeout_ms)
{
    if (!link_ok(c) || (!data && len) || len > T5_BLE_VALUE_MAX) return T5_ERR_INVALID_ARG;
    if (!response) {
        t5_mutex_lock(s_mx);
        t5_err_t e = c->up ? map_rc(ble_gattc_write_no_rsp_flat(c->handle, handle, data, (uint16_t)len)) : T5_ERR_STATE;
        t5_mutex_unlock(s_mx);
        return e;
    }
    write_t w = { handle, data, len };
    return op_run(c, start_write, &w, timeout_ms);
}

t5_err_t t5_ble_gatt_subscribe(t5_ble_conn_t *c, uint16_t val_handle, bool on, uint32_t timeout_ms)
{
    if (!link_ok(c)) return T5_ERR_INVALID_ARG;
    t5_ble_chr_t ch = { 0 };
    bool found = false;
    t5_mutex_lock(s_mx);
    for (int i = 0; i < c->nchr && !found; i++) {
        if (c->chrs[i].val_handle == val_handle) {
            ch = c->chrs[i];
            found = true;
        }
    }
    t5_mutex_unlock(s_mx);
    if (!found || !ch.cccd_handle) return T5_ERR_NOT_FOUND;
    /* Notifications if it has them, else indications. */
    uint8_t v[2] = { on ? ((ch.props & T5_BLE_PROP_NOTIFY) ? 1 : 2) : 0, 0 };
    return t5_ble_gatt_write(c, ch.cccd_handle, v, 2, true, timeout_ms);
}

int t5_ble_gatt_notification(t5_ble_conn_t *c, uint16_t *handle, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (!link_ok(c) || (!buf && cap)) return T5_ERR_INVALID_ARG;
    note_t *n = heap_caps_malloc(sizeof *n, MALLOC_CAP_SPIRAM);
    if (!n) return T5_ERR_NO_MEM;
    uint32_t t0 = t5_time_ms();
    int r = T5_ERR_TIMEOUT;
    for (;;) {
        if (xQueueReceive(c->notes, n, 0) == pdTRUE) {
            size_t k = n->len < cap ? n->len : cap;
            memcpy(buf, n->data, k);
            if (handle) *handle = n->handle;
            r = (int)k;
            break;
        }
        if (!c->up) {
            r = T5_ERR_STATE;
            break;
        }
        uint32_t spent = t5_time_ms() - t0;
        if (timeout_ms != T5_BLE_FOREVER && spent >= timeout_ms) break;
        uint32_t step = timeout_ms == T5_BLE_FOREVER || timeout_ms - spent > 100 ? 100 : timeout_ms - spent;
        if (xQueueReceive(c->notes, n, pdMS_TO_TICKS(step)) == pdTRUE) {
            size_t k = n->len < cap ? n->len : cap;
            memcpy(buf, n->data, k);
            if (handle) *handle = n->handle;
            r = (int)k;
            break;
        }
    }
    free(n);
    return r;
}

/* ---- links we make (the central role) ------------------------------------ */
static int mtu_done(uint16_t h, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    (void)h; (void)err; (void)mtu; (void)arg;
    return 0;
}

static int link_event(struct ble_gap_event *e, void *arg)
{
    int src;
    if (sec_event(e, &src)) return src;
    t5_mutex_lock(s_mx);
    t5_ble_conn_t *c = arg;
    if (!link_valid_locked(c)) {
        t5_mutex_unlock(s_mx);
        return 0;
    }
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        c->connecting = false;
        if (e->connect.status == 0) {
            c->handle = e->connect.conn_handle;
            c->up = true;
            ble_gattc_exchange_mtu(c->handle, mtu_done, NULL);
            link_lock_update_locked();
            post(T5_EV_BLE_CONNECTED, T5_OK, links_up_locked());
        } else {
            post(T5_EV_BLE_DISCONNECTED, e->connect.status, links_up_locked());
            if (c->sys) {
                link_remove_locked(c);
                xSemaphoreGive(c->done);
                t5_mutex_unlock(s_mx);
                link_free(c);
                return 0;
            }
        }
        c->status = e->connect.status;
        xSemaphoreGive(c->done);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        T5_LOGI(TAG, "link %u down: reason 0x%x", c->handle, e->disconnect.reason);
        c->up = false;
        c->handle = BLE_HS_CONN_HANDLE_NONE;
        link_lock_update_locked();
        post(T5_EV_BLE_DISCONNECTED, e->disconnect.reason, links_up_locked());
        if (c->sys) {
            link_remove_locked(c);
            t5_mutex_unlock(s_mx);
            link_free(c);
            return 0;
        }
        c->status = BLE_HS_ENOTCONN;
        xSemaphoreGive(c->done);            /* an op or a close waiting */
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        note_t n = { .handle = e->notify_rx.attr_handle };
        uint16_t len = OS_MBUF_PKTLEN(e->notify_rx.om);
        n.len = len > sizeof n.data ? sizeof n.data : len;
        os_mbuf_copydata(e->notify_rx.om, 0, n.len, n.data);
        if (c->notes) xQueueSend(c->notes, &n, 0);
        break;
    }
    default:
        break;
    }
    t5_mutex_unlock(s_mx);
    return 0;
}

static void link_release(void *h)
{
    T5_IGNORE(t5_ble_conn_close(h));      /* exit cleanup: nothing to report to */
}

/* Start connecting (under s_mx). */
static t5_err_t connect_locked(t5_ble_conn_t *c, const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms)
{
    if (s_scanner && !s_scanner->ended) return T5_ERR_BUSY;    /* NimBLE can't scan and connect at once */
    memcpy(c->addr, addr, 6);
    c->addr_type = addr_type;
    ble_addr_t a = { .type = addr_type };
    memcpy(a.val, addr, 6);
    c->connecting = true;
    int32_t dur = timeout_ms == T5_BLE_FOREVER || timeout_ms > INT32_MAX ? BLE_HS_FOREVER : (int32_t)timeout_ms;
    int rc = ble_gap_connect(s_own_addr_type, &a, dur, NULL, link_event, c);
    if (rc) c->connecting = false;
    return map_rc(rc);
}

t5_err_t t5_ble_conn_open(const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms, t5_ble_conn_t **out)
{
    if (!addr || !out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!s_mx) return T5_ERR_STATE;
    void *app = t5_app_ctx_get();
    t5_mutex_lock(s_mx);
    t5_err_t e = !s_synced ? T5_ERR_STATE : owner_take_locked(app) ? T5_OK : T5_ERR_BUSY;
    t5_ble_conn_t *c = NULL;
    if (e == T5_OK) {
        c = link_new_locked(app, true);
        if (!c) e = T5_ERR_BUSY;
        else e = connect_locked(c, addr, addr_type, timeout_ms);
        if (e != T5_OK) {
            owner_give_locked(app);
            if (c) link_remove_locked(c);
        }
    }
    t5_mutex_unlock(s_mx);
    if (e != T5_OK) {
        link_free(c);
        return e;
    }
    /* NimBLE reports the end of the attempt either way (a timeout too). */
    xSemaphoreTake(c->done, portMAX_DELAY);
    if (!c->up) {
        t5_mutex_lock(s_mx);
        link_remove_locked(c);
        owner_give_locked(app);
        t5_mutex_unlock(s_mx);
        int st = c->status;
        link_free(c);
        return st == BLE_HS_ETIMEOUT ? T5_ERR_TIMEOUT : T5_ERR_IO;
    }
    if (app) t5_res_add(T5_RES_OTHER, c, link_release);
    *out = c;
    return T5_OK;
}

bool t5_ble_conn_up(t5_ble_conn_t *c)
{
    return link_ok(c) && c->up;
}

uint16_t t5_ble_conn_mtu(t5_ble_conn_t *c)
{
    if (!link_ok(c)) return 0;
    t5_mutex_lock(s_mx);
    uint16_t m = c->up ? ble_att_mtu(c->handle) : 0;
    t5_mutex_unlock(s_mx);
    return m;
}

t5_err_t t5_ble_conn_close(t5_ble_conn_t *c)
{
    if (!link_ok(c) || c->sys) return T5_ERR_INVALID_ARG;
    t5_res_remove(c);
    t5_mutex_lock(s_mx);
    bool wait = c->up && s_synced;
    if (wait) {
        xSemaphoreTake(c->done, 0);
        if (ble_gap_terminate(c->handle, BLE_ERR_REM_USER_CONN_TERM)) wait = false;
    }
    t5_mutex_unlock(s_mx);
    /* The disconnect event, then nothing more comes for this link. */
    for (int i = 0; wait && c->up && i < CLOSE_WAIT_MS / 50; i++) xSemaphoreTake(c->done, pdMS_TO_TICKS(50));
    t5_mutex_lock(s_mx);
    link_remove_locked(c);
    owner_give_locked(c->app);
    link_lock_update_locked();
    t5_mutex_unlock(s_mx);
    link_free(c);
    return T5_OK;
}

/* ---- advertising (API) ---------------------------------------------------- */
static void adv_release(void *h)
{
    T5_IGNORE(t5_ble_adv_stop(h));
}

static t5_err_t adv_start(const t5_ble_adv_cfg_t *cfg, void *app, uint16_t appearance, t5_ble_advertiser_t **out)
{
    if (!cfg || !out || cfg->mfr_len > 29 || (cfg->mfr_len && !cfg->mfr_data)) return T5_ERR_INVALID_ARG;
    if (cfg->interval_ms && (cfg->interval_ms < 20 || cfg->interval_ms > 10240)) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!s_mx) return T5_ERR_STATE;
    t5_ble_advertiser_t *a = heap_caps_calloc(1, sizeof *a, MALLOC_CAP_SPIRAM);
    if (!a) return T5_ERR_NO_MEM;
    a->magic = ADV_MAGIC;
    a->app = app;
    a->appearance = appearance;
    a->cfg = *cfg;
    if (cfg->name) snprintf(a->name, sizeof a->name, "%s", cfg->name);
    if (cfg->mfr_len) memcpy(a->mfr, cfg->mfr_data, cfg->mfr_len);
    a->cfg.name = a->name;
    a->cfg.mfr_data = a->mfr;
    t5_err_t e = T5_OK;
    if (cfg->svc_uuid) {
        a->has_uuid = ble_uuid_from_str(&a->uuid, cfg->svc_uuid) == 0 && a->uuid.u.type != BLE_UUID_TYPE_32;
        if (!a->has_uuid) e = T5_ERR_INVALID_ARG;
    }
    /* Without the name it must fit the packet; the name may go in the scan response. */
    if (e == T5_OK && (adv_size(a, false) > 31 || (a->name[0] && 2 + strlen(a->name) > 31))) e = T5_ERR_INVALID_ARG;
    t5_mutex_lock(s_mx);
    if (e == T5_OK && s_adv) e = T5_ERR_BUSY;
    if (e == T5_OK && !owner_take_locked(a->app)) e = T5_ERR_BUSY;
    if (e == T5_OK) {
        s_adv = a;
        int rc = adv_apply_locked(a);       /* or when the stack is next in sync */
        if (rc) {
            s_adv = NULL;
            owner_give_locked(a->app);
            e = map_rc(rc);
        } else if (a->app) {
            t5_res_add(T5_RES_OTHER, a, adv_release);
        }
    }
    t5_mutex_unlock(s_mx);
    if (e != T5_OK) {
        free(a);
        return e;
    }
    *out = a;
    return T5_OK;
}

t5_err_t t5_ble_adv_start(const t5_ble_adv_cfg_t *cfg, t5_ble_advertiser_t **out)
{
    return adv_start(cfg, t5_app_ctx_get(), 0, out);
}

static t5_ble_advertiser_t *s_sys_adv;

t5_err_t ble_adv_sys_start(const char *name, uint16_t appearance, const char *uuid)
{
    t5_mutex_lock(s_mx);
    t5_err_t e = s_sys_adv ? T5_ERR_BUSY : T5_OK;
    if (e == T5_OK) {
        /* Fast enough for a host looking to (re)connect. */
        const t5_ble_adv_cfg_t cfg = { .name = name, .svc_uuid = uuid, .interval_ms = 50, .connectable = true };
        e = adv_start(&cfg, NULL, appearance, &s_sys_adv);
    }
    t5_mutex_unlock(s_mx);
    return e;
}

void ble_adv_sys_stop(void)
{
    t5_mutex_lock(s_mx);
    t5_ble_advertiser_t *a = s_sys_adv;
    s_sys_adv = NULL;
    t5_mutex_unlock(s_mx);
    if (a) T5_IGNORE(t5_ble_adv_stop(a));
}

t5_mutex_t ble_mx(void)
{
    return s_mx;
}

bool ble_synced(void)
{
    return s_synced;
}

t5_err_t t5_ble_adv_stop(t5_ble_advertiser_t *a)
{
    if (!a || a->magic != ADV_MAGIC) return T5_ERR_INVALID_ARG;
    t5_res_remove(a);
    t5_mutex_lock(s_mx);
    if (s_adv == a) {
        s_adv = NULL;
        if (s_synced && ble_gap_adv_active()) ble_gap_adv_stop();
    }
    owner_give_locked(a->app);
    a->magic = 0;
    t5_mutex_unlock(s_mx);
    free(a);
    return T5_OK;
}

/* ---- the GATT server (API) ------------------------------------------------- */
#define CHR_ARG(slot, k) ((void *)(intptr_t)((slot) * 16 + (k) + 1))

static int access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    int v = (int)(intptr_t)arg - 1, slot = v / 16, k = v % 16;
    int rc = BLE_ATT_ERR_UNLIKELY;
    t5_mutex_lock(s_mx);
    t5_ble_server_t *s = slot >= 0 && slot < MAX_SERVERS ? s_servers[slot] : NULL;
    if (s && k < s->n) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            rc = os_mbuf_append(ctxt->om, s->val[k].data, s->val[k].len) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        } else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            srv_item_t *it = heap_caps_calloc(1, sizeof *it, MALLOC_CAP_SPIRAM);
            uint16_t len = 0;
            if (!it) {
                rc = BLE_ATT_ERR_INSUFFICIENT_RES;
            } else if (ble_hs_mbuf_to_flat(ctxt->om, it->data, sizeof it->data, &len)) {
                rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            } else {
                memcpy(s->val[k].data, it->data, len);
                s->val[k].len = len;
                it->ev.type = T5_BLE_SERVER_WRITE;
                it->ev.chr = k;
                it->ev.peer = conn;
                it->len = len;
                t5_ble_conn_t *c = link_by_handle_locked(conn);
                if (c) memcpy(it->ev.addr, c->addr, 6);
                xQueueSend(s->events, it, 0);
                rc = 0;
            }
            free(it);
        }
    }
    t5_mutex_unlock(s_mx);
    return rc;
}

static int server_register_locked(t5_ble_server_t *s)
{
    if (s->registered || !s_synced) return 0;
    int rc = ble_gatts_add_dynamic_svcs(s->svc);
    s->registered = rc == 0;
    return rc;
}

static void server_release(void *h)
{
    T5_IGNORE(t5_ble_server_close(h));
}

static void server_free(t5_ble_server_t *s)
{
    if (s->events) vQueueDeleteWithCaps(s->events);
    s->magic = 0;
    free(s);
}

t5_err_t t5_ble_server_open(const char *svc_uuid, const t5_ble_server_chr_t *chrs, int n, t5_ble_server_t **out)
{
    if (!svc_uuid || !chrs || n < 1 || n > T5_BLE_SERVER_MAX_CHARS || !out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!s_mx) return T5_ERR_STATE;
    t5_ble_server_t *s = heap_caps_calloc(1, sizeof *s, MALLOC_CAP_SPIRAM);
    if (!s) return T5_ERR_NO_MEM;
    s->magic = SRV_MAGIC;
    s->app = t5_app_ctx_get();
    s->n = n;
    s->events = xQueueCreateWithCaps(SERVER_QUEUE, sizeof(srv_item_t), MALLOC_CAP_SPIRAM);
    t5_err_t e = s->events && ble_uuid_from_str(&s->svc_uuid, svc_uuid) == 0 ? T5_OK : T5_ERR_INVALID_ARG;
    for (int k = 0; k < n && e == T5_OK; k++) {
        uint8_t p = chrs[k].props;
        if (!chrs[k].uuid || ble_uuid_from_str(&s->chr_uuid[k], chrs[k].uuid) || !p) e = T5_ERR_INVALID_ARG;
        s->props[k] = p;
        s->chr[k].uuid = &s->chr_uuid[k].u;
        s->chr[k].access_cb = access;
        s->chr[k].val_handle = &s->val_handle[k];
        s->chr[k].flags = (p & T5_BLE_PROP_READ ? BLE_GATT_CHR_F_READ : 0) |
                          (p & T5_BLE_PROP_WRITE ? BLE_GATT_CHR_F_WRITE : 0) |
                          (p & T5_BLE_PROP_WRITE_NR ? BLE_GATT_CHR_F_WRITE_NO_RSP : 0) |
                          (p & T5_BLE_PROP_NOTIFY ? BLE_GATT_CHR_F_NOTIFY : 0) |
                          (p & T5_BLE_PROP_INDICATE ? BLE_GATT_CHR_F_INDICATE : 0);
    }
    s->svc[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s->svc[0].uuid = &s->svc_uuid.u;
    s->svc[0].characteristics = s->chr;
    t5_mutex_lock(s_mx);
    int slot = -1;
    for (int i = 0; e == T5_OK && i < MAX_SERVERS; i++) {
        if (s_servers[i] && ble_uuid_cmp(&s_servers[i]->svc_uuid.u, &s->svc_uuid.u) == 0) e = T5_ERR_BUSY;
        if (!s_servers[i] && slot < 0) slot = i;
    }
    if (e == T5_OK && slot < 0) e = T5_ERR_BUSY;
    if (e == T5_OK && !owner_take_locked(s->app)) e = T5_ERR_BUSY;
    if (e == T5_OK) {
        s->slot = slot;
        for (int k = 0; k < n; k++) s->chr[k].arg = CHR_ARG(slot, k);
        s_servers[slot] = s;
        int rc = server_register_locked(s);    /* or when the stack is next in sync */
        if (rc) {
            s_servers[slot] = NULL;
            owner_give_locked(s->app);
            e = map_rc(rc);
        } else if (s->app) {
            t5_res_add(T5_RES_OTHER, s, server_release);
        }
    }
    t5_mutex_unlock(s_mx);
    if (e != T5_OK) {
        server_free(s);
        return e;
    }
    *out = s;
    return T5_OK;
}

t5_err_t t5_ble_server_set(t5_ble_server_t *s, int chr, const void *data, size_t len)
{
    if (!s || s->magic != SRV_MAGIC || chr < 0 || chr >= s->n || len > T5_BLE_VALUE_MAX || (!data && len)) {
        return T5_ERR_INVALID_ARG;
    }
    t5_mutex_lock(s_mx);
    memcpy(s->val[chr].data, data, len);
    s->val[chr].len = (uint16_t)len;
    bool tell = s->registered && s_synced && (s->props[chr] & (T5_BLE_PROP_NOTIFY | T5_BLE_PROP_INDICATE));
    uint16_t h = s->val_handle[chr];
    t5_mutex_unlock(s_mx);
    /* Subscribed centrals get it (NimBLE reads it back through access()). */
    if (tell) ble_gatts_chr_updated(h);
    return T5_OK;
}

int t5_ble_server_next(t5_ble_server_t *s, t5_ble_server_ev_t *ev, void *buf, size_t cap, uint32_t timeout_ms)
{
    if (!s || s->magic != SRV_MAGIC || !ev || (!buf && cap)) return T5_ERR_INVALID_ARG;
    srv_item_t *it = heap_caps_malloc(sizeof *it, MALLOC_CAP_SPIRAM);
    if (!it) return T5_ERR_NO_MEM;
    int r = T5_ERR_TIMEOUT;
    if (xQueueReceive(s->events, it, ticks(timeout_ms)) == pdTRUE) {
        *ev = it->ev;
        size_t k = it->ev.type == T5_BLE_SERVER_WRITE ? (it->len < cap ? it->len : cap) : 0;
        if (k) memcpy(buf, it->data, k);
        r = (int)k;
    }
    free(it);
    return r;
}

t5_err_t t5_ble_server_close(t5_ble_server_t *s)
{
    if (!s || s->magic != SRV_MAGIC) return T5_ERR_INVALID_ARG;
    t5_res_remove(s);
    t5_mutex_lock(s_mx);
    if (s_servers[s->slot] == s) s_servers[s->slot] = NULL;
    if (s->registered && s_synced) ble_gatts_delete_svc(&s->svc_uuid.u);
    owner_give_locked(s->app);
    t5_mutex_unlock(s_mx);
    server_free(s);
    return T5_OK;
}

/* ---- stack lifecycle ------------------------------------------------------ */
static void on_sync(void)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) return;
    t5_mutex_lock(s_mx);
    s_synced = true;
    /* Servers and an advertiser opened while the stack was down, or kept
     * across a C6 restart, go (back) on the air. */
    for (int i = 0; i < MAX_SERVERS; i++) {
        if (s_servers[i] && server_register_locked(s_servers[i])) T5_LOGW(TAG, "a GATT server didn't register");
    }
    if (adv_apply_locked(s_adv)) T5_LOGW(TAG, "advertising didn't start");
    t5_mutex_unlock(s_mx);
    xSemaphoreGive(s_sync);
    T5_LOGI(TAG, "ready");
    post(T5_EV_BLE_READY, T5_OK, 0);
}

static void on_reset(int reason)
{
    s_synced = false;
    xSemaphoreTake(s_sync, 0);
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
    if (s_running || !s_enabled || !s_link_up) return;
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
    /* The GAP (device name) and GATT services centrals expect of a peripheral. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    /* Pairing: LE Secure Connections with numeric comparison (the Tab5
     * shows a code, the person confirms it matches), bonds kept in NVS;
     * when the store is full the oldest bond goes. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_YES_NO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    /* HID's services first and every time, so their handles never move. */
    if (ble_hid_register()) T5_LOGE(TAG, "the HID services didn't register");
    xSemaphoreTake(s_sync, 0);
    s_running = true;
    nimble_port_freertos_init(host_task);
}

/* Everything on the air ends with the stack: links are down (their handles
 * stay for their owners to close), scans end, servers must register again. */
static void stack_down_locked(void)
{
    for (int i = 0; i < T5_BLE_MAX_LINKS; i++) {
        t5_ble_conn_t *c = s_links[i];
        if (!c) continue;
        bool was = c->up || c->connecting;
        c->up = false;
        c->connecting = false;
        c->handle = BLE_HS_CONN_HANDLE_NONE;
        if (!c->central || c->sys) {
            if (!c->central) servers_post_locked(T5_BLE_SERVER_DISCONNECTED, 0, c->addr);
            link_remove_locked(c);
            link_free(c);
        } else if (was) {
            c->status = BLE_HS_ENOTCONN;
            xSemaphoreGive(c->done);
        }
        if (was) post(T5_EV_BLE_DISCONNECTED, -1, 0);
    }
    if (s_scanner) s_scanner->ended = true;
    ble_hid_down_locked();
    for (int i = 0; i < MAX_SERVERS; i++) {
        if (s_servers[i]) s_servers[i]->registered = false;
    }
    link_lock_update_locked();
}

static void stack_stop(void)
{
    if (!s_running) return;
    /* Stopping NimBLE while its host task is still in the start-up sync
     * frees timers that the sync then uses (seen as a crash in
     * ble_hs_timer_sched when Sleep powered the C6 off right after boot).
     * Let the start finish first. */
    if (!s_synced) xSemaphoreTake(s_sync, pdMS_TO_TICKS(SYNC_WAIT_MS));
    t5_mutex_lock(s_mx);
    s_synced = false;
    stack_down_locked();
    t5_mutex_unlock(s_mx);
    nimble_port_stop();
    nimble_port_deinit();
    esp_hosted_bt_host_stack_teardown();
    s_running = false;
}

/* From the net task (t5_ble_on_link) and any caller of t5_ble_enable. */
static void ensure_init(void)
{
    static t5_mutex_t s_init_mx;
    static bool s_inited;
    if (__atomic_load_n(&s_inited, __ATOMIC_ACQUIRE)) return;
    t5_mutex_t m = t5_mutex_once(&s_init_mx);
    if (!m) return;
    t5_mutex_lock(m);
    if (!s_inited) {
        s_mx = t5_mutex_create();
        s_sync = xSemaphoreCreateBinary();
        nvs_handle_t h;
        uint8_t v = 0;
        if (net_nvs_open(NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "ble", &v);
            nvs_close(h);
        }
        s_enabled = v;
        __atomic_store_n(&s_inited, true, __ATOMIC_RELEASE);
    }
    t5_mutex_unlock(m);
}

void t5_ble_on_link(bool up)
{
    ensure_init();
    s_link_up = up;
    if (up && s_enabled) stack_start();
    else stack_stop();
}

/* ---- the system's API (ble_sys.h) ------------------------------------------ */
t5_err_t t5_ble_enable(bool on)
{
    ensure_init();
    s_enabled = on;
    nvs_handle_t h;
    if (net_nvs_open(NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ble", on);
        nvs_commit(h);
        nvs_close(h);
    }
    /* The stack starts and stops on the net task, which owns the C6. */
    net_ble_changed();
    return T5_OK;   /* the net task powers the C6 if it was off; the stack starts once it is up */
}

bool t5_ble_enabled(void)
{
    ensure_init();
    return s_enabled;
}

bool t5_ble_ready(void)
{
    return s_synced;
}

int t5_ble_links(void)
{
    if (!s_mx) return 0;
    t5_mutex_lock(s_mx);
    int n = links_up_locked();
    t5_mutex_unlock(s_mx);
    return n;
}

static t5_ble_scanner_t s_sys_scanner = { .magic = SCAN_MAGIC, .sys = true };

t5_err_t t5_ble_sys_scan(uint32_t ms)
{
    if (!s_mx) return T5_ERR_STATE;
    const t5_ble_scan_cfg_t cfg = { .duration_ms = ms ? ms : 5000, .active = true };
    t5_mutex_lock(s_mx);
    t5_err_t e = scan_begin_locked(&s_sys_scanner, &cfg);   /* BUSY while any scan runs, its own too */
    if (e == T5_OK) s_ndevs = 0;
    t5_mutex_unlock(s_mx);
    return e;
}

int t5_ble_sys_scan_results(t5_ble_dev_t *out, int max)
{
    if (!s_mx || !out || max < 0) return 0;
    t5_mutex_lock(s_mx);
    int n = s_ndevs < max ? s_ndevs : max;
    memcpy(out, s_devs, (size_t)n * sizeof *out);
    t5_mutex_unlock(s_mx);
    return n;
}

t5_err_t t5_ble_sys_connect(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr || !s_mx) return T5_ERR_STATE;
    t5_mutex_lock(s_mx);
    t5_err_t e = !s_synced ? T5_ERR_STATE : s_sys_link ? T5_ERR_BUSY : T5_OK;
    if (e == T5_OK && s_scanner == &s_sys_scanner) {
        s_scanner = NULL;                   /* Settings' scan gives way to its connect */
        ble_gap_disc_cancel();
    }
    t5_ble_conn_t *c = e == T5_OK ? link_new_locked(NULL, true) : NULL;
    if (e == T5_OK && !c) e = T5_ERR_BUSY;
    if (e == T5_OK) {
        c->sys = true;
        e = connect_locked(c, addr, addr_type, 10000);
        if (e == T5_OK) s_sys_link = c;
        else link_remove_locked(c);
    }
    t5_mutex_unlock(s_mx);
    if (e != T5_OK) link_free(c);
    return e;
}

t5_err_t t5_ble_sys_disconnect(void)
{
    if (!s_mx) return T5_ERR_STATE;
    t5_mutex_lock(s_mx);
    t5_ble_conn_t *c = s_sys_link;
    t5_err_t e = !c ? T5_ERR_STATE : c->up ? map_rc(ble_gap_terminate(c->handle, BLE_ERR_REM_USER_CONN_TERM))
                                             : map_rc(ble_gap_conn_cancel());
    t5_mutex_unlock(s_mx);
    return e;   /* the link is freed when NimBLE reports it gone */
}

bool t5_ble_sys_connected(void)
{
    if (!s_mx) return false;
    t5_mutex_lock(s_mx);
    bool up = s_sys_link && s_sys_link->up;
    t5_mutex_unlock(s_mx);
    return up;
}

/* ---- pairing and bonds (t5_net/ble_sys.h) --------------------------------- */
t5_err_t t5_ble_sys_pair_reply(uint16_t conn, bool accept)
{
    if (!s_synced) return T5_ERR_STATE;
    struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = accept };
    return map_rc(ble_sm_inject_io(conn, &io));
}

int t5_ble_sys_bonds(t5_ble_bond_t *out, int max)
{
    if (!s_running || !out || max <= 0) return 0;
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, MYNEWT_VAL(BLE_STORE_MAX_BONDS))) return 0;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].addr, peers[i].val, 6);
        out[i].addr_type = peers[i].type;
    }
    return n;
}

t5_err_t t5_ble_sys_forget(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr) return T5_ERR_INVALID_ARG;
    if (!s_running) return T5_ERR_STATE;
    ble_addr_t a = { .type = addr_type };
    memcpy(a.val, addr, 6);
    /* A link to it ends first: its keys go with the bond. */
    struct ble_gap_conn_desc d;
    for (int i = 0; i < T5_BLE_MAX_LINKS; i++) {
        t5_mutex_lock(s_mx);
        t5_ble_conn_t *c = s_links[i];
        uint16_t h = c && c->up ? c->handle : BLE_HS_CONN_HANDLE_NONE;
        t5_mutex_unlock(s_mx);
        if (h != BLE_HS_CONN_HANDLE_NONE && ble_gap_conn_find(h, &d) == 0 && !ble_addr_cmp(&d.peer_id_addr, &a)) {
            ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    int rc = ble_store_util_delete_peer(&a);
    return rc == 0 || rc == BLE_HS_ENOENT ? T5_OK : map_rc(rc);
}
