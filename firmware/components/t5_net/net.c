/* SPDX-License-Identifier: Apache-2.0
 * Network service on the device: C6 power and link (esp_hosted over SDIO),
 * Wi-Fi station, SNTP, link health, sleep integration and C6 updates.
 *
 * One task owns all of it: public calls and IDF event handlers queue
 * messages for it. The getters on other tasks read only s_st, s_scan and
 * s_cfg, which the task changes under s_mtx (and reads without it);
 * t5_net_sync_time starts SNTP under s_mtx too.
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "esp_hosted_misc.h"
#include "esp_hosted_ota.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "t5_bsp/xpin.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_net/ble_sys.h"
#include "t5_net/net.h"
#include "t5_net/netcfg.h"
#include "t5_net/sock.h"
#include "t5_storage/secret.h"
#include "t5_pins/pins.h"
#include "t5_power/power.h"
#include "t5_power/rail.h"
#include "t5_time/t5_time.h"
#include "c6img.h"
#include "net_priv.h"

static const char *TAG = "t5.net";

#define NVS_KEY "cfg"
#define C6_PART_LABEL "c6fw"           /* partitions.csv */
#define POWER_SETTLE_MS 100          /* C6 rail up before the link starts */
#define POWER_OFF_MS 200             /* C6 rail down long enough to reset it */
#define HEALTH_PERIOD_MS 30000
#define HEALTH_FAILS_TO_RESET 2
#define RESCAN_MS 30000              /* saved networks but none in range */
#define START_RETRY_MIN_MS 10000
#define START_RETRY_MAX_MS (10u * 60 * 1000)
#define WAKE_CPU_MS 2000             /* CPU held after a C6 wake edge */
#define SUSPEND_WAIT_MS 4000             /* the C6 may be mid-restart */
#define SUSPEND_BUDGET_MS 1500

typedef enum {
    CMD_START, CMD_STOP, CMD_SCAN, CMD_CONNECT, CMD_DISCONNECT, CMD_FORGET, CMD_RESET, CMD_SUSPEND, CMD_RESUME,
    CMD_UPDATE, CMD_ANTENNA, CMD_BLE, CMD_HOLD,
    EV_STA_START, EV_CONNECTED, EV_DISCONNECTED, EV_GOT_IP, EV_SCAN_DONE, EV_LINK_DOWN, EV_LINK_FAIL, EV_WAKE,
    EV_SNTP,
} msg_type_t;

typedef struct {
    msg_type_t type;
    bool flag;                   /* save / force / keep */
    int32_t arg;                 /* disconnect reason, antenna */
    char ssid[T5_NET_SSID_LEN];
    char pass[T5_NET_PASS_LEN];
    uint8_t ip[16];
} msg_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_done;     /* CMD_SUSPEND completion */
static t5_mutex_t s_mtx;
static t5_net_status_t s_st;
static t5_net_ap_t s_scan[T5_NET_MAX_SCAN];
static int s_nscan;
static t5_netcfg_t s_cfg;
static t5_netcfg_backoff_t s_backoff;
static t5_rail_hold_t *s_rail;
static esp_netif_t *s_netif;
static bool s_hosted_up, s_wifi_up, s_sntp_started;
static bool s_auto = true;           /* auto-connect to saved networks */
static bool s_held;                  /* an app's monitor has the radio (net_station_hold) */
static bool s_scan_for_auto;
static bool s_scanning;
static bool s_got_ip;
static bool s_suspended_off;         /* powered down by Sleep, restart on resume */
static bool s_ps;                    /* in modem power save for Sleep */
static char s_target[T5_NET_SSID_LEN];
static char s_pending_pass[T5_NET_PASS_LEN];
static bool s_pending_save;
static uint32_t s_t_power_on, s_t_connect;
static uint32_t s_t_link_change;     /* last time we took the link up or down ourselves */
static bool s_expect_leave;          /* our own esp_wifi_disconnect() is in flight */
/* The last network we had an address on: a C6 reset or resume rejoins it
 * directly on its channel and BSSID, without scanning (plan 3.x: < 5 s). */
static char s_last_ssid[T5_NET_SSID_LEN];
static uint8_t s_last_bssid[6];
static uint8_t s_last_channel;
static uint32_t s_next_health, s_next_scan, s_next_start, s_next_connect;
static uint32_t s_start_backoff = START_RETRY_MIN_MS;
static int s_health_fails;
static bool s_inited;

const char *t5_net_state_name(t5_net_state_t s)
{
    switch (s) {
    case T5_NET_OFF: return "off";
    case T5_NET_STARTING: return "starting";
    case T5_NET_IDLE: return "idle";
    case T5_NET_CONNECTING: return "connecting";
    case T5_NET_CONNECTED: return "connected";
    case T5_NET_FAILED: return "failed";
    default: return "?";
    }
}

static void set_state(t5_net_state_t s)
{
    t5_mutex_lock(s_mtx);
    s_st.state = s;
    t5_mutex_unlock(s_mtx);
}

static void post_ev(t5_event_id_t id, int32_t err)
{
    t5_ev_net_t ev = { .err = err, .rssi = s_st.rssi, .count = (uint8_t)s_nscan };
    memcpy(ev.ip, s_st.ip, 4);
    t5_event_post(id, &ev, sizeof ev);
}

/* Bounded copy that always terminates (and never trips stringop-truncation). */
static void scpy(char *dst, size_t cap, const char *src, size_t srcmax)
{
    size_t n = strnlen(src, srcmax);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool due(uint32_t at, uint32_t now)
{
    return at && (int32_t)(now - at) >= 0;
}

static t5_err_t send(const msg_t *m)
{
    if (!s_q) return T5_ERR_STATE;
    return xQueueSend(s_q, m, pdMS_TO_TICKS(100)) == pdTRUE ? T5_OK : T5_ERR_BUSY;
}

/* The saved networks are sealed with a key bound to this chip (plan 9.4,
 * t5_storage/secret.h). A plaintext blob from before is read once and saved
 * sealed. */

static esp_err_t nvs_ready(void)
{
    static esp_err_t s_nvs = ESP_ERR_INVALID_STATE;
    if (s_nvs == ESP_OK) return ESP_OK;
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    s_nvs = e;
    return e;
}

esp_err_t net_nvs_open(nvs_open_mode_t mode, nvs_handle_t *h)
{
    esp_err_t e = nvs_ready();
    return e == ESP_OK ? nvs_open(NET_NVS_NS, mode, h) : e;
}

static void cfg_save(void);

static void cfg_load(void)
{
    nvs_handle_t h;
    t5_netcfg_init(&s_cfg);
    if (net_nvs_open(NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    uint8_t *blob = NULL;
    if (nvs_get_blob(h, NVS_KEY, NULL, &len) == ESP_OK && len) blob = malloc(len);
    if (blob && nvs_get_blob(h, NVS_KEY, blob, &len) != ESP_OK) len = 0;
    nvs_close(h);
    if (!blob) return;
    bool migrate = false;
    size_t n = 0;
    if (t5_secret_is_sealed(blob, len)) {
        if (t5_secret_open(blob, len, &s_cfg, sizeof s_cfg, &n) != T5_OK || n != sizeof s_cfg) {
            T5_LOGW(TAG, "saved networks don't open (sealed on another chip?): starting with none");
            t5_netcfg_init(&s_cfg);
        }
    } else if (len == sizeof s_cfg) {
        memcpy(&s_cfg, blob, sizeof s_cfg);
        migrate = true;
    }
    free(blob);
    if (!t5_netcfg_valid(&s_cfg)) {
        t5_netcfg_init(&s_cfg);
        return;
    }
    if (migrate) {
        T5_LOGI(TAG, "sealing the saved networks with this chip's key");
        cfg_save();
    }
}

static void cfg_save(void)
{
    size_t cap = sizeof s_cfg + T5_SECRET_OVERHEAD, n = 0;
    uint8_t *sealed = malloc(cap);
    if (!sealed) return;
    if (t5_secret_seal(&s_cfg, sizeof s_cfg, sealed, cap, &n) != T5_OK) {
        T5_LOGE(TAG, "can't seal the saved networks: not saving");
        free(sealed);
        return;
    }
    nvs_handle_t h;
    if (net_nvs_open(NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, NVS_KEY, sealed, n) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    memset(sealed, 0, cap);
    free(sealed);
}

static void on_idf_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    msg_t m = {0};
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START: m.type = EV_STA_START; break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *e = data;
            m.type = EV_CONNECTED;
            m.arg = e->channel;
            memcpy(m.ssid, e->ssid, e->ssid_len < T5_NET_SSID_LEN ? e->ssid_len : T5_NET_SSID_LEN - 1);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e = data;
            m.type = EV_DISCONNECTED;
            m.arg = e->reason;
            break;
        }
        case WIFI_EVENT_SCAN_DONE: m.type = EV_SCAN_DONE; break;
        default: return;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        m.type = EV_GOT_IP;
        memcpy(m.ip, &e->ip_info.ip.addr, 4);
        memcpy(m.ip + 4, &e->ip_info.gw.addr, 4);
        memcpy(m.ip + 8, &e->ip_info.netmask.addr, 4);
    } else if (base == ESP_HOSTED_EVENT) {
        if (id == ESP_HOSTED_EVENT_TRANSPORT_DOWN) m.type = EV_LINK_DOWN;
        else if (id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) m.type = EV_LINK_FAIL;
        else return;
    } else {
        return;
    }
    xQueueSend(s_q, &m, 0);
}

static void IRAM_ATTR on_wake_isr(void *arg)
{
    (void)arg;
    msg_t m = { .type = EV_WAKE };
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_q, &m, &hp);
    portYIELD_FROM_ISR(hp);
}

static void on_sntp(struct timeval *tv)
{
    msg_t m = { .type = EV_SNTP, .arg = (int32_t)tv->tv_sec };
    xQueueSend(s_q, &m, 0);
}

static void fwversion(void)
{
    esp_hosted_coprocessor_fwver_t v = {0};
    char buf[32] = "?";
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK) {
        snprintf(buf, sizeof buf, "%lu.%lu.%lu", (unsigned long)v.major1, (unsigned long)v.minor1, (unsigned long)v.patch1);
    }
    esp_hosted_app_desc_t d = {0};
    if (esp_hosted_get_coprocessor_app_desc(&d) == ESP_OK && d.version[0]) {
        size_t n = strlen(buf);
        if (strncmp(buf, d.version, n)) snprintf(buf + n, sizeof buf - n, " (%.*s)", (int)(sizeof buf - n - 4), d.version);
    }
    t5_mutex_lock(s_mtx);
    scpy(s_st.c6_version, sizeof s_st.c6_version, buf, sizeof buf);
    t5_mutex_unlock(s_mtx);
}

/* The C6 runs while Wi-Fi or Bluetooth is on; the Wi-Fi driver (scans,
 * joining networks) only while Wi-Fi is. So Bluetooth works with Wi-Fi off. */
static bool wifi_wanted(void)
{
    return !(s_cfg.flags & T5_NETCFG_FLAG_DISABLED);
}

static bool radio_wanted(void)
{
    return wifi_wanted() || t5_ble_enabled();
}

/* Wi-Fi off, the C6 left up (for Bluetooth). */
static void wifi_down(void)
{
    bool connected = s_st.state == T5_NET_CONNECTED;
    if (s_wifi_up) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_up = false;
    }
    s_got_ip = false;
    s_scanning = false;
    s_ps = false;
    t5_mutex_lock(s_mtx);
    memset(s_st.ip, 0, sizeof s_st.ip);
    s_st.ssid[0] = 0;
    s_st.rssi = 0;
    s_st.power_save = false;
    t5_mutex_unlock(s_mtx);
    set_state(T5_NET_OFF);
    if (connected) post_ev(T5_EV_NET_DISCONNECTED, T5_OK);
}

static void power_down(void)
{
    s_t_link_change = t5_time_ms();
    if (s_hosted_up) t5_ble_on_link(false);
    if (s_wifi_up) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_up = false;
    }
    if (s_hosted_up) {
        esp_hosted_deinit();
        s_hosted_up = false;
    }
    if (s_rail) {
        T5_LOG_ON_ERR(TAG, t5_rail_release(s_rail));
        s_rail = NULL;
    }
    bool was = s_st.state != T5_NET_OFF && s_st.state != T5_NET_FAILED;
    s_got_ip = false;
    s_scanning = false;
    s_ps = false;
    t5_mutex_lock(s_mtx);
    memset(s_st.ip, 0, sizeof s_st.ip);
    s_st.ssid[0] = 0;
    s_st.rssi = 0;
    s_st.power_save = false;
    t5_mutex_unlock(s_mtx);
    set_state(T5_NET_OFF);
    if (was) post_ev(T5_EV_NET_C6_DOWN, T5_OK);
}

static bool rejoin_last(void);

static void start_scan(bool for_auto)
{
    if (!s_wifi_up || s_scanning) return;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        s_scanning = true;
        s_scan_for_auto = for_auto;
    }
}

static t5_err_t power_up(void)
{
    set_state(T5_NET_STARTING);
    s_t_power_on = t5_time_ms();
    if (!s_rail) {
        T5_RETURN_ON_ERR(t5_rail_acquire_system(T5_RAIL_WLAN, "net", T5_RAIL_KEEP_IN_SLEEP, &s_rail));
        t5_delay_ms(POWER_SETTLE_MS);
    }
    bool c6_new = !s_hosted_up;
    if (!s_hosted_up) {
        if (esp_hosted_init() != 0 || esp_hosted_connect_to_slave() != 0) {
            T5_LOGE(TAG, "C6 link did not come up");
            esp_hosted_deinit();
            return T5_ERR_TIMEOUT;
        }
        s_hosted_up = true;
    }
    if (!s_wifi_up && wifi_wanted()) {
        if (!s_netif) s_netif = esp_netif_create_default_wifi_sta();
        wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&wc) != ESP_OK) return T5_ERR_FAIL;
        esp_wifi_set_storage(WIFI_STORAGE_RAM);   /* credentials live in our NVS, not the C6's */
        esp_wifi_set_mode(WIFI_MODE_STA);
        if (esp_wifi_start() != ESP_OK) {
            esp_wifi_deinit();
            return T5_ERR_FAIL;
        }
        s_wifi_up = true;
    }
    if (!wifi_wanted()) {
        /* Bluetooth only: the C6 is up, the Wi-Fi driver stays off. */
        if (c6_new) {
            fwversion();
            s_t_link_change = t5_time_ms();
            T5_LOGI(TAG, "C6 up for Bluetooth (Wi-Fi off) in %u ms", (unsigned)(t5_time_ms() - s_t_power_on));
            post_ev(T5_EV_NET_C6_UP, T5_OK);
            s_health_fails = 0;
            s_next_health = t5_time_ms() + HEALTH_PERIOD_MS;
            s_start_backoff = START_RETRY_MIN_MS;
        }
        set_state(T5_NET_OFF);
        t5_ble_on_link(true);
        return T5_OK;
    }
    fwversion();
    s_t_link_change = t5_time_ms();
    uint32_t ms = t5_time_ms() - s_t_power_on;
    t5_mutex_lock(s_mtx);
    s_st.link_up_ms = ms;
    s_st.link_ups++;
    t5_mutex_unlock(s_mtx);
    T5_LOGI(TAG, "C6 up in %u ms, firmware %s", (unsigned)ms, s_st.c6_version);
    set_state(T5_NET_IDLE);
    post_ev(T5_EV_NET_C6_UP, T5_OK);
    t5_ble_on_link(true);
    s_health_fails = 0;
    s_next_health = t5_time_ms() + HEALTH_PERIOD_MS;
    s_start_backoff = START_RETRY_MIN_MS;
    s_target[0] = 0;
    if (s_auto && s_cfg.n && !rejoin_last()) start_scan(true);
    return T5_OK;
}

static void start(void)
{
    s_next_start = 0;
    if (power_up() != T5_OK) {
        power_down();
        set_state(T5_NET_FAILED);
        s_next_start = t5_time_ms() + s_start_backoff;
        T5_LOGW(TAG, "retrying the C6 in %u s", (unsigned)(s_start_backoff / 1000));
        s_start_backoff = s_start_backoff * 2 > START_RETRY_MAX_MS ? START_RETRY_MAX_MS : s_start_backoff * 2;
    }
}

static void reset_c6(const char *why)
{
    T5_LOGW(TAG, "resetting the C6: %s", why);
    t5_mutex_lock(s_mtx);
    s_st.c6_resets++;
    t5_mutex_unlock(s_mtx);
    power_down();
    t5_delay_ms(POWER_OFF_MS);
    start();
}

static void do_connect_hint(const char *ssid, const char *pass, uint8_t channel, const uint8_t *bssid)
{
    if (!s_wifi_up) return;
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, ssid, strnlen(ssid, sizeof wc.sta.ssid));
    if (pass) memcpy(wc.sta.password, pass, strnlen(pass, sizeof wc.sta.password));
    wc.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if (channel && bssid) {
        wc.sta.channel = channel;
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, bssid, 6);
        wc.sta.scan_method = WIFI_FAST_SCAN;
    } else {
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    }
    if (s_st.state == T5_NET_CONNECTED || s_st.state == T5_NET_CONNECTING) {
        s_expect_leave = true;
        esp_wifi_disconnect();
    }
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        T5_LOGE(TAG, "connect to '%s' could not start", ssid);
        return;
    }
    scpy(s_target, sizeof s_target, ssid, T5_NET_SSID_LEN);
    s_t_connect = t5_time_ms();
    s_got_ip = false;
    set_state(T5_NET_CONNECTING);
    T5_LOGI(TAG, "connecting to '%s'", ssid);
}

static void do_connect(const char *ssid, const char *pass)
{
    do_connect_hint(ssid, pass, 0, NULL);
}

/* Rejoin the last network fast, if it's still saved. */
static bool rejoin_last(void)
{
    int i = s_last_ssid[0] ? t5_netcfg_find(&s_cfg, s_last_ssid) : -1;
    if (i < 0) return false;
    s_pending_save = false;
    do_connect_hint(s_cfg.e[i].ssid, s_cfg.e[i].pass, s_last_channel, s_last_bssid);
    return true;
}

static void auto_pick(void)
{
    if (!s_auto || s_st.state == T5_NET_CONNECTED || s_st.state == T5_NET_CONNECTING || !s_cfg.n) return;
    int ap = -1;
    t5_mutex_lock(s_mtx);
    int i = t5_netcfg_pick(&s_cfg, &s_backoff, s_scan, s_nscan, t5_time_ms(), &ap);
    t5_mutex_unlock(s_mtx);
    if (i >= 0) {
        s_pending_save = false;
        do_connect(s_cfg.e[i].ssid, s_cfg.e[i].pass);
    } else {
        uint32_t now = t5_time_ms();
        uint32_t retry = t5_netcfg_next_retry(&s_backoff, now);
        s_next_scan = (int32_t)(retry - now) > (int32_t)RESCAN_MS ? retry : now + RESCAN_MS;
    }
}

static void on_scan_done(void)
{
    s_scanning = false;
    uint16_t n = T5_NET_MAX_SCAN;
    wifi_ap_record_t *rec = calloc(n, sizeof *rec);
    if (!rec) return;
    if (esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) n = 0;
    t5_mutex_lock(s_mtx);
    s_nscan = 0;
    for (int i = 0; i < n && s_nscan < T5_NET_MAX_SCAN; i++) {
        t5_net_ap_t *a = &s_scan[s_nscan];
        memset(a, 0, sizeof *a);
        memcpy(a->ssid, rec[i].ssid, sizeof a->ssid - 1);
        if (!a->ssid[0]) continue;
        memcpy(a->bssid, rec[i].bssid, 6);
        a->rssi = rec[i].rssi;
        a->channel = rec[i].primary;
        a->auth = (uint8_t)rec[i].authmode;
        a->saved = t5_netcfg_find(&s_cfg, a->ssid) >= 0;
        s_nscan++;
    }
    t5_mutex_unlock(s_mtx);
    free(rec);
    post_ev(T5_EV_NET_SCAN_DONE, T5_OK);
    if (s_scan_for_auto) auto_pick();
}

/* Start SNTP; if it runs already and again is set, ask now. */
static t5_err_t sntp_request(bool again)
{
    esp_err_t e = ESP_OK;
    t5_mutex_lock(s_mtx);
    if (!s_sntp_started) {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        sc.sync_cb = on_sntp;
        e = esp_netif_sntp_init(&sc);
        s_sntp_started = e == ESP_OK;
    } else if (again) {
        e = esp_netif_sntp_start();     /* restarts: asks again now */
    }
    t5_mutex_unlock(s_mtx);
    return e == ESP_OK ? T5_OK : T5_ERR_IO;
}

static void on_got_ip(const msg_t *m)
{
    s_got_ip = true;
    uint32_t ms = t5_time_ms() - s_t_connect;
    wifi_ap_record_t ap = {0};
    esp_wifi_sta_get_ap_info(&ap);
    esp_netif_dns_info_t dns = {0};
    if (s_netif) esp_netif_get_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    t5_mutex_lock(s_mtx);
    memcpy(s_st.ip, m->ip, 4);
    memcpy(s_st.gw, m->ip + 4, 4);
    memcpy(s_st.mask, m->ip + 8, 4);
    memcpy(s_st.dns, &dns.ip.u_addr.ip4.addr, 4);
    s_st.rssi = ap.rssi;
    s_st.connect_ms = ms;
    s_st.connects++;
    t5_mutex_unlock(s_mtx);
    set_state(T5_NET_CONNECTED);
    T5_LOGI(TAG, "'%s' up: %u.%u.%u.%u in %u ms, rssi %d", s_st.ssid, m->ip[0], m->ip[1], m->ip[2], m->ip[3],
            (unsigned)ms, ap.rssi);
    t5_netcfg_clear_fail(&s_backoff, s_st.ssid);
    scpy(s_last_ssid, sizeof s_last_ssid, s_st.ssid, sizeof s_st.ssid);
    memcpy(s_last_bssid, ap.bssid, 6);
    s_last_channel = ap.primary;
    t5_mutex_lock(s_mtx);
    if (s_pending_save) t5_netcfg_add(&s_cfg, s_st.ssid, s_pending_pass);
    bool saved = t5_netcfg_find(&s_cfg, s_st.ssid) >= 0;
    if (saved) t5_netcfg_mark_ok(&s_cfg, s_st.ssid);
    t5_mutex_unlock(s_mtx);
    s_pending_save = false;
    if (saved) cfg_save();
    memset(s_pending_pass, 0, sizeof s_pending_pass);
    T5_IGNORE(sntp_request(false));     /* NTP is retried on the next address */
    post_ev(T5_EV_NET_GOT_IP, T5_OK);
}

static void on_disconnected(int reason)
{
    if (reason == WIFI_REASON_ASSOC_LEAVE &&
        (s_expect_leave || (s_st.state == T5_NET_CONNECTING && t5_time_ms() - s_t_connect < 300))) {
        /* Our own disconnect before a new connect, or the C6 dropping its
         * previous association as the new connect starts. */
        s_expect_leave = false;
        return;
    }
    bool was_up = s_got_ip;
    s_got_ip = false;
    t5_mutex_lock(s_mtx);
    memset(s_st.ip, 0, sizeof s_st.ip);
    s_st.disconnects++;
    t5_mutex_unlock(s_mtx);
    post_ev(T5_EV_NET_DISCONNECTED, reason);
    if (!s_wifi_up) return;
    bool attempting = s_st.state == T5_NET_CONNECTING;
    set_state(T5_NET_IDLE);
    uint32_t now = t5_time_ms();
    if (!was_up && !attempting) {
        return;   /* not ours: e.g. the C6 dropping a stale association at boot */
    }
    if (!was_up && s_target[0]) {
        if (s_last_channel && !strcmp(s_target, s_last_ssid)) {
            /* The fast rejoin missed (AP moved channel?): forget the hint, rescan. */
            s_last_channel = 0;
        }
        /* Never got an address: wrong password, out of range, refused. */
        T5_LOGW(TAG, "'%s' failed (reason %d)", s_target, reason);
        t5_netcfg_fail(&s_backoff, s_target, now);
        s_pending_save = false;
        memset(s_pending_pass, 0, sizeof s_pending_pass);
    } else {
        T5_LOGW(TAG, "lost '%s' (reason %d)", s_target, reason);
    }
    if (s_auto) {
        /* A link that was up: try again soon. Otherwise rescan and pick. */
        s_next_connect = was_up ? now + 1000 : 0;
        if (!was_up) s_next_scan = now + 1000;
    }
}

static t5_err_t part_read(void *ctx, uint32_t off, void *buf, uint32_t len)
{
    return esp_partition_read((const esp_partition_t *)ctx, off, buf, len) == ESP_OK ? T5_OK : T5_ERR_IO;
}

static const esp_partition_t *c6_part(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, C6_PART_LABEL);
}

static void read_image_version(void)
{
    const esp_partition_t *p = c6_part();
    t5_c6img_info_t info;
    t5_mutex_lock(s_mtx);
    s_st.c6_image[0] = 0;
    if (p && t5_c6img_parse(part_read, (void *)p, p->size, &info) == T5_OK && info.chip_id == T5_C6_CHIP_ID) {
        scpy(s_st.c6_image, sizeof s_st.c6_image, info.version, sizeof info.version);
    }
    t5_mutex_unlock(s_mtx);
}

/* Which firmware the C6 runs. Our radio firmware names its build
 * ("t5-c6-0.2.0", the version in its image); esp_hosted only reports its
 * own protocol version ("3.1.0"), the same for every build, which is all
 * there is for firmware that doesn't answer. A build name also goes into
 * the status. */
static bool running_version(char *out, size_t cap)
{
    char build[32];
    if (wifimon_c6_build(build, sizeof build) == T5_OK && build[0]) {
        scpy(out, cap, build, sizeof build);
        t5_mutex_lock(s_mtx);
        scpy(s_st.c6_version, sizeof s_st.c6_version, build, sizeof build);
        t5_mutex_unlock(s_mtx);
        return true;
    }
    esp_hosted_app_desc_t d = {0};
    if (esp_hosted_get_coprocessor_app_desc(&d) != ESP_OK) return false;
    scpy(out, cap, d.version, sizeof d.version);
    return true;
}

static t5_err_t c6_update(bool force)
{
    const esp_partition_t *p = c6_part();
    t5_c6img_info_t info;
    if (!p) return T5_ERR_NOT_FOUND;
    t5_err_t err = t5_c6img_parse(part_read, (void *)p, p->size, &info);
    if (err != T5_OK) return err;
    if (info.chip_id != T5_C6_CHIP_ID) return T5_ERR_INVALID_ARG;
    if (!s_hosted_up) return T5_ERR_STATE;
    char before[32] = "";
    running_version(before, sizeof before);
    if (!force && !strcmp(before, info.version)) {
        T5_LOGI(TAG, "C6 already runs %s", before);
        return T5_OK;
    }
    T5_LOGW(TAG, "updating the C6: %s -> %s (%u bytes)", before[0] ? before : "?", info.version, (unsigned)info.size);
    uint8_t *buf = malloc(EH_HOST_CP_OTA_CHUNK_MAX);
    if (!buf) return T5_ERR_NO_MEM;
    err = esp_hosted_cp_ota_begin() == ESP_OK ? T5_OK : T5_ERR_FAIL;
    for (uint32_t off = 0; err == T5_OK && off < info.size; off += EH_HOST_CP_OTA_CHUNK_MAX) {
        uint32_t n = info.size - off < EH_HOST_CP_OTA_CHUNK_MAX ? info.size - off : EH_HOST_CP_OTA_CHUNK_MAX;
        if (esp_partition_read(p, off, buf, n) != ESP_OK || esp_hosted_cp_ota_write(buf, n) != ESP_OK) {
            err = T5_ERR_IO;
        }
    }
    free(buf);
    if (err == T5_OK && esp_hosted_cp_ota_end() != ESP_OK) err = T5_ERR_FAIL;
    if (err == T5_OK && esp_hosted_cp_ota_activate() != ESP_OK) err = T5_ERR_FAIL;
    if (err != T5_OK) {
        T5_LOGE(TAG, "C6 update write failed: %s; the C6 keeps %s", t5_err_str(err), before);
        return err;
    }
    /* Reboot the C6 into the new image and check it came back with it. If it
     * doesn't, a second power cycle lets the C6 bootloader roll back. */
    for (int attempt = 0; attempt < 2; attempt++) {
        reset_c6(attempt ? "rollback check" : "apply update");      /* the link is up, or not, when it returns */
        char now[32] = "";
        if (s_hosted_up && running_version(now, sizeof now)) {
            if (!strcmp(now, info.version)) {
                T5_LOGI(TAG, "C6 updated to %s", now);
                return T5_OK;
            }
            T5_LOGE(TAG, "C6 came back with %s, not %s: rolled back", now, info.version);
            return T5_ERR_FAIL;
        }
    }
    T5_LOGE(TAG, "C6 did not come back after the update");
    return T5_ERR_TIMEOUT;
}

static void suspend(void)
{
    if (t5_wakelock_count(T5_LOCK_NET) > 0 && s_hosted_up && !s_wifi_up) {
        /* Bluetooth only, with a link or an app's handle open: keep the C6. */
        T5_LOGI(TAG, "sleep: C6 kept up for Bluetooth");
    } else if (t5_wakelock_count(T5_LOCK_NET) > 0 && s_wifi_up) {
        /* With connections open (a download, a socket an app is using),
         * the light power save: MAX_MODEM sleeps through DTIM intervals
         * and cuts throughput to a trickle. */
        bool busy = t5_net_handles_open() > 0;
        esp_wifi_set_ps(busy ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM);
        s_ps = true;
        t5_mutex_lock(s_mtx);
        s_st.power_save = true;
        t5_mutex_unlock(s_mtx);
        T5_LOGI(TAG, "sleep: Wi-Fi kept up (%s)", busy ? "connections open" : "net lock held, power save");
    } else if (s_hosted_up || s_st.state != T5_NET_OFF) {
        s_suspended_off = true;
        power_down();
        T5_LOGI(TAG, "sleep: C6 off");
    }
}

static void resume(void)
{
    if (s_ps && s_wifi_up) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        s_ps = false;
        t5_mutex_lock(s_mtx);
        s_st.power_save = false;
        t5_mutex_unlock(s_mtx);
    }
    if (s_suspended_off) {
        s_suspended_off = false;
        if (radio_wanted()) start();
    }
}

static t5_err_t hook_suspend(void *arg)
{
    (void)arg;
    msg_t m = { .type = CMD_SUSPEND };
    xSemaphoreTake(s_done, 0);
    if (send(&m) != T5_OK) return T5_ERR_BUSY;
    return xSemaphoreTake(s_done, pdMS_TO_TICKS(SUSPEND_WAIT_MS)) == pdTRUE ? T5_OK : T5_ERR_TIMEOUT;
}

static t5_err_t hook_resume(void *arg)
{
    (void)arg;
    msg_t m = { .type = CMD_RESUME };
    return send(&m);   /* reconnecting happens in the background */
}

static void handle(const msg_t *m)
{
    switch (m->type) {
    case CMD_START:
        if (s_cfg.flags & T5_NETCFG_FLAG_DISABLED) {
            t5_mutex_lock(s_mtx);
            s_cfg.flags &= ~T5_NETCFG_FLAG_DISABLED;
            t5_mutex_unlock(s_mtx);
            cfg_save();
        }
        if (s_st.state == T5_NET_OFF || s_st.state == T5_NET_FAILED) start();   /* the C6 may be up already, for Bluetooth */
        break;
    case CMD_STOP:
        t5_mutex_lock(s_mtx);
        s_cfg.flags |= T5_NETCFG_FLAG_DISABLED;
        t5_mutex_unlock(s_mtx);
        cfg_save();
        s_next_start = 0;
        if (t5_ble_enabled() && s_hosted_up) wifi_down();    /* Bluetooth keeps the C6 */
        else power_down();
        break;
    case CMD_SCAN:
        start_scan(false);
        break;
    case CMD_CONNECT:
        if (s_held) break;                  /* an app's monitor has the radio */
        s_auto = true;
        s_pending_save = m->flag;
        scpy(s_pending_pass, sizeof s_pending_pass, m->pass, sizeof m->pass);
        t5_netcfg_clear_fail(&s_backoff, m->ssid);
        do_connect(m->ssid, m->pass);
        break;
    case CMD_DISCONNECT:
        s_auto = false;
        s_target[0] = 0;
        if (s_wifi_up) {
            s_expect_leave = true;
            esp_wifi_disconnect();
        }
        break;
    case CMD_FORGET: {
        t5_mutex_lock(s_mtx);
        bool removed = t5_netcfg_remove(&s_cfg, m->ssid);
        t5_mutex_unlock(s_mtx);
        if (removed) cfg_save();
        if (!strcmp(s_last_ssid, m->ssid)) s_last_ssid[0] = 0;
        if (!strcmp(s_st.ssid, m->ssid) && s_wifi_up) {
            s_target[0] = 0;
            esp_wifi_disconnect();
        }
        break;
    }
    case CMD_RESET:
        reset_c6("requested");
        break;
    case CMD_SUSPEND:
        suspend();
        xSemaphoreGive(s_done);
        break;
    case CMD_RESUME:
        resume();
        break;
    case CMD_UPDATE: {
        t5_err_t e = c6_update(m->flag);
        read_image_version();
        post_ev(T5_EV_NET_C6_UPDATE, e);
        break;
    }
    case CMD_ANTENNA:
        t5_xpin_output(T5_XP_RF_ANT_SEL, m->arg == T5_ANT_EXTERNAL);
        t5_mutex_lock(s_mtx);
        s_cfg.antenna = (uint8_t)m->arg;
        t5_mutex_unlock(s_mtx);
        cfg_save();
        break;
    case CMD_BLE:
        /* Bluetooth on powers the C6 even with Wi-Fi off; off powers it down
         * unless Wi-Fi still wants it. */
        if (t5_ble_enabled() && !s_hosted_up && !s_suspended_off) start();
        else if (!radio_wanted() && s_hosted_up) power_down();
        else t5_ble_on_link(s_hosted_up);
        break;
    case CMD_HOLD:
        if (m->flag && !s_held) {
            s_held = true;
            s_auto = false;
            if (s_wifi_up && (s_st.state == T5_NET_CONNECTED || s_st.state == T5_NET_CONNECTING)) {
                s_expect_leave = true;
                esp_wifi_disconnect();
            }
            set_state(T5_NET_IDLE);
            T5_LOGW(TAG, "station paused: an app has the radio");
        } else if (!m->flag && s_held) {
            s_held = false;
            s_auto = true;
            T5_LOGI(TAG, "station back");
            if (s_wifi_up && !rejoin_last()) s_next_scan = t5_time_ms();
        }
        break;
    case EV_STA_START:
        break;
    case EV_CONNECTED:
        t5_mutex_lock(s_mtx);
        scpy(s_st.ssid, sizeof s_st.ssid, m->ssid, sizeof m->ssid);
        s_st.channel = (uint8_t)m->arg;
        t5_mutex_unlock(s_mtx);
        post_ev(T5_EV_NET_CONNECTED, T5_OK);
        break;
    case EV_DISCONNECTED:
        on_disconnected(m->arg);
        break;
    case EV_GOT_IP:
        on_got_ip(m);
        break;
    case EV_SCAN_DONE:
        /* A held radio's scan is an app's (wifi_excl.c): its results are its own. */
        if (s_held) s_scanning = false;
        else on_scan_done();
        break;
    case EV_LINK_DOWN:
    case EV_LINK_FAIL:
        /* Our own teardown posts a transport-down too; it arrives after the
         * link is already back up. Only a drop we didn't cause counts. */
        if (t5_time_ms() - s_t_link_change < 3000) break;
        if (s_hosted_up && !s_suspended_off) reset_c6(m->type == EV_LINK_FAIL ? "transport failure" : "transport down");
        break;
    case EV_WAKE:
        /* A C6 that Sleep powered off can't be asking for us: the edge was
         * latched while it went down (data it had queued, or the rail
         * falling). Waking for it powered the C6 straight back on, and
         * Sleep then never lasted. */
        if (s_suspended_off || !s_hosted_up) break;
        t5_mutex_lock(s_mtx);
        s_st.wakes++;
        t5_mutex_unlock(s_mtx);
        t5_power_wake(T5_PWR_CAUSE_WAKE_NET, WAKE_CPU_MS);
        post_ev(T5_EV_NET_WAKE, T5_OK);
        break;
    case EV_SNTP:
        if (t5_time_set((time_t)m->arg, T5_TIME_SRC_NTP) == T5_OK) {
            if (!s_st.sntp_synced) T5_LOGI(TAG, "time from NTP");
            t5_mutex_lock(s_mtx);
            s_st.sntp_synced = true;
            t5_mutex_unlock(s_mtx);
        }
        break;
    }
}

static void timers(void)
{
    uint32_t now = t5_time_ms();
    if (due(s_next_start, now) && radio_wanted()) {
        s_next_start = 0;
        start();
    }
    if (s_hosted_up && due(s_next_health, now)) {
        s_next_health = now + HEALTH_PERIOD_MS;
        esp_hosted_coprocessor_fwver_t v;
        if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK) {
            s_health_fails = 0;
        } else {
            t5_mutex_lock(s_mtx);
            s_st.health_failures++;
            t5_mutex_unlock(s_mtx);
            if (++s_health_fails >= HEALTH_FAILS_TO_RESET) reset_c6("health check failed");
        }
        if (s_got_ip) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                t5_mutex_lock(s_mtx);
                s_st.rssi = ap.rssi;
                t5_mutex_unlock(s_mtx);
            }
        }
    }
    if (!s_wifi_up) return;
    if (due(s_next_connect, now)) {
        s_next_connect = 0;
        if (s_auto && s_target[0] && s_st.state != T5_NET_CONNECTED) do_connect(s_target, NULL);
    }
    if (due(s_next_scan, now)) {
        s_next_scan = 0;
        if (s_auto && s_st.state == T5_NET_IDLE) start_scan(true);
    }
}

static void net_task(void *arg)
{
    (void)arg;
    if (radio_wanted()) start();
    for (;;) {
        msg_t m;
        if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(500)) == pdTRUE) handle(&m);
        timers();
    }
}

t5_err_t t5_net_init(void)
{
    wifi_excl_init();
    if (s_inited) return T5_OK;
    esp_err_t e = nvs_ready();
    if (e != ESP_OK) T5_LOGE(TAG, "nvs: %s", esp_err_to_name(e));
    s_mtx = t5_mutex_create();
    s_q = xQueueCreate(16, sizeof(msg_t));
    s_done = xSemaphoreCreateBinary();
    if (!s_mtx || !s_q || !s_done) return T5_ERR_NO_MEM;
    t5_mutex_lock(s_mtx);
    cfg_load();
    s_st.enabled = !(s_cfg.flags & T5_NETCFG_FLAG_DISABLED);
    t5_mutex_unlock(s_mtx);
    t5_xpin_output(T5_XP_RF_ANT_SEL, s_cfg.antenna == T5_ANT_EXTERNAL);
    read_image_version();

    if (esp_netif_init() != ESP_OK) return T5_ERR_FAIL;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return T5_ERR_FAIL;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_idf_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_idf_event, NULL);
    esp_event_handler_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, on_idf_event, NULL);

    /* C6 wake line (IO2 -> G14), bench check 12. Pulled down so an idle C6
     * that doesn't drive it can't wake us. */
    gpio_config_t io = { .pin_bit_mask = 1ULL << T5_PIN_C6_WAKE, .mode = GPIO_MODE_INPUT,
                         .pull_down_en = GPIO_PULLDOWN_ENABLE, .intr_type = GPIO_INTR_POSEDGE };
    if (gpio_config(&io) == ESP_OK) {
        e = gpio_install_isr_service(0);
        if (e == ESP_OK || e == ESP_ERR_INVALID_STATE) gpio_isr_handler_add(T5_PIN_C6_WAKE, on_wake_isr, NULL);
    }

    const t5_suspend_hook_t hook = { .name = "net", .prio = T5_SUSPEND_PRIO_NET, .budget_ms = SUSPEND_BUDGET_MS,
                                     .suspend = hook_suspend, .resume = hook_resume };
    T5_LOG_ON_ERR(TAG, t5_power_register_hook(&hook));
    s_inited = true;
    T5_LOGI(TAG, "%d saved network(s), radio %s, antenna %s, c6fw image %s", s_cfg.n,
            s_st.enabled ? "on" : "off", s_cfg.antenna == T5_ANT_EXTERNAL ? "external" : "internal",
            s_st.c6_image[0] ? s_st.c6_image : "none");
    return t5_task_create_lp("t5_net", net_task, NULL, 6144, 7, 0, NULL);
}

void t5_net_status(t5_net_status_t *out)
{
    if (!s_mtx) {
        memset(out, 0, sizeof *out);
        return;
    }
    t5_mutex_lock(s_mtx);
    *out = s_st;
    out->enabled = !(s_cfg.flags & T5_NETCFG_FLAG_DISABLED);
    t5_mutex_unlock(s_mtx);
}

t5_err_t t5_net_enable(bool on)
{
    msg_t m = { .type = on ? CMD_START : CMD_STOP };
    return send(&m);
}

t5_err_t t5_net_scan(void)
{
    msg_t m = { .type = CMD_SCAN };
    return send(&m);
}

int t5_net_scan_results(t5_net_ap_t *out, int max)
{
    if (!s_mtx) return 0;
    t5_mutex_lock(s_mtx);
    int n = s_nscan < max ? s_nscan : max;
    memcpy(out, s_scan, (size_t)n * sizeof *out);
    t5_mutex_unlock(s_mtx);
    return n;
}

t5_err_t t5_net_connect(const char *ssid, const char *pass, bool save)
{
    if (!ssid || !ssid[0] || strlen(ssid) >= T5_NET_SSID_LEN || (pass && strlen(pass) >= T5_NET_PASS_LEN)) {
        return T5_ERR_INVALID_ARG;
    }
    msg_t m = { .type = CMD_CONNECT, .flag = save };
    strcpy(m.ssid, ssid);
    if (pass) strcpy(m.pass, pass);
    t5_err_t e = send(&m);
    memset(&m, 0, sizeof m);
    return e;
}

t5_err_t net_station_hold(bool on)
{
    msg_t m = { .type = CMD_HOLD, .flag = on };
    return send(&m);
}

bool net_wifi_up(void)
{
    return s_wifi_up;
}

t5_err_t t5_net_disconnect(void)
{
    msg_t m = { .type = CMD_DISCONNECT };
    return send(&m);
}

t5_err_t t5_net_forget(const char *ssid)
{
    if (!ssid || strlen(ssid) >= T5_NET_SSID_LEN) return T5_ERR_INVALID_ARG;
    msg_t m = { .type = CMD_FORGET };
    strcpy(m.ssid, ssid);
    return send(&m);
}

int t5_net_saved_list(char (*ssids)[T5_NET_SSID_LEN], int max)
{
    if (!s_mtx) return 0;
    t5_mutex_lock(s_mtx);
    int total = s_cfg.n;
    int n = total < max ? total : max;
    for (int i = 0; ssids && i < n; i++) memcpy(ssids[i], s_cfg.e[i].ssid, T5_NET_SSID_LEN);
    t5_mutex_unlock(s_mtx);
    return total;
}

t5_err_t t5_net_sync_time(void)
{
    if (!s_mtx) return T5_ERR_STATE;
    t5_mutex_lock(s_mtx);
    bool up = s_st.state == T5_NET_CONNECTED;
    t5_mutex_unlock(s_mtx);
    return up ? sntp_request(true) : T5_ERR_STATE;
}

t5_err_t t5_net_c6_reset(void)
{
    msg_t m = { .type = CMD_RESET };
    return send(&m);
}

t5_err_t t5_net_c6_running(char *out, size_t n)
{
    if (n) out[0] = 0;
    read_image_version();                   /* c6fw may have been rewritten since */
    if (!s_hosted_up) return T5_ERR_STATE;
    return running_version(out, n) ? T5_OK : T5_ERR_IO;
}

t5_err_t t5_net_c6_update(bool force)
{
    msg_t m = { .type = CMD_UPDATE, .flag = force };
    return send(&m);
}

void net_ble_changed(void)
{
    msg_t m = { .type = CMD_BLE };
    send(&m);
}

t5_err_t t5_radio_antenna_set(t5_antenna_t ant)
{
    if (ant != T5_ANT_INTERNAL && ant != T5_ANT_EXTERNAL) return T5_ERR_INVALID_ARG;
    msg_t m = { .type = CMD_ANTENNA, .arg = ant };
    return send(&m);
}

t5_antenna_t t5_radio_antenna(void)
{
    if (!s_mtx) return T5_ANT_INTERNAL;
    t5_mutex_lock(s_mtx);
    bool ext = s_cfg.antenna == T5_ANT_EXTERNAL;
    t5_mutex_unlock(s_mtx);
    return ext ? T5_ANT_EXTERNAL : T5_ANT_INTERNAL;
}
