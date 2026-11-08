/*
 * Tab5 OS entry point: service start order.
 *
 * Order matters. Events first (everything posts), then the bus, then the
 * expanders (every rail and reset line), then services that only need the
 * bus. Display before touch: the probe that picks the panel also tells touch
 * which controller is fitted. Storage and the IMU must never block boot.
 * Power comes after storage (its settings) and the display (the backlight
 * rail); the battery service after power (soft off, charge mode). The UI
 * and the app runtime come last.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "t5_ota/ota.h"
#include "t5_battery/battery.h"
#include "t5_bsp/board.h"
#include "t5_bsp/xpin.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_diag/t5_diag.h"
#include "t5_display/t5_display.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_imu/t5_imu.h"
#include "t5_input/t5_touch.h"
#include "t5_app/apps.h"
#include "t5_audio/audio.h"
#include "t5_camera/camera.h"
#include "t5_shell/shell.h"
#include "t5_usb/usb.h"
#include "sysapps/sysapps.h"
#include "t5_net/net.h"
#include "t5_ports/ports.h"
#include "t5_py/py.h"
#include "t5_dev/dev.h"
#include "t5_power/autosave.h"
#include "t5_power/power.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/t5_time.h"
#include "t5_ui/t5_ui.h"

static const char *TAG = "main";

#define START(what, expr)                                                   \
    do {                                                                    \
        t5_err_t _e = (expr);                                               \
        if (_e != T5_OK) T5_LOGE(TAG, "%s failed: %s", what, t5_err_str(_e)); \
    } while (0)

static void log_event(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)len; (void)arg;
    if (id == T5_EV_BATTERY_UPDATE) {
        return;   /* once per percent; the console has 'bat' */
    }
    if (id >= T5_EV_I2C_DEVICE_FAULT && id <= T5_EV_I2C_DEVICE_OK) {
        const t5_ev_i2c_t *e = data;
        T5_LOGW(TAG, "event %s: 0x%02x err %s", t5_event_name(id), e->addr, t5_err_str((t5_err_t)e->err));
    } else {
        T5_LOGI(TAG, "event %s", t5_event_name(id));
    }
}

static void save_before_restart(void)
{
    T5_LOG_ON_ERR(TAG, t5_autosave_now());
}

void app_main(void)
{
    t5_diag_log_capture_start();
    /* Plan 9.1, 9.3: a new image is on trial until this boot proves healthy;
     * failed boots before this one may mean safe mode. */
    t5_ota_boot_check();
    t5_ota_set_restart_hook(save_before_restart);
    t5_apps_set_safe_mode(t5_safe_mode());

    ESP_ERROR_CHECK(t5_event_init() == T5_OK ? ESP_OK : ESP_FAIL);
    t5_event_subscribe(T5_EV_ANY, log_event, NULL);

    START("board", t5_bsp_board_init());
    START("i2c", t5_i2c_init());
    START("expanders", t5_xpin_init());
    START("board defaults", t5_xpin_apply_boot_defaults());
    START("time", t5_time_init());
    /* A charge-mode timer wake checks USB and goes back to deep sleep here,
     * before the display or anything slow starts. */
    t5_power_read_wake_reason();
    if (t5_power_wake_reason() == T5_WAKE_CHARGE_TIMER) {
        t5_battery_charge_mode_resume();
    }
    START("imu", t5_imu_start());          /* background */
    START("storage", t5_storage_init());
    t5_time_tz_load();                     /* local time zone, from /data */
    START("recovery flag", t5_ota_check_sd_flag());   /* plan 9.2: a RECOVERY file on the card */
    /* Before the display: saving and erasing a crash's core dump means long
     * flash operations, which would starve a running panel. */
    START("crash check", t5_apps_boot_check());
    START("display", t5_display_init());   /* black, backlight held off */
    START("touch", t5_touch_init());
    START("power", t5_power_init());       /* after storage and display: settings, backlight */
    START("battery", t5_battery_init());
    START("net", t5_net_init());           /* background: powers the C6 and connects */
    START("ports", t5_ports_init());
    /* Phase 7: media and USB, after power (rails, wake locks, hooks). */
    START("audio", t5_audio_init());
    START("camera", t5_camera_init());
    START("usb", t5_usb_init());
    /* The UI after power (suspend hooks) and touch (input); the app runtime
     * last, since it may restore the last app or run a scheduled alarm. */
    if (t5_display_ready()) {
        START("ui", t5_ui_init());
        START("shell", t5_shell_register());   /* the home screen, before the runtime shows it */
        START("system apps", t5_sysapps_register());
        START("python", t5_py_init());     /* the micropython runtime, before the app runtime */
        START("apps", t5_apps_init());     /* shows the launcher, then the backlight comes on */
        if (!t5_ui_ready() || !t5_apps_ready()) {
            t5_display_release_hold();     /* no UI: light the panel for console tools */
        }
    }

    /* Developer mode (plan 8.5): listens on the network only if it is on. */
    START("developer mode", t5_dev_init());
    ESP_ERROR_CHECK(t5_diag_start());
    ESP_LOGI(TAG, "boot complete");
    /* Healthy once the shell is on the panel (or right away on a boot
     * without a display, where the console is the way in). */
    for (int i = 0; i < 100 && t5_apps_ready(); i++) {
        t5_ui_stats_t st;
        t5_ui_get_stats(&st);
        if (st.panel_frames > 2) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    t5_ota_boot_ok();
}
