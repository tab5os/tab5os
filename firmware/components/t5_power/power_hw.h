/* SPDX-License-Identifier: Apache-2.0
 * Hardware layer under t5_power. Device: power_hw.c. Host: test/host/mocks.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"
#include "t5_power/rail.h"

/* Reset reasons, mapped from esp_reset_reason(). */
typedef enum {
    T5_RST_POWERON,
    T5_RST_SW,
    T5_RST_PANIC,         /* also task/interrupt watchdog */
    T5_RST_WDT,           /* "other watchdog": what a PMS150G power-up looks like */
    T5_RST_DEEPSLEEP,
    T5_RST_BROWNOUT,
    T5_RST_OTHER,
} t5_reset_t;

t5_err_t t5_power_hw_init(void);
t5_err_t t5_power_hw_rail_set(t5_rail_t rail, bool on);
/* Sleep drops the CPU to its low clock; false restores the full clock. */
void t5_power_hw_cpu_low(bool low);
uint32_t t5_power_hw_cpu_mhz(void);
t5_reset_t t5_power_hw_reset_reason(void);
/* Wake the state task (from any task, or an ISR when from_isr). */
void t5_power_hw_notify(bool from_isr);
/* Block the state task until notified or ms passes. */
void t5_power_hw_wait(uint32_t ms);
/* Start the state task, which calls fn in a loop (fn does its own waiting). */
t5_err_t t5_power_hw_start_task(void (*fn)(void));
/* Deep sleep the P4 (charge mode), waking after wake_after_ms (0: no timer).
 * Does not return on the device. */
void t5_power_hw_halt(uint32_t wake_after_ms);
/* Is the P4 being woken from a deep sleep by its timer? */
bool t5_power_hw_woke_from_deep_sleep_timer(void);
