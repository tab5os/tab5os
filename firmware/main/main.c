/*
 * Tab5 OS entry point: service start order.
 *
 * Order matters. Events first (everything posts), then the bus, then the
 * expanders (every rail and reset line), then services that only need the
 * bus. Display before touch: the probe that picks the panel also tells touch
 * which controller is fitted. Storage and the IMU must never block boot.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "t5_bsp/board.h"
#include "t5_bsp/xpin.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_diag/t5_diag.h"
#include "t5_display/t5_display.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_imu/t5_imu.h"
#include "t5_input/t5_touch.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/t5_time.h"

static const char *TAG = "main";

#define START(what, expr)                                                   \
    do {                                                                    \
        t5_err_t _e = (expr);                                               \
        if (_e != T5_OK) T5_LOGE(TAG, "%s failed: %s", what, t5_err_str(_e)); \
    } while (0)

static void log_event(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)len; (void)arg;
    if (id >= T5_EV_I2C_DEVICE_FAULT && id <= T5_EV_I2C_DEVICE_OK) {
        const t5_ev_i2c_t *e = data;
        T5_LOGW(TAG, "event %s: 0x%02x err %s", t5_event_name(id), e->addr, t5_err_str((t5_err_t)e->err));
    } else {
        T5_LOGI(TAG, "event %s", t5_event_name(id));
    }
}

void app_main(void)
{
    t5_diag_log_capture_start();
    /* Phase 9 replaces this with a real health check before marking valid. */
    esp_ota_mark_app_valid_cancel_rollback();

    ESP_ERROR_CHECK(t5_event_init() == T5_OK ? ESP_OK : ESP_FAIL);
    t5_event_subscribe(T5_EV_ANY, log_event, NULL);

    START("board", t5_bsp_board_init());
    START("i2c", t5_i2c_init());
    START("expanders", t5_xpin_init());
    START("board defaults", t5_xpin_apply_boot_defaults());
    START("time", t5_time_init());
    START("imu", t5_imu_start());          /* background */
    START("storage", t5_storage_init());
    START("display", t5_display_init());
    if (t5_display_ready()) {
        START("test pattern", t5_display_test_pattern());
    }
    START("touch", t5_touch_init());

    ESP_ERROR_CHECK(t5_diag_start());
    ESP_LOGI(TAG, "boot complete");
}
