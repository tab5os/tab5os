/* SPDX-License-Identifier: Apache-2.0
 * Hardware layers under t5_power and t5_battery.
 */
#include <string.h>
#include "t5_mocks.h"
#include "power_hw.h"
#include "battery_hw.h"

mock_power_t mock_power;

t5_err_t t5_power_hw_init(void) { return T5_OK; }
t5_err_t t5_power_hw_rail_set(t5_rail_t rail, bool on)
{
    if (mock_power.rail_fail) return T5_ERR_IO;
    mock_power.rail[rail] = on;
    mock_power.rail_writes++;
    return T5_OK;
}
void t5_power_hw_cpu_low(bool low) { mock_power.cpu_low = low; }
uint32_t t5_power_hw_cpu_mhz(void) { return mock_power.cpu_low ? 90 : 360; }
t5_reset_t t5_power_hw_reset_reason(void) { return (t5_reset_t)mock_power.reset; }
void t5_power_hw_notify(bool from_isr) { (void)from_isr; mock_power.notifies++; }
void t5_power_hw_wait(uint32_t ms) { (void)ms; }
t5_err_t t5_power_hw_start_task(void (*fn)(void)) { mock_power.task = fn; return T5_OK; }
void t5_power_hw_halt(uint32_t ms) { mock_power.halted++; mock_power.halt_ms = ms; }
bool t5_power_hw_woke_from_deep_sleep_timer(void) { return mock_power.deep_sleep_timer; }

t5_err_t t5_battery_hw_chip_temp(int16_t *c10)
{
    if (mock_power.chip_c10 == INT16_MIN) return T5_ERR_NOT_SUPPORTED;
    *c10 = mock_power.chip_c10;
    return T5_OK;
}
bool t5_battery_hw_usb_host(void) { return mock_power.usb_host; }
t5_err_t t5_battery_hw_start_task(void (*step)(void), uint32_t period_ms)
{
    (void)period_ms;
    mock_power.battery_task = step;
    return T5_OK;
}

void mock_power_reset(void)
{
    memset(&mock_power, 0, sizeof mock_power);
    mock_power.reset = T5_RST_POWERON;
    mock_power.chip_c10 = INT16_MIN;
}
