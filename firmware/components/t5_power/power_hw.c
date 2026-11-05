/* SPDX-License-Identifier: Apache-2.0 */
#include "esp_attr.h"
#include "esp_private/esp_clk.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "t5_bsp/xpin.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_display/t5_display.h"
#include "t5_pins/pins.h"
#include "power_hw.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "t5.power_hw";

static SemaphoreHandle_t s_wake;
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_cpu_max;
static bool s_cpu_max_held;
#endif

t5_err_t t5_power_hw_init(void)
{
    if (!s_wake) {
        s_wake = xSemaphoreCreateBinary();
        if (!s_wake) return T5_ERR_NO_MEM;
    }
#if CONFIG_PM_ENABLE
    if (!s_cpu_max) {
        /* Dynamic frequency scaling only, no automatic light sleep: the panel
         * needs its video stream (bench 0.8). The lowest CPLL step keeps APB
         * where it is, so peripherals don't notice (360/90 or 400/100). */
        int max = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        esp_pm_config_t cfg = { .max_freq_mhz = max, .min_freq_mhz = max / 4, .light_sleep_enable = false };
        esp_err_t e = esp_pm_configure(&cfg);
        if (e != ESP_OK) {
            T5_LOGW(TAG, "esp_pm_configure: %s; Sleep keeps the full clock", esp_err_to_name(e));
        } else if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "t5_power", &s_cpu_max) == ESP_OK) {
            esp_pm_lock_acquire(s_cpu_max);
            s_cpu_max_held = true;
        }
    }
#endif
    return T5_OK;
}

t5_err_t t5_power_hw_rail_set(t5_rail_t rail, bool on)
{
    switch (rail) {
    case T5_RAIL_USB5V: return t5_xpin_output(T5_XP_USB5V_EN, on);
    case T5_RAIL_EXT5V: return t5_xpin_output(T5_XP_EXT5V_EN, on);
    case T5_RAIL_WLAN: return t5_xpin_output(T5_XP_WLAN_PWR_EN, on);
    case T5_RAIL_SPK: return t5_xpin_output(T5_XP_SPK_EN, on);
    case T5_RAIL_BACKLIGHT: return t5_display_ready() ? t5_display_blank(!on) : T5_OK;
    default: return T5_ERR_INVALID_ARG;
    }
}

void t5_power_hw_cpu_low(bool low)
{
#if CONFIG_PM_ENABLE
    if (!s_cpu_max) return;
    if (low && s_cpu_max_held) {
        esp_pm_lock_release(s_cpu_max);
        s_cpu_max_held = false;
    } else if (!low && !s_cpu_max_held) {
        esp_pm_lock_acquire(s_cpu_max);
        s_cpu_max_held = true;
    }
#else
    (void)low;
#endif
}

uint32_t t5_power_hw_cpu_mhz(void)
{
    return (uint32_t)(esp_clk_cpu_freq() / 1000000);
}

t5_reset_t t5_power_hw_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return T5_RST_POWERON;
    case ESP_RST_SW:
    case ESP_RST_EXT:
    case ESP_RST_USB: return T5_RST_SW;   /* restarts asked for by a person or a host */
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_CPU_LOCKUP: return T5_RST_PANIC;
    case ESP_RST_WDT: return T5_RST_WDT;
    case ESP_RST_DEEPSLEEP: return T5_RST_DEEPSLEEP;
    case ESP_RST_BROWNOUT: return T5_RST_BROWNOUT;
    default: return T5_RST_OTHER;
    }
}

bool t5_power_hw_woke_from_deep_sleep_timer(void)
{
    return esp_reset_reason() == ESP_RST_DEEPSLEEP &&
           (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_TIMER));
}

void t5_power_hw_notify(bool from_isr)
{
    if (!s_wake) return;
    if (from_isr) {
        BaseType_t hp = pdFALSE;
        xSemaphoreGiveFromISR(s_wake, &hp);
        portYIELD_FROM_ISR(hp);
    } else {
        xSemaphoreGive(s_wake);
    }
}

void t5_power_hw_wait(uint32_t ms)
{
    if (s_wake) xSemaphoreTake(s_wake, pdMS_TO_TICKS(ms));
    else t5_delay_ms(ms);
}

static void (*s_step)(void);

static void power_task(void *arg)
{
    (void)arg;
    for (;;) {
        s_step();
    }
}

t5_err_t t5_power_hw_start_task(void (*fn)(void))
{
    s_step = fn;
    return t5_task_create("t5_power", power_task, NULL, 6144, 8, 0, NULL);
}

void t5_power_hw_halt(uint32_t wake_after_ms)
{
    if (wake_after_ms) esp_sleep_enable_timer_wakeup((uint64_t)wake_after_ms * 1000);
    esp_deep_sleep_start();
}
