/* SPDX-License-Identifier: Apache-2.0
 * MicroPython HAL for Tab5 OS: time, stdio (to the VM's owner) and waits.
 * There are no pins here on purpose: hardware goes through `t5`.
 */
#pragma once
#include <stdint.h>
#include "py/mpconfig.h"
#include "esp_timer.h"
#include "esp_cpu.h"

static inline mp_uint_t mp_hal_ticks_ms(void)
{
    return (mp_uint_t)(esp_timer_get_time() / 1000);
}

static inline mp_uint_t mp_hal_ticks_us(void)
{
    return (mp_uint_t)esp_timer_get_time();
}

static inline mp_uint_t mp_hal_ticks_cpu(void)
{
    return (mp_uint_t)esp_cpu_get_cycle_count();
}

void mp_hal_delay_ms(mp_uint_t ms);
void mp_hal_delay_us(mp_uint_t us);
uint64_t mp_hal_time_ns(void);
void mp_hal_set_interrupt_char(int c);
int mp_hal_stdin_rx_chr(void);
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len);
