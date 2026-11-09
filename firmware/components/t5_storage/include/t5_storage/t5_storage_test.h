/* SPDX-License-Identifier: Apache-2.0
 * Host tests only.
 */
#pragma once
#include <stdint.h>

/* One step of the SD poller, as its task runs it every T5_SD_POLL_MS. */
void t5_storage_sd_poll(uint32_t now_ms);
/* SD state back to ABSENT, /data unmounted. */
void t5_storage_reset_for_test(void);
