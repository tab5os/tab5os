/* SPDX-License-Identifier: Apache-2.0
 * Device event loop: a dedicated esp_event loop, task pinned to core 0.
 * ESP-IDF's own events (Wi-Fi, IP) can be forwarded into it in Phase 3.
 */
#include <string.h>
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t5_core/os.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_event_priv.h"

ESP_EVENT_DEFINE_BASE(T5_EVENT_BASE);

static const char *TAG = "t5.event";
static esp_event_loop_handle_t s_loop;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    /* esp_event doesn't pass the size back; handlers know their payload type. */
    t5_event_dispatch((t5_event_id_t)id, data, data ? T5_EVENT_MAX_DATA : 0);
}

t5_err_t t5_event_init(void)
{
    if (s_loop) {
        return T5_OK;
    }
    T5_RETURN_ON_ERR(t5_event_table_init());
    const esp_event_loop_args_t args = {
        .queue_size = 32,
        .task_name = "t5_events",
        .task_priority = 10,
        .task_stack_size = 4096,
        .task_core_id = 0,
    };
    if (esp_event_loop_create(&args, &s_loop) != ESP_OK) {
        return T5_ERR_NO_MEM;
    }
    if (esp_event_handler_register_with(s_loop, T5_EVENT_BASE, ESP_EVENT_ANY_ID, on_event, NULL) != ESP_OK) {
        return T5_ERR_FAIL;
    }
    /* Apps' event handlers run on this task (stack margin: see os.h). */
    TaskHandle_t h = xTaskGetHandle("t5_events");
    if (h) t5_task_guard_margin((t5_task_t)h);
    return T5_OK;
}

t5_err_t t5_event_post(t5_event_id_t id, const void *data, size_t len)
{
    if (!s_loop) {
        return T5_ERR_STATE;
    }
    uint8_t buf[T5_EVENT_MAX_DATA] = {0};
    if (len > sizeof buf) {
        return T5_ERR_INVALID_ARG;
    }
    if (data && len) {
        memcpy(buf, data, len);
    }
    esp_err_t err = esp_event_post_to(s_loop, T5_EVENT_BASE, id, data ? buf : NULL,
                                      data ? sizeof buf : 0, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        T5_LOGW(TAG, "dropped %s: %s", t5_event_name(id), esp_err_to_name(err));
        return T5_ERR_BUSY;
    }
    return T5_OK;
}
