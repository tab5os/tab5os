/* SPDX-License-Identifier: Apache-2.0
 * Exclusive Wi-Fi (the "radio" part of t5_net/wifimon.h): the app runs the
 * C6's Wi-Fi through esp_wifi_remote while the net task's station is held.
 * The net task still handles the station's events (connected, address,
 * lost), so a network the app joins gets the OS's netif and sockets; with
 * the station held it never reconnects by itself.
 *
 * Taking the radio saves what the app may change; giving it back restores
 * that, stops a soft AP, ends a monitor and lets the station rejoin.
 */
#include <string.h>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "t5_core/excl_sys.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_net/net.h"
#include "t5_net/wifimon.h"
#include "net_priv.h"

static const char *TAG = "t5.wifiex";

static struct {
    bool saved;
    wifi_country_t country;
    int8_t power;             /* quarter dBm */
    uint8_t proto;
    wifi_bandwidth_t bw;
    uint8_t mac[6];
    bool ap_on;
} s_save;
static esp_netif_t *s_ap_netif;

static t5_err_t esp(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return T5_OK;
    case ESP_ERR_INVALID_ARG: return T5_ERR_INVALID_ARG;
    case ESP_ERR_NOT_SUPPORTED: return T5_ERR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return T5_ERR_TIMEOUT;
    case ESP_ERR_NO_MEM: return T5_ERR_NO_MEM;
    default: return T5_ERR_IO;
    }
}

static bool connected(void)
{
    t5_net_status_t ns;
    t5_net_status(&ns);
    return ns.state == T5_NET_CONNECTED || ns.state == T5_NET_CONNECTING;
}

static t5_err_t excl_take(void)
{
    if (!net_wifi_up()) return T5_ERR_STATE;           /* Wi-Fi off in Settings */
    wifimon_end_for_excl();                             /* another app's monitor ends */
    T5_RETURN_ON_ERR(net_station_hold(true));
    for (int i = 0; i < 40 && connected(); i++) t5_delay_ms(50);
    memset(&s_save, 0, sizeof s_save);
    s_save.saved = esp_wifi_get_country(&s_save.country) == ESP_OK && esp_wifi_get_max_tx_power(&s_save.power) == ESP_OK &&
                   esp_wifi_get_protocol(WIFI_IF_STA, &s_save.proto) == ESP_OK &&
                   esp_wifi_get_bandwidth(WIFI_IF_STA, &s_save.bw) == ESP_OK &&
                   esp_wifi_get_mac(WIFI_IF_STA, s_save.mac) == ESP_OK;
    T5_LOGW(TAG, "an app has the Wi-Fi radio");
    return T5_OK;
}

static void excl_give(void)
{
    wifimon_end_for_excl();
    esp_wifi_disconnect();
    if (s_save.ap_on) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_save.ap_on = false;
    }
    if (s_save.saved) {
        esp_wifi_set_country(&s_save.country);
        esp_wifi_set_max_tx_power(s_save.power);
        esp_wifi_set_protocol(WIFI_IF_STA, s_save.proto);
        esp_wifi_set_bandwidth(WIFI_IF_STA, s_save.bw);
        esp_wifi_set_mac(WIFI_IF_STA, s_save.mac);
    }
    T5_IGNORE(net_station_hold(false));                 /* rejoins the saved network */
    T5_LOGI(TAG, "Wi-Fi back to the system");
}

void wifi_excl_init(void)
{
    static const t5_excl_provider_t prov = { excl_take, excl_give };
    t5_excl_set_provider(T5_EXCL_WIFI, &prov);
}

#define CHECK(h) \
    if (!t5_excl_holds(h, T5_EXCL_WIFI)) return T5_ERR_STATE

int t5_wifi_excl_scan(t5_excl_t *h, const t5_wifi_scan_cfg_t *cfg, t5_wifi_ap_t *out, int max)
{
    if (!cfg || cfg->size < sizeof *cfg || cfg->channel > 13 || (max && !out) || max < 0) return T5_ERR_INVALID_ARG;
    CHECK(h);
    wifi_scan_config_t sc = { .channel = cfg->channel, .show_hidden = cfg->show_hidden,
                              .scan_type = cfg->passive ? WIFI_SCAN_TYPE_PASSIVE : WIFI_SCAN_TYPE_ACTIVE };
    if (cfg->dwell_ms) {
        if (cfg->passive) sc.scan_time.passive = cfg->dwell_ms;
        else sc.scan_time.active.min = sc.scan_time.active.max = cfg->dwell_ms;
    }
    T5_RETURN_ON_ERR(esp(esp_wifi_scan_start(&sc, true)));
    uint16_t n = (uint16_t)(max > 64 ? 64 : max);
    wifi_ap_record_t *rec = n ? calloc(n, sizeof *rec) : NULL;
    if (n && !rec) {
        esp_wifi_clear_ap_list();
        return T5_ERR_NO_MEM;
    }
    t5_err_t e = n ? esp(esp_wifi_scan_get_ap_records(&n, rec)) : esp(esp_wifi_clear_ap_list());
    for (int i = 0; e == T5_OK && i < n; i++) {
        memset(&out[i], 0, sizeof out[i]);
        memcpy(out[i].bssid, rec[i].bssid, 6);
        memcpy(out[i].ssid, rec[i].ssid, 32);
        out[i].rssi = rec[i].rssi;
        out[i].channel = rec[i].primary;
        out[i].auth = (uint8_t)rec[i].authmode;
    }
    free(rec);
    return e == T5_OK ? n : e;
}

t5_err_t t5_wifi_excl_connect(t5_excl_t *h, const char *ssid, const char *pass, uint32_t timeout_ms)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pass && strlen(pass) > 63)) return T5_ERR_INVALID_ARG;
    CHECK(h);
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid, ssid, strlen(ssid));
    if (pass) memcpy(wc.sta.password, pass, strlen(pass));
    wc.sta.threshold.authmode = pass && pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    esp_wifi_disconnect();
    T5_RETURN_ON_ERR(esp(esp_wifi_set_config(WIFI_IF_STA, &wc)));
    T5_RETURN_ON_ERR(esp(esp_wifi_connect()));
    int64_t until = t5_time_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        t5_net_status_t ns;
        t5_net_status(&ns);
        if (ns.state == T5_NET_CONNECTED && ns.ip[0]) return T5_OK;
        if (!t5_excl_holds(h, T5_EXCL_WIFI)) return T5_ERR_STATE;
        if (t5_time_us() >= until) {
            esp_wifi_disconnect();
            return T5_ERR_TIMEOUT;
        }
        t5_delay_ms(100);
    }
}

t5_err_t t5_wifi_excl_disconnect(t5_excl_t *h)
{
    CHECK(h);
    return esp(esp_wifi_disconnect());
}

t5_err_t t5_wifi_excl_ap_start(t5_excl_t *h, const t5_wifi_ap_cfg_t *cfg)
{
    if (!cfg || cfg->size < sizeof *cfg || !cfg->ssid || !cfg->ssid[0] || strlen(cfg->ssid) > 32) return T5_ERR_INVALID_ARG;
    size_t pl = cfg->pass ? strlen(cfg->pass) : 0;
    if ((pl && (pl < 8 || pl > 63)) || cfg->channel > 13 || cfg->max_clients > 8) return T5_ERR_INVALID_ARG;
    CHECK(h);
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return T5_ERR_NOT_SUPPORTED;
    wifi_config_t wc = { 0 };
    memcpy(wc.ap.ssid, cfg->ssid, strlen(cfg->ssid));
    wc.ap.ssid_len = (uint8_t)strlen(cfg->ssid);
    if (pl) memcpy(wc.ap.password, cfg->pass, pl);
    wc.ap.authmode = pl ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.ap.channel = cfg->channel ? cfg->channel : 6;
    wc.ap.max_connection = cfg->max_clients ? cfg->max_clients : 4;
    wc.ap.ssid_hidden = cfg->hidden;
    wc.ap.pmf_cfg.capable = true;
    t5_err_t e = esp(esp_wifi_set_mode(WIFI_MODE_APSTA));
    if (e == T5_OK) e = esp(esp_wifi_set_config(WIFI_IF_AP, &wc));
    if (e == T5_OK) s_save.ap_on = true;
    else esp_wifi_set_mode(WIFI_MODE_STA);
    return e;
}

t5_err_t t5_wifi_excl_ap_stop(t5_excl_t *h)
{
    CHECK(h);
    if (!s_save.ap_on) return T5_OK;
    s_save.ap_on = false;
    return esp(esp_wifi_set_mode(WIFI_MODE_STA));
}

int t5_wifi_excl_ap_clients(t5_excl_t *h, uint8_t (*mac)[6], int max)
{
    if ((max && !mac) || max < 0) return T5_ERR_INVALID_ARG;
    CHECK(h);
    if (!s_save.ap_on) return T5_ERR_STATE;
    wifi_sta_list_t l;
    T5_RETURN_ON_ERR(esp(esp_wifi_ap_get_sta_list(&l)));
    int n = l.num < max ? l.num : max;
    for (int i = 0; i < n; i++) memcpy(mac[i], l.sta[i].mac, 6);
    return n;
}

t5_err_t t5_wifi_excl_radio(t5_excl_t *h, const t5_wifi_radio_t *set, t5_wifi_radio_t *get)
{
    if ((set && set->size < sizeof *set) || (get && get->size < sizeof *get)) return T5_ERR_INVALID_ARG;
    CHECK(h);
    t5_err_t e = T5_OK;
    if (set && set->country[0]) {
        e = esp(esp_wifi_set_country_code(set->country, true));
    }
    if (e == T5_OK && set && set->tx_power_dbm) {
        if (set->tx_power_dbm < 2 || set->tx_power_dbm > 20) return T5_ERR_INVALID_ARG;
        e = esp(esp_wifi_set_max_tx_power((int8_t)(set->tx_power_dbm * 4)));
    }
    if (e == T5_OK && set && set->protocols) e = esp(esp_wifi_set_protocol(WIFI_IF_STA, set->protocols));
    if (e == T5_OK && set && set->bandwidth) {
        if (set->bandwidth != 20 && set->bandwidth != 40) return T5_ERR_INVALID_ARG;
        e = esp(esp_wifi_set_bandwidth(WIFI_IF_STA, set->bandwidth == 40 ? WIFI_BW40 : WIFI_BW20));
    }
    if (e == T5_OK && get) {
        char cc[3] = "";
        int8_t pw = 0;
        uint8_t proto = 0;
        wifi_bandwidth_t bw = WIFI_BW20;
        esp_wifi_get_country_code(cc);
        esp_wifi_get_max_tx_power(&pw);
        esp_wifi_get_protocol(WIFI_IF_STA, &proto);
        esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
        uint32_t size = get->size;
        memset(get, 0, sizeof *get);
        get->size = size;
        memcpy(get->country, cc, 2);
        get->tx_power_dbm = (int8_t)(pw / 4);
        get->protocols = proto;
        get->bandwidth = bw == WIFI_BW40 ? 40 : 20;
    }
    return e;
}

t5_err_t t5_wifi_excl_mac(t5_excl_t *h, uint8_t mac[6], bool set)
{
    if (!mac) return T5_ERR_INVALID_ARG;
    CHECK(h);
    if (!set) return esp(esp_wifi_get_mac(WIFI_IF_STA, mac));
    if ((mac[0] & 0x01) || !(mac[0] & 0x02)) return T5_ERR_INVALID_ARG;   /* unicast, locally administered */
    if (connected()) return T5_ERR_STATE;
    return esp(esp_wifi_set_mac(WIFI_IF_STA, mac));
}
