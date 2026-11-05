/* SPDX-License-Identifier: Apache-2.0 */
#include "driver/temperature_sensor.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t5_core/os.h"
#include "battery_hw.h"

static temperature_sensor_handle_t s_ts;

t5_err_t t5_battery_hw_chip_temp(int16_t *c10)
{
    if (!s_ts) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_ts) != ESP_OK) return T5_ERR_NOT_SUPPORTED;
        if (temperature_sensor_enable(s_ts) != ESP_OK) return T5_ERR_IO;
    }
    float c;
    if (temperature_sensor_get_celsius(s_ts, &c) != ESP_OK) return T5_ERR_IO;
    *c10 = (int16_t)(c * 10.0f);
    return T5_OK;
}

bool t5_battery_hw_usb_host(void)
{
    return usb_serial_jtag_is_connected();
}

static void (*s_step)(void);
static uint32_t s_period;

static void task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        s_step();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(s_period));
    }
}

t5_err_t t5_battery_hw_start_task(void (*step)(void), uint32_t period_ms)
{
    s_step = step;
    s_period = period_ms;
    return t5_task_create("t5_battery", task, NULL, 4096, 6, 0, NULL);
}
