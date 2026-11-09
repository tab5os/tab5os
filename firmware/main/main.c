/*
 * Tab5 OS entry point: service start order.
 *
 * Order matters. Events first (everything posts), then the board bring-up
 * (the bus and the expanders: every rail and reset line), then services that
 * only need the bus. The panel probe (resets released, controller
 * identified) before the display and touch, which both read its answer;
 * storage before the display (its timing override is on /data). Storage and
 * the IMU must never block boot. Power comes after storage (its settings),
 * wired to the display, touch, IMU and UI (board_power.c); the battery
 * service after power (soft off, charge mode). The UI and the app runtime
 * come last.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_err.h"
#include "t5_ota/ota.h"
#include "t5_abi/t5_abi.h"
#include "t5_battery/battery.h"
#include "t5_bsp/board.h"
#include "t5_bsp/panel.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/logring.h"
#include "t5_diag/t5_diag.h"
#include "t5_display/t5_display.h"
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
#include "t5_power/power.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/t5_time.h"
#include "t5_ui/t5_ui.h"
#include "board_media.h"
#include "board_power.h"

static const char *TAG = "main";

/* A service that fails to start is logged and boot goes on: the console
 * and whatever did start stay usable. The failures are summed up once boot
 * is done. */
#define BOOT_FAILS_MAX 8
static const char *s_failed[BOOT_FAILS_MAX];
static int s_nfailed;

static void start(const char *what, t5_err_t e)
{
    if (e == T5_OK) return;
    T5_LOGE(TAG, "%s failed: %s", what, t5_err_str(e));
    if (s_nfailed < BOOT_FAILS_MAX) s_failed[s_nfailed] = what;
    s_nfailed++;
}

static void boot_summary(void)
{
    if (!s_nfailed) {
        T5_LOGI(TAG, "boot complete");
        return;
    }
    char list[160] = "";
    size_t n = 0;
    for (int i = 0; i < s_nfailed && i < BOOT_FAILS_MAX; i++) {
        int w = snprintf(list + n, sizeof list - n, "%s%s", i ? ", " : "", s_failed[i]);
        if (w < 0 || (size_t)w >= sizeof list - n) break;
        n += (size_t)w;
    }
    T5_LOGE(TAG, "boot complete; %d failed to start: %s", s_nfailed, list);
}

static void log_event(t5_event_id_t id, const void *data, size_t len, void *arg)
{
    (void)len; (void)arg;
    if (id == T5_EV_BATTERY_UPDATE) {
        return;   /* once per percent; the console has 'bat' */
    }
    if (id == T5_EV_I2C_DEVICE_FAULT || id == T5_EV_I2C_DEVICE_OK) {
        const t5_ev_i2c_t *e = data;
        T5_LOGW(TAG, "event %s: 0x%02x err %s", t5_event_name(id), e->addr, t5_err_str((t5_err_t)e->err));
    } else {
        T5_LOGI(TAG, "event %s", t5_event_name(id));
    }
}

void app_main(void)
{
    t5_log_capture_start();
    /* Plan 9.1, 9.3: a new image is on trial until this boot proves healthy;
     * failed boots before this one may mean safe mode. */
    t5_ota_boot_check();
    t5_ota_set_restart_hook(t5_power_restart);
    t5_apps_set_safe_mode(t5_safe_mode());

    ESP_ERROR_CHECK(t5_event_init() == T5_OK ? ESP_OK : ESP_FAIL);
    t5_event_subscribe(T5_EV_ANY, log_event, NULL);

    start("board", t5_bsp_bringup());
    start("time", t5_time_init());
    start("imu", t5_imu_start());          /* background */
    start("storage", t5_storage_init());
    /* A charge-mode timer wake checks USB and goes back to deep sleep here,
     * with its settings (storage) but before the display or anything slow. */
    t5_power_read_wake_reason();
    if (t5_power_wake_reason() == T5_WAKE_CHARGE_TIMER) {
        t5_battery_charge_mode_resume();
    }
    t5_time_tz_load();                     /* local time zone, from /data */
    start("recovery flag", t5_ota_check_sd_flag());   /* plan 9.2: a RECOVERY file on the card */
    /* Before the display: saving and erasing a crash's core dump means long
     * flash operations, which would starve a running panel. */
    start("crash check", t5_apps_boot_check());
    start("panel", t5_bsp_panel_probe());
    start("display", t5_display_init());   /* black, backlight held off */
    start("touch", t5_touch_init());
    board_power_wire();
    start("power", t5_power_init());       /* after storage and display: settings, backlight */
    start("battery", t5_battery_init());
    start("net", t5_net_init());           /* background: powers the C6 and connects */
    start("ports", t5_ports_init());
    /* Phase 7: media and USB, after power (rails, wake locks, hooks). */
    start("audio", t5_audio_init());
    board_media_wire();
    start("camera", t5_camera_init());
    start("usb", t5_usb_init());
    /* The UI after power (suspend hooks) and touch (input); the app runtime
     * last, since it may restore the last app or run a scheduled alarm. */
    if (t5_display_ready()) {
        start("ui", t5_ui_init());
        start("shell", t5_shell_register());   /* the home screen, before the runtime shows it */
        start("system apps", t5_sysapps_register());
        start("python", t5_py_init());     /* the micropython runtime, before the app runtime */
        t5_abi_register();
        start("apps", t5_apps_init());     /* shows the launcher, then the backlight comes on */
        if (!t5_ui_ready() || !t5_apps_ready()) {
            t5_display_release_hold();     /* no UI: light the panel for console tools */
        }
    }

    /* Developer mode (plan 8.5): listens on the network only if it is on. */
    start("developer mode", t5_dev_init());
    ESP_ERROR_CHECK(t5_diag_start());
    boot_summary();
    /* Healthy once the shell is on the panel, or after 10 s (a boot without
     * a UI, where the console is the way in, right away). */
    if (t5_apps_ready()) t5_display_wait_shown(10000);
    t5_ota_boot_ok();
}
