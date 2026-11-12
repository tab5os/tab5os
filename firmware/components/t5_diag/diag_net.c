/* SPDX-License-Identifier: Apache-2.0
 * net: Phase 3 network service.
 */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "t5_core/os.h"
#include "t5_net/ble_sys.h"
#include "t5_net/net.h"
#include "t5_net/sock.h"
#include "t5_net/wifimon.h"
#include "t5_power/power.h"
#include "diag_priv.h"

static void print_status(void)
{
    t5_net_status_t s;
    t5_net_status(&s);
    printf("state %s, radio %s, antenna %s%s\n", t5_net_state_name(s.state), s.enabled ? "on" : "off",
           t5_radio_antenna() == T5_ANT_EXTERNAL ? "external" : "internal", s.power_save ? ", power save" : "");
    printf("C6 firmware %s, c6fw image %s\n", s.c6_version[0] ? s.c6_version : "-", s.c6_image[0] ? s.c6_image : "none");
    if (s.state == T5_NET_CONNECTED || s.state == T5_NET_CONNECTING) {
        printf("ssid '%s' ch %u rssi %d\n", s.ssid, s.channel, s.rssi);
    }
    if (s.state == T5_NET_CONNECTED) {
        printf("ip %u.%u.%u.%u gw %u.%u.%u.%u mask %u.%u.%u.%u dns %u.%u.%u.%u, ntp %s\n", s.ip[0], s.ip[1], s.ip[2],
               s.ip[3], s.gw[0], s.gw[1], s.gw[2], s.gw[3], s.mask[0], s.mask[1], s.mask[2], s.mask[3], s.dns[0],
               s.dns[1], s.dns[2], s.dns[3], s.sntp_synced ? "synced" : "not yet");
    }
    printf("link up %u ms (x%u), connect %u ms, C6 resets %u, health failures %u, connects %u, disconnects %u, "
           "wakes %u\n", (unsigned)s.link_up_ms, (unsigned)s.link_ups, (unsigned)s.connect_ms, (unsigned)s.c6_resets,
           (unsigned)s.health_failures, (unsigned)s.connects, (unsigned)s.disconnects, (unsigned)s.wakes);
}

static const char *auth_str(uint8_t a)
{
    static const char *k[] = { "open", "wep", "wpa", "wpa2", "wpa/2", "ent", "wpa3", "wpa2/3", "wapi", "owe" };
    return a < sizeof k / sizeof k[0] ? k[a] : "?";
}

/* A tiny TCP echo server for the Phase 3 exit test: the P4 sleeps with the
 * connection open and answers a packet. Holds a net lock while it runs. */
static int s_echo_port;
static SemaphoreHandle_t s_echo_up;     /* given once the server listens, or failed to */
static StaticSemaphore_t s_echo_up_buf;

static void echo_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    t5_wakelock_t *lock = t5_wakelock_acquire(T5_LOCK_NET, "echo");
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
        printf("echo: can't listen on %d\n", port);
        xSemaphoreGive(s_echo_up);
        goto out;
    }
    printf("echo: listening on %d\n", port);
    xSemaphoreGive(s_echo_up);
    while (s_echo_port == port) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        char buf[256];
        int n;
        while ((n = recv(c, buf, sizeof buf, 0)) > 0) {
            /* Report the power state the packet found us in, then echo. */
            char hdr[48];
            int h = snprintf(hdr, sizeof hdr, "[%s] ", t5_power_state_name(t5_power_state()));
            send(c, hdr, h, 0);
            send(c, buf, n, 0);
        }
        close(c);
    }
out:
    if (ls >= 0) close(ls);
    t5_wakelock_release(lock);
    vTaskDelete(NULL);
}

/* net mon S [CH] [all]: a Wi-Fi monitor self-test. Counts frames by type
 * and lists the networks whose beacons it heard, then the drop counts. */
static int net_mon(uint32_t secs, uint8_t ch, bool all)
{
    t5_wifimon_cfg_t cfg = { .size = sizeof cfg, .channel = ch, .snaplen = 128,
                             .types = all ? (T5_WIFIMON_MGMT | T5_WIFIMON_DATA | T5_WIFIMON_CTRL) : T5_WIFIMON_MGMT };
    t5_wifimon_t *m;
    t5_err_t e = t5_wifimon_open(&cfg, &m);
    if (e != T5_OK) {
        printf("monitor: %s\n", t5_err_str(e));
        return 1;
    }
    struct { uint8_t bssid[6]; char ssid[33]; int8_t rssi; uint8_t ch; uint32_t n; } seen[16];
    int nseen = 0;
    uint32_t by_type[16] = { 0 }, bytes = 0;
    int64_t until = t5_time_us() + (int64_t)secs * 1000000;
    uint8_t buf[128];
    while (t5_time_us() < until) {
        t5_wifimon_frame_t f;
        int n = t5_wifimon_next(m, &f, buf, sizeof buf, 200);
        if (n < 0) continue;
        by_type[f.type & 0x0f]++;
        bytes += f.len;
        /* Beacons: type 0 subtype 8; the SSID element at 36. */
        if (n >= 38 && buf[0] == 0x80 && buf[36] == 0) {
            int k = 0;
            while (k < nseen && memcmp(seen[k].bssid, buf + 16, 6)) k++;
            if (k == nseen && nseen < 16) {
                nseen++;
                memcpy(seen[k].bssid, buf + 16, 6);
                int l = buf[37] < 32 && 38 + buf[37] <= n ? buf[37] : 0;
                memcpy(seen[k].ssid, buf + 38, (size_t)l);
                seen[k].ssid[l] = 0;
                seen[k].n = 0;
            }
            if (k < nseen) {
                seen[k].rssi = f.rssi;
                seen[k].ch = f.channel;
                seen[k].n++;
            }
        }
    }
    t5_wifimon_stats_t st;
    t5_wifimon_stats(m, &st);
    T5_IGNORE(t5_wifimon_close(m));
    printf("monitor ch %u, %u s: %u frames (mgmt %u, ctrl %u, data %u, misc %u), %u bytes on air\n", st.channel,
           (unsigned)secs, (unsigned)st.frames, (unsigned)by_type[1], (unsigned)by_type[2], (unsigned)by_type[4],
           (unsigned)by_type[8], (unsigned)bytes);
    printf("dropped: radio %u, link %u, queue %u\n", (unsigned)st.dropped_radio, (unsigned)st.dropped_link,
           (unsigned)st.dropped_queue);
    for (int k = 0; k < nseen; k++) {
        printf("  %02x:%02x:%02x:%02x:%02x:%02x ch %2u %4d dBm %4u beacons  %s\n", seen[k].bssid[0], seen[k].bssid[1],
               seen[k].bssid[2], seen[k].bssid[3], seen[k].bssid[4], seen[k].bssid[5], seen[k].ch, seen[k].rssi,
               (unsigned)seen[k].n, seen[k].ssid[0] ? seen[k].ssid : "(hidden)");
    }
    return 0;
}

static int cmd_net(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "mon")) {
        uint32_t secs = 0, ch = 0;
        if (!diag_parse_u32(argv[2], 1, 60, &secs)) return 1;
        if (argc >= 4 && strcmp(argv[3], "all") && !diag_parse_u32(argv[3], 1, 13, &ch)) return 1;
        bool all = !strcmp(argv[argc - 1], "all");
        return net_mon(secs, (uint8_t)ch, all);
    }
    if (argc == 1) { print_status(); return 0; }
    const char *c = argv[1];
    if (!strcmp(c, "on")) return t5_net_enable(true) != T5_OK;
    if (!strcmp(c, "off")) return t5_net_enable(false) != T5_OK;
    if (!strcmp(c, "scan")) {
        t5_net_scan();
        vTaskDelay(pdMS_TO_TICKS(4000));
        t5_net_ap_t ap[T5_NET_MAX_SCAN];
        int n = t5_net_scan_results(ap, T5_NET_MAX_SCAN);
        printf("%d network(s)\n", n);
        for (int i = 0; i < n; i++) {
            printf("  %4d dBm  ch %-2u %-6s %s%s\n", ap[i].rssi, ap[i].channel, auth_str(ap[i].auth), ap[i].ssid,
                   ap[i].saved ? "  (saved)" : "");
        }
        return 0;
    }
    if (!strcmp(c, "connect") && argc >= 3) {
        bool save = !(argc >= 5 && !strcmp(argv[4], "nosave"));
        t5_err_t e = t5_net_connect(argv[2], argc >= 4 ? argv[3] : NULL, save);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "disconnect")) return t5_net_disconnect() != T5_OK;
    if (!strcmp(c, "forget") && argc >= 3) return t5_net_forget(argv[2]) != T5_OK;
    if (!strcmp(c, "saved")) {
        char s[8][T5_NET_SSID_LEN];
        int n = t5_net_saved_list(s, 8);
        for (int i = 0; i < n && i < 8; i++) printf("  %s\n", s[i]);
        printf("%d saved\n", n);
        return 0;
    }
    if (!strcmp(c, "reset")) {
        int64_t t0 = esp_timer_get_time();
        t5_net_c6_reset();
        /* Wait for the link and, if a network is saved, an address. */
        t5_net_status_t s;
        do {
            vTaskDelay(pdMS_TO_TICKS(100));
            t5_net_status(&s);
        } while (s.state != T5_NET_CONNECTED && esp_timer_get_time() - t0 < 30000000LL &&
                 !(s.state == T5_NET_IDLE && t5_net_saved_list(NULL, 0) == 0));
        printf("C6 reset -> %s in %lld ms\n", t5_net_state_name(s.state), (long long)((esp_timer_get_time() - t0) / 1000));
        return 0;
    }
    if (!strcmp(c, "update")) {
        bool force = argc >= 3 && !strcmp(argv[2], "--force");
        printf("%s (watch the log)\n", t5_err_str(t5_net_c6_update(force)));
        return 0;
    }
    if (!strcmp(c, "ant") && argc >= 3) {
        return t5_radio_antenna_set(!strcmp(argv[2], "ext") ? T5_ANT_EXTERNAL : T5_ANT_INTERNAL) != T5_OK;
    }
    if (!strcmp(c, "handles")) {
        static char buf[2048];
        int n = t5_net_handles_dump(buf, sizeof buf);
        printf("%s%d open\n", buf, n);
        return 0;
    }
    if (!strcmp(c, "echo") && argc >= 3) {
        uint32_t port;
        if (!strcmp(argv[2], "stop")) { s_echo_port = 0; return 0; }
        if (!diag_parse_u32(argv[2], 1, 65535, &port)) goto usage;
        s_echo_port = (int)port;
        if (!s_echo_up) s_echo_up = xSemaphoreCreateBinaryStatic(&s_echo_up_buf);
        xSemaphoreTake(s_echo_up, 0);
        if (xTaskCreatePinnedToCore(echo_task, "echo", 4096, (void *)(intptr_t)port, 5, NULL, 0) != pdPASS) return 1;
        /* Its "listening" line is part of this command's reply. */
        xSemaphoreTake(s_echo_up, pdMS_TO_TICKS(5000));
        return 0;
    }
usage:
    printf("usage: net [mon <s> [ch] [all] | on | off | scan | connect <ssid> [pass] [nosave] | disconnect | forget <ssid> | saved\n"
           "           | reset | update [--force] | ant int|ext | echo <port>|stop | handles]\n");
    return 1;
}

/* The console's Bluetooth objects: the same app API (t5_net/ble.h), as a
 * system caller, so `ble` exercises what apps use. */
static t5_ble_conn_t *s_ble_link;
static t5_ble_advertiser_t *s_ble_adv;
static t5_ble_server_t *s_ble_srv;
/* The test server: one service, a readable/notifying counter, a writable
 * text and a readable name (UUIDs from uuidgen). */
#define BLE_TEST_SVC "5f3c0001-8b1e-4c4b-9b49-3d0c6a3f7a10"
static const t5_ble_server_chr_t s_ble_test_chrs[] = {
    { "5f3c0002-8b1e-4c4b-9b49-3d0c6a3f7a10", T5_BLE_PROP_READ | T5_BLE_PROP_NOTIFY },
    { "5f3c0003-8b1e-4c4b-9b49-3d0c6a3f7a10", T5_BLE_PROP_READ | T5_BLE_PROP_WRITE | T5_BLE_PROP_WRITE_NR },
    { "0x2a29", T5_BLE_PROP_READ },
};

static void ble_hex(const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) printf(" %02x", d[i]);
    printf("  \"");
    for (size_t i = 0; i < n; i++) putchar(d[i] >= 32 && d[i] < 127 ? d[i] : '.');
    printf("\"\n");
}

static int ble_scan_cmd(uint32_t ms)
{
    t5_ble_scanner_t *sc;
    t5_err_t e = t5_ble_scan_start(&(t5_ble_scan_cfg_t){ .duration_ms = ms, .active = true }, &sc);
    if (e != T5_OK) {
        printf("scan: %s\n", t5_err_str(e));
        return 1;
    }
    int n = 0;
    t5_ble_report_t r;
    while (t5_ble_scan_next(sc, &r, ms + 1000) == T5_OK) {
        char a[18];
        t5_ble_format_addr(r.addr, a);
        printf("  %s (%u) %4d dBm %s%s %s\n", a, r.addr_type, r.rssi, r.connectable ? "conn" : "    ",
               r.scan_rsp ? " rsp" : "    ", r.name);
        n++;
    }
    T5_IGNORE(t5_ble_scan_stop(sc));
    printf("%d report(s)\n", n);
    return 0;
}

static int ble_server_cmd(int argc, char **argv)
{
    const char *c = argc >= 3 ? argv[2] : "on";
    uint32_t v;
    if (!strcmp(c, "on")) {
        if (s_ble_srv) return 0;
        t5_err_t e = t5_ble_server_open(BLE_TEST_SVC, s_ble_test_chrs, 3, &s_ble_srv);
        if (e == T5_OK) e = t5_ble_server_set(s_ble_srv, 2, "Tab5 OS", 7);
        if (e == T5_OK) e = t5_ble_server_set(s_ble_srv, 0, "\0", 1);
        if (e == T5_OK && !s_ble_adv) {
            e = t5_ble_adv_start(&(t5_ble_adv_cfg_t){ .name = "Tab5 test", .svc_uuid = BLE_TEST_SVC, .connectable = true },
                                 &s_ble_adv);
        }
        printf("server %s: %s, advertising as \"Tab5 test\"\n", BLE_TEST_SVC, t5_err_str(e));
        return e != T5_OK;
    }
    if (!s_ble_srv) {
        printf("no server: ble server on\n");
        return 1;
    }
    if (!strcmp(c, "off")) {
        T5_IGNORE(t5_ble_server_close(s_ble_srv));
        s_ble_srv = NULL;
        return 0;
    }
    if (!strcmp(c, "set") && argc == 5 && diag_parse_u32(argv[3], 0, 2, &v)) {
        t5_err_t e = t5_ble_server_set(s_ble_srv, (int)v, argv[4], strlen(argv[4]));
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "ev") && argc >= 4 && diag_parse_u32(argv[3], 1, 120000, &v)) {
        /* Print events for v ms; every second, notify a counter on chr 0. */
        static const char *k[] = { "connected", "disconnected", "write", "subscribed" };
        uint8_t buf[T5_BLE_VALUE_MAX];
        uint32_t t0 = t5_time_ms(), tick = 0;
        while (t5_time_ms() - t0 < v) {
            t5_ble_server_ev_t ev;
            int n = t5_ble_server_next(s_ble_srv, &ev, buf, sizeof buf, 200);
            if (n >= 0) {
                char a[18];
                t5_ble_format_addr(ev.addr, a);
                printf("  %s peer %u %s chr %d%s:", k[ev.type], ev.peer, a, ev.chr,
                       ev.type == T5_BLE_SERVER_SUBSCRIBED ? (ev.on ? " on" : " off") : "");
                ble_hex(buf, (size_t)n);
            }
            if ((t5_time_ms() - t0) / 1000 > tick) {
                tick++;
                uint8_t cnt = (uint8_t)tick;
                T5_IGNORE(t5_ble_server_set(s_ble_srv, 0, &cnt, 1));   /* a test value */
            }
        }
        return 0;
    }
    printf("usage: ble server [on | off | set <0..2> <text> | ev <ms>]\n");
    return 1;
}

static int ble_link_cmd(const char *c, int argc, char **argv)
{
    uint32_t v;
    if (!s_ble_link) {
        printf("not connected: ble connect <addr> [type]\n");
        return 1;
    }
    if (!strcmp(c, "gatt")) {
        t5_err_t e = t5_ble_gatt_discover(s_ble_link, 10000);
        static t5_ble_chr_t ch[T5_BLE_MAX_CHARS];
        int n = t5_ble_gatt_chars(s_ble_link, ch, T5_BLE_MAX_CHARS);
        printf("discover: %s, %d characteristic(s), mtu %u\n", t5_err_str(e), n, t5_ble_conn_mtu(s_ble_link));
        for (int i = 0; i < n; i++) {
            printf("  0x%04x cccd 0x%04x props 0x%02x %s in %s\n", ch[i].val_handle, ch[i].cccd_handle, ch[i].props,
                   ch[i].uuid, ch[i].svc_uuid);
        }
        return e != T5_OK;
    }
    if (!strcmp(c, "read") && argc == 3 && diag_parse_u32(argv[2], 1, 0xffff, &v)) {
        uint8_t buf[T5_BLE_VALUE_MAX];
        int n = t5_ble_gatt_read(s_ble_link, (uint16_t)v, buf, sizeof buf, 5000);
        if (n < 0) printf("read: %s\n", t5_err_str((t5_err_t)n));
        else {
            printf("%d byte(s):", n);
            ble_hex(buf, (size_t)n);
        }
        return n < 0;
    }
    if (!strcmp(c, "write") && argc >= 4 && diag_parse_u32(argv[2], 1, 0xffff, &v)) {
        bool nr = argc >= 5 && !strcmp(argv[4], "nr");
        t5_err_t e = t5_ble_gatt_write(s_ble_link, (uint16_t)v, argv[3], strlen(argv[3]), !nr, 5000);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "sub") && argc >= 3 && diag_parse_u32(argv[2], 1, 0xffff, &v)) {
        t5_err_t e = t5_ble_gatt_subscribe(s_ble_link, (uint16_t)v, !(argc >= 4 && !strcmp(argv[3], "off")), 5000);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "notes") && argc == 3 && diag_parse_u32(argv[2], 1, 120000, &v)) {
        uint8_t buf[T5_BLE_VALUE_MAX];
        uint16_t h;
        int got = 0, n;
        uint32_t t0 = t5_time_ms();
        while (t5_time_ms() - t0 < v && (n = t5_ble_gatt_notification(s_ble_link, &h, buf, sizeof buf, 200)) != T5_ERR_STATE) {
            if (n < 0) continue;
            printf("  0x%04x:", h);
            ble_hex(buf, (size_t)n);
            got++;
        }
        printf("%d notification(s)\n", got);
        return 0;
    }
    if (!strcmp(c, "close")) {
        T5_IGNORE(t5_ble_conn_close(s_ble_link));
        s_ble_link = NULL;
        return 0;
    }
    return -1;
}

static int cmd_ble(int argc, char **argv)
{
    const char *c = argc >= 2 ? argv[1] : "";
    uint32_t v;
    if (!*c) {
        printf("ble %s (%s), %d link(s)%s%s%s\n", t5_ble_ready() ? "ready" : "not ready",
               t5_ble_enabled() ? "on" : "off", t5_ble_links(), s_ble_link ? ", console link open" : "",
               s_ble_adv ? ", advertising" : "", s_ble_srv ? ", test server open" : "");
        return 0;
    }
    if (!strcmp(c, "on") || !strcmp(c, "off")) {
        t5_err_t e = t5_ble_enable(!strcmp(c, "on"));
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "scan")) {
        v = 5000;
        if (argc >= 3 && !diag_parse_u32(argv[2], 100, 60000, &v)) return 1;
        return ble_scan_cmd(v);
    }
    if (!strcmp(c, "connect") && argc >= 3) {
        uint8_t addr[6];
        if (!t5_ble_parse_addr(argv[2], addr)) return 1;
        v = 0;
        if (argc >= 4 && !diag_parse_u32(argv[3], 0, 3, &v)) return 1;
        if (s_ble_link) T5_IGNORE(t5_ble_conn_close(s_ble_link));
        s_ble_link = NULL;
        int64_t t0 = t5_time_us();
        t5_err_t e = t5_ble_conn_open(addr, (uint8_t)v, 10000, &s_ble_link);
        printf("%s in %u ms, mtu %u\n", t5_err_str(e), (unsigned)((t5_time_us() - t0) / 1000),
               e == T5_OK ? t5_ble_conn_mtu(s_ble_link) : 0);
        return e != T5_OK;
    }
    if (!strcmp(c, "adv")) {
        if (s_ble_adv) T5_IGNORE(t5_ble_adv_stop(s_ble_adv));
        s_ble_adv = NULL;
        if (argc >= 3 && !strcmp(argv[2], "off")) return 0;
        static const uint8_t mfr[] = { 0xe5, 0x02, 'T', '5' };    /* Espressif's company id, then "T5" */
        t5_err_t e = t5_ble_adv_start(&(t5_ble_adv_cfg_t){ .name = argc >= 3 ? argv[2] : "Tab5", .mfr_data = mfr,
                                                            .mfr_len = sizeof mfr,
                                                            .connectable = argc >= 4 && !strcmp(argv[3], "conn") },
                                      &s_ble_adv);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "server")) return ble_server_cmd(argc, argv);
    int r = ble_link_cmd(c, argc, argv);
    if (r >= 0) return r;
    printf("usage: ble [on | off | scan [ms] | connect <aa:bb:..> [type] | gatt | read <h> | write <h> <text> [nr] |\n"
           "            sub <h> [off] | notes <ms> | close | adv [name [conn] | off] | server ...]\n");
    return 1;
}

void diag_register_net(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "net", .help = "Wi-Fi via the C6", .hint = "[scan|connect|...]", .func = cmd_net },
        { .command = "ble", .help = "Bluetooth LE: scan, links, GATT, advertising, a test server", .hint = "[on|scan|connect|gatt|adv|server|...]", .func = cmd_ble },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
