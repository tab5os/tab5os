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
#include "freertos/task.h"
#include "t5_net/ble.h"
#include "t5_net/net.h"
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
        goto out;
    }
    printf("echo: listening on %d\n", port);
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

static int cmd_net(int argc, char **argv)
{
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
    if (!strcmp(c, "echo") && argc >= 3) {
        uint32_t port;
        if (!strcmp(argv[2], "stop")) { s_echo_port = 0; return 0; }
        if (!diag_parse_u32(argv[2], 1, 65535, &port)) goto usage;
        s_echo_port = (int)port;
        xTaskCreatePinnedToCore(echo_task, "echo", 4096, (void *)(intptr_t)port, 5, NULL, 0);
        return 0;
    }
usage:
    printf("usage: net [on | off | scan | connect <ssid> [pass] [nosave] | disconnect | forget <ssid> | saved\n"
           "           | reset | update [--force] | ant int|ext | echo <port>|stop]\n");
    return 1;
}

static void on_notify(uint16_t h, const uint8_t *d, size_t n, void *arg)
{
    (void)arg;
    printf("notify 0x%04x:", h);
    for (size_t i = 0; i < n; i++) printf(" %02x", d[i]);
    printf("\n");
}

static int cmd_ble(int argc, char **argv)
{
    uint32_t v;
    uint8_t addr[6];
    if (argc == 1) {
        printf("ble %s, %s\n", t5_ble_ready() ? "ready" : "not ready", t5_ble_connected() ? "connected" : "not connected");
        return 0;
    }
    const char *c = argv[1];
    if (!strcmp(c, "on") || !strcmp(c, "off")) {
        t5_err_t e = t5_ble_enable(!strcmp(c, "on"));
        printf("%s\n", t5_err_str(e));
        return 0;
    }
    if (!strcmp(c, "scan")) {
        uint32_t ms = 4000;
        if (argc >= 3 && !diag_parse_u32(argv[2], 100, 60000, &ms)) goto usage;
        t5_err_t e = t5_ble_scan(ms);
        if (e != T5_OK) { printf("%s\n", t5_err_str(e)); return 1; }
        vTaskDelay(pdMS_TO_TICKS(ms + 300));
        t5_ble_dev_t d[T5_BLE_MAX_SCAN];
        int n = t5_ble_scan_results(d, T5_BLE_MAX_SCAN);
        printf("%d device(s)\n", n);
        for (int i = 0; i < n; i++) {
            char a[18];
            t5_ble_format_addr(d[i].addr, a);
            printf("  %s/%u %4d dBm  %s\n", a, d[i].addr_type, d[i].rssi, d[i].name);
        }
        return 0;
    }
    if (!strcmp(c, "connect") && argc >= 3 && t5_ble_parse_addr(argv[2], addr)) {
        uint32_t type = 0;
        if (argc >= 4 && !diag_parse_u32(argv[3], 0, 3, &type)) goto usage;
        t5_err_t e = t5_ble_connect(addr, (uint8_t)type);
        for (int i = 0; e == T5_OK && i < 120 && !t5_ble_connected(); i++) vTaskDelay(pdMS_TO_TICKS(100));
        printf("%s, %s\n", t5_err_str(e), t5_ble_connected() ? "connected" : "not connected");
        return !t5_ble_connected();
    }
    if (!strcmp(c, "disconnect")) return t5_ble_disconnect() != T5_OK;
    if (!strcmp(c, "gatt")) {
        t5_err_t e = t5_ble_discover();
        t5_ble_chr_t ch[T5_BLE_MAX_CHARS];
        int n = t5_ble_chars(ch, T5_BLE_MAX_CHARS);
        printf("%s, %d characteristic(s)\n", t5_err_str(e), n);
        for (int i = 0; i < n; i++) printf("  0x%04x props 0x%02x  %s / %s\n", ch[i].val_handle, ch[i].props, ch[i].svc_uuid, ch[i].uuid);
        return e != T5_OK;
    }
    if (!strcmp(c, "read") && argc >= 3 && diag_parse_u32(argv[2], 1, 0xFFFF, &v)) {
        uint8_t buf[128];
        size_t n = 0;
        t5_err_t e = t5_ble_read((uint16_t)v, buf, sizeof buf, &n);
        printf("%s:", t5_err_str(e));
        for (size_t i = 0; i < n; i++) printf(" %02x", buf[i]);
        printf("  \"");
        for (size_t i = 0; i < n; i++) putchar(buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
        printf("\"\n");
        return e != T5_OK;
    }
    if (!strcmp(c, "sub") && argc >= 3 && diag_parse_u32(argv[2], 1, 0xFFFF, &v)) {
        t5_ble_set_notify_cb(on_notify, NULL);
        t5_err_t e = t5_ble_subscribe((uint16_t)v, !(argc >= 4 && !strcmp(argv[3], "off")));
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
usage:
    printf("usage: ble [on | off | scan [ms] | connect <aa:bb:..> [type] | disconnect | gatt | read <handle>\n"
           "           | sub <value-handle> [off]]\n");
    return 1;
}

void diag_register_net(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "net", .help = "Wi-Fi via the C6", .hint = "[scan|connect|...]", .func = cmd_net },
        { .command = "ble", .help = "BLE central via the C6", .hint = "[on|scan|connect|gatt|...]", .func = cmd_ble },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
