/* SPDX-License-Identifier: Apache-2.0
 * The log ring: the last 16 KB of log output, kept in PSRAM from the first
 * line of app_main, for dmesg, the Terminal's log view and the developer
 * mode log stream. Readers keep their own position (bytes since boot), so
 * any number can follow it.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define T5_LOG_RING_BYTES 16384

/* Start copying log output into the ring (idempotent). */
void t5_log_capture_start(void);
/* Bytes logged since boot. */
uint64_t t5_log_head(void);
/* Copy bytes from *pos (an earlier t5_log_head()) into buf and advance *pos.
 * If *pos fell out of the ring, it jumps to the oldest byte kept. Returns the
 * count copied (0 when caught up). */
size_t t5_log_read(uint64_t *pos, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
