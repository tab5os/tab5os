/* SPDX-License-Identifier: Apache-2.0
 * MicroPython from the console (plan 8.1, 8.2):
 *   py exec <code>     run code (\n for new lines) and print its output
 *   py run <file>      run a script under /data/user, /sd or /usb
 *   py test            the containment checks (plan 8.2 exit criterion)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "t5_dev/dev.h"
#include "t5_ota/ota.h"
#include "t5_py/py.h"
#include "diag_priv.h"

#define OUT_MAX (16 * 1024)

static const char *const s_state[] = { "running", "done", "exception", "stopped" };

static int run_code(const char *code, uint32_t timeout_ms)
{
    char *out = malloc(OUT_MAX);
    if (!out) return 1;
    t5_py_state_t st = t5_py_exec_capture(code, out, OUT_MAX, timeout_ms);
    fputs(out, stdout);
    if (out[0] && out[strlen(out) - 1] != '\n') putchar('\n');
    printf("[%s]\n", s_state[st]);
    free(out);
    return st == T5_PY_DONE ? 0 : 1;
}

/* The containment suite runs as system code (all permissions), so it shows
 * what no Python code can do whatever its manifest says. */
static const char s_containment[] =
    "import sys\n"
    "fails = 0\n"
    "def check(name, ok):\n"
    "    global fails\n"
    "    print(('PASS ' if ok else 'FAIL ') + name)\n"
    "    if not ok: fails += 1\n"
    "def raises(src, exc=Exception):\n"
    "    try:\n"
    "        exec(src)\n"
    "    except exc:\n"
    "        return True\n"
    "    except Exception as e:\n"
    "        print('  unexpected', type(e).__name__, e)\n"
    "        return False\n"
    "    return False\n"
    "for m in ('machine', 'uctypes', 'esp', 'esp32', '_thread', 'ffi', 'vfs', 'lwip', 'usocket',\n"
    "          'bluetooth', 'framebuf', 'pyb', 'uos'):\n"
    "    check('no module ' + m, raises('import ' + m, ImportError))\n"
    "import socket, ssl, network\n"
    "check('socket is the t5 one', hasattr(socket.socket, '_tls'))\n"
    "check('no raw sockets', raises('socket.socket(socket.AF_INET, socket.SOCK_RAW)', OSError))\n"
    "check('ssl is the t5 one', ssl.MBEDTLS_VERSION.startswith('mbedTLS (ESP-IDF)'))\n"
    "check('network cannot change Wi-Fi', raises('network.WLAN().config(ssid=\"x\")', OSError))\n"
    "check('no native emitter', raises('@micropython.native\\ndef f(): pass', SyntaxError))\n"
    "check('no viper emitter', raises('@micropython.viper\\ndef f(): pass', SyntaxError))\n"
    "check('no inline assembler', raises('@micropython.asm_rv32\\ndef f(): pass', SyntaxError))\n"
    "check('no machine.mem32 via builtins', not hasattr(__import__('builtins'), 'mem32'))\n"
    "check('no mem functions in micropython', not any(hasattr(__import__('micropython'), n) for n in ('mem8', 'mem16', 'mem32')))\n"
    "try:\n"
    "    f = open('/data/user/_t5_test.mpy', 'wb')\n"
    "    f.write(b'M\\x06\\x00\\x1f')\n"
    "    f.close()\n"
    "    sys.path.insert(0, '/data/user')\n"
    "    check('no .mpy loading', raises('import _t5_test', ImportError))\n"
    "    import os\n"
    "    os.remove('/data/user/_t5_test.mpy')\n"
    "except OSError as e:\n"
    "    print('  skipped .mpy check:', e)\n"
    "check('open outside the roots', raises(\"open('/data/sys/crashes.bin')\", OSError))\n"
    "check('write outside the roots', raises(\"open('/data/x.txt', 'w')\", OSError))\n"
    "check('no .. escape', raises(\"open('/data/user/../sys/crashes.bin')\", ValueError))\n"
    "check('listdir outside the roots', raises(\"__import__('os').listdir('/data/sys')\", OSError))\n"
    "check('rename out of the roots', raises(\"__import__('os').rename('/data/user/a', '/data/a')\", OSError))\n"
    "import t5\n"
    "check('t5 imports', True)\n"
    "check('internal I2C addresses refused', raises(\"t5.port.open('mbus.i2c').read_reg(0x44, 0, 1)\", OSError))\n"
    "check('apps cannot hold internal rails', raises(\"t5.port.rail('backlight')\", ValueError))\n"
    "print('containment:', 'all passed' if not fails else '%d failed' % fails)\n";

static int cmd_py(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: py exec <code> | py run <file> | py test | py stop\n");
        return 1;
    }
    if (!strcmp(argv[1], "test")) return run_code(s_containment, 20000);
    if (!strcmp(argv[1], "exec") && argc >= 3) {
        /* Join the words back and turn "\n" into new lines. */
        char *code = calloc(1, 1024);
        if (!code) return 1;
        size_t n = 0;
        for (int i = 2; i < argc; i++) {
            for (const char *p = argv[i]; *p && n < 1020; p++) {
                if (p[0] == '\\' && p[1] == 'n') {
                    code[n++] = '\n';
                    p++;
                } else {
                    code[n++] = *p;
                }
            }
            if (i + 1 < argc && n < 1020) code[n++] = ' ';
        }
        int r = run_code(code, 30000);
        free(code);
        return r;
    }
    if (!strcmp(argv[1], "run") && argc >= 3) {
        char code[300];
        snprintf(code, sizeof code, "exec(open(%c%s%c).read(), {'__name__': '__main__'})", '\'', argv[2], '\'');
        return run_code(code, 60000);
    }
    printf("py: unknown subcommand\n");
    return 1;
}

/* devmode [on|off|forget]: developer mode (plan 8.5). */
static int cmd_devmode(int argc, char **argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
        t5_err_t e = t5_dev_set_enabled(!strcmp(argv[1], "on"));
        if (e != T5_OK) printf("%s\n", t5_err_str(e));
    } else if (argc >= 2 && !strcmp(argv[1], "forget")) {
        t5_err_t fe = t5_dev_forget_all();
        if (fe != T5_OK) printf("%s\n", t5_err_str(fe));
    }
    t5_dev_status_t st;
    t5_dev_status(&st);
    printf("developer mode %s, %s on port %u, %d paired, %u requests, %u refused%s\n", st.enabled ? "on" : "off",
           st.listening ? "listening" : "not listening", st.port, st.clients, (unsigned)st.requests,
           (unsigned)st.denied, st.pairing ? ", pairing code on screen" : "");
    return 0;
}

/* ota [file PATH [unsigned] | url URL [unsigned] | recovery | safemode] (plan 9.1-9.3) */
static int cmd_ota(int argc, char **argv)
{
    char err[128] = "";
    t5_err_t e = T5_OK;
    bool unsigned_ok = argc >= 4 && !strcmp(argv[3], "unsigned");
    if (argc >= 3 && !strcmp(argv[1], "file")) e = t5_ota_install_file(argv[2], unsigned_ok, err, sizeof err);
    else if (argc >= 3 && !strcmp(argv[1], "url")) e = t5_ota_install_url(argv[2], unsigned_ok, err, sizeof err);
    else if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        /* ota selftest [SIG128HEX KEYID64HEX] */
        uint8_t sig[64], kid[32];
        bool signed_test = argc >= 4 && strlen(argv[2]) == 128 && strlen(argv[3]) == 64;
        for (int i = 0; signed_test && i < 64; i++) sscanf(argv[2] + 2 * i, "%2hhx", &sig[i]);
        for (int i = 0; signed_test && i < 32; i++) sscanf(argv[3] + 2 * i, "%2hhx", &kid[i]);
        e = t5_ota_selftest(signed_test ? sig : NULL, signed_test ? kid : NULL, err, sizeof err);
    } else if (argc >= 2 && !strcmp(argv[1], "recovery")) e = t5_ota_reboot_to_recovery();
    else if (argc >= 2 && !strcmp(argv[1], "safemode")) e = t5_ota_reboot_safe_mode();
    else if (argc >= 2) {
        printf("usage: ota [file PATH [unsigned] | url URL [unsigned] | selftest [SIG KEYID] | recovery | safemode]\n");
        return 1;
    }
    if (argc >= 2) printf("%s%s%s\n", t5_err_str(e), err[0] ? ": " : "", err);
    t5_ota_status_t st;
    t5_ota_status(&st);
    static const char *const names[] = { "idle", "receiving", "verifying", "ready (restart)", "failed" };
    printf("update %s %s %u/%u%s%s, %s, %d failed boots before this one, safe mode %s\n", names[st.state], st.version,
           (unsigned)st.done, (unsigned)st.total, st.error[0] ? ": " : "", st.error,
           t5_ota_on_trial() ? "running on trial" : "confirmed", t5_ota_failed_boots(), t5_safe_mode() ? "on" : "off");
    return e != T5_OK;
}

void diag_register_py(void)
{
    const esp_console_cmd_t o = {
        .command = "ota",
        .help = "Updates: file PATH | url URL [unsigned] | recovery | safemode",
        .func = cmd_ota,
    };
    esp_console_cmd_register(&o);
    const esp_console_cmd_t d = {
        .command = "devmode",
        .help = "Developer mode: on | off | forget (paired computers)",
        .func = cmd_devmode,
    };
    esp_console_cmd_register(&d);
    const esp_console_cmd_t c = {
        .command = "py",
        .help = "MicroPython: exec <code> | run <file> | test",
        .hint = "exec | run | test",
        .func = cmd_py,
    };
    esp_console_cmd_register(&c);
}
