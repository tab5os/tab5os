/*
 * Bring-up console (Phase 0). Commands for the bench checks:
 *   sys, tasks, psram, reboot       (0.2)
 *   i2c scan | r | w                (0.3)
 *   exp dump | out | in             (0.4)
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"

/* Register all commands and start a REPL on the USB Serial/JTAG console. */
esp_err_t t5_diag_start(void);

/* Start copying log output to the dmesg ring. Call first thing in app_main. */
void t5_diag_log_capture_start(void);
