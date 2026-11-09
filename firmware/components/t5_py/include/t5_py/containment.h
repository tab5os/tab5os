/* SPDX-License-Identifier: Apache-2.0
 * Module names Python code must never reach (plan 8.2). The build check
 * (tools/py/check_modules.py) reads these lists from this file, and the
 * console's `py test` imports each forbidden one, so both test the same set.
 */
#pragma once

/* Never importable: hardware, memory, threads, raw networking, other ports. */
#define T5_PY_FORBIDDEN_MODULES                                                                          \
    "machine", "uctypes", "esp", "esp32", "_thread", "ffi", "pyb", "rp2", "vfs", "lwip", "usocket", "uos", \
        "bluetooth", "espnow", "onewire", "framebuf", "tls"

/* Only as Tab5 OS's own frozen Python (t5_py/frozen), never as a C module. */
#define T5_PY_FROZEN_ONLY_MODULES "network", "ssl"
