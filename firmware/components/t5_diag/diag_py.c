/* SPDX-License-Identifier: Apache-2.0
 * MicroPython from the console (plan 8.1, 8.2):
 *   py exec <code>     run code (\n for new lines) and print its output
 *   py run <file>      run a script in internal storage, on the SD card or a USB drive
 *   py test            the containment checks (plan 8.2 exit criterion)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "t5_py/containment.h"
#include "t5_py/py.h"
#include "t5_storage/paths.h"
#include "diag_priv.h"

#define OUT_MAX (16 * 1024)

static int run_code(const char *code, uint32_t timeout_ms)
{
    char *out = malloc(OUT_MAX);
    if (!out) return 1;
    t5_py_state_t st = t5_py_exec_capture(code, out, OUT_MAX, timeout_ms);
    fputs(out, stdout);
    if (out[0] && out[strlen(out) - 1] != '\n') putchar('\n');
    printf("[%s]\n", t5_py_state_name(st));
    free(out);
    return st == T5_PY_DONE ? 0 : 1;
}

/* The containment suite runs as system code (all permissions), so it shows
 * what no Python code can do whatever its manifest says. */
static const char *const s_forbidden[] = { T5_PY_FORBIDDEN_MODULES };
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
    "for m in forbidden:\n"
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
    "    f = open('" T5_USER_DIR "/_t5_test.mpy', 'wb')\n"
    "    f.write(b'M\\x06\\x00\\x1f')\n"
    "    f.close()\n"
    "    sys.path.insert(0, '" T5_USER_DIR "')\n"
    "    check('no .mpy loading', raises('import _t5_test', ImportError))\n"
    "    import os\n"
    "    os.remove('" T5_USER_DIR "/_t5_test.mpy')\n"
    "except OSError as e:\n"
    "    print('  skipped .mpy check:', e)\n"
    "check('open outside the roots', raises(\"open('" T5_SYS_DIR "/" T5_CRASH_DB_NAME "')\", OSError))\n"
    "check('write outside the roots', raises(\"open('" T5_DATA_PATH "/x.txt', 'w')\", OSError))\n"
    "check('no .. escape', raises(\"open('" T5_USER_DIR "/../.." T5_SYS_DIR "/" T5_CRASH_DB_NAME "')\", ValueError))\n"
    "check('listdir outside the roots', raises(\"__import__('os').listdir('" T5_SYS_DIR "')\", OSError))\n"
    "check('rename out of the roots', raises(\"__import__('os').rename('" T5_USER_DIR "/a', '" T5_DATA_PATH "/a')\", OSError))\n"
    "import t5\n"
    "check('t5 imports', True)\n"
    "check('internal I2C addresses refused', raises(\"t5.port.open('mbus.i2c').read_reg(0x44, 0, 1)\", OSError))\n"
    "check('apps cannot hold internal rails', raises(\"t5.port.rail('backlight')\", ValueError))\n"
    "print('containment:', 'all passed' if not fails else '%d failed' % fails)\n";

static int run_containment(void)
{
    size_t cap = sizeof s_containment + 32;
    for (size_t i = 0; i < sizeof s_forbidden / sizeof s_forbidden[0]; i++) cap += strlen(s_forbidden[i]) + 4;
    char *code = malloc(cap);
    if (!code) return 1;
    size_t n = (size_t)snprintf(code, cap, "forbidden = (");
    for (size_t i = 0; i < sizeof s_forbidden / sizeof s_forbidden[0]; i++) {
        n += (size_t)snprintf(code + n, cap - n, "'%s', ", s_forbidden[i]);
    }
    snprintf(code + n, cap - n, ")\n%s", s_containment);
    int r = run_code(code, 20000);
    free(code);
    return r;
}

static int cmd_py(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: py exec <code> | py run <file> | py test\n");
        return 1;
    }
    if (!strcmp(argv[1], "test")) return run_containment();
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

void diag_register_py(void)
{
    const esp_console_cmd_t c = {
        .command = "py",
        .help = "MicroPython: exec <code> | run <file> | test",
        .hint = "exec | run | test",
        .func = cmd_py,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
