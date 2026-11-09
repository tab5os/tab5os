/* SPDX-License-Identifier: Apache-2.0
 * The USB console (USB Serial/JTAG): bench checks, diagnostics, and the
 * commands the host tools drive (tools/t5/t5link.py). `help` lists them.
 */
#pragma once
#include "esp_err.h"

/* `app put PATH SIZE` (diag_app.c), the t5 CLI's USB fallback: after READY
 * the sender writes one line at a time, "BASE64 CRC32HEX", each holding
 * T5_DIAG_PUT_CHUNK bytes (the last one fewer). "@ok" acknowledges a line,
 * "@re" asks for it again (the console can drop bytes), "put: ..." is a
 * fatal error and "OK <bytes> <crc32>" ends the transfer. A line must fit
 * the USB console's 256-byte receive ring, which drops overflow: 150 bytes
 * are 200 base64 characters. */
#define T5_DIAG_PUT_CHUNK 150

/* Register all commands and start a REPL on the USB Serial/JTAG console. */
esp_err_t t5_diag_start(void);
