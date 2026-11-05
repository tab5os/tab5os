/* SPDX-License-Identifier: Apache-2.0 */
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "t5_pins/pins.h"
#include "touch_hw.h"

static SemaphoreHandle_t s_sem;

static void IRAM_ATTR on_int(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

t5_err_t t5_touch_hw_init(bool use_irq)
{
    if (!use_irq) {
        /* GT911 boards: a pull-up on INT blocks the controller; hold it low (BSP fix). */
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << T5_PIN_TOUCH_INT, .mode = GPIO_MODE_OUTPUT, .pull_up_en = 1,
        };
        gpio_config(&io);
        gpio_set_level(T5_PIN_TOUCH_INT, 0);
        return T5_OK;
    }
    s_sem = xSemaphoreCreateBinary();
    if (!s_sem) {
        return T5_ERR_NO_MEM;
    }
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << T5_PIN_TOUCH_INT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1, .intr_type = GPIO_INTR_NEGEDGE,
    };
    if (gpio_config(&io) != ESP_OK) {
        return T5_ERR_IO;
    }
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   /* already installed is fine */
        return T5_ERR_IO;
    }
    return gpio_isr_handler_add(T5_PIN_TOUCH_INT, on_int, NULL) == ESP_OK ? T5_OK : T5_ERR_IO;
}

bool t5_touch_hw_wait(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
