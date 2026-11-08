/* SPDX-License-Identifier: Apache-2.0
 * USB-C role (plan 7.8, bench check 6). Console by default: USB-Serial/JTAG
 * on full-speed PHY 0 (GPIO 24/25). Mass storage mode:
 *   1. unmount /sd and borrow the raw card (t5_storage_sd_lend)
 *   2. start TinyUSB MSC on the full-speed OTG controller
 *   3. swap the PHYs, so OTG takes PHY 0 (the USB-C pins) and USJ moves to
 *      PHY 1, with its pads off (PHY 1's pads are I2S pins on the Tab5)
 * Leaving reverses it: when the computer ejects the card, on request
 * (Settings), or after an optional timeout. /data is never exposed.
 */
#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"
#include "soc/lp_system_struct.h"
#include "soc/usb_serial_jtag_struct.h"
#include "soc/usb_wrap_struct.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_storage/t5_storage.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tusb.h"
#include "usb_priv.h"

static const char *TAG = "t5.usbc";

static t5_usbc_mode_t s_mode = T5_USBC_CONSOLE;
static void *s_card;
static tinyusb_msc_storage_handle_t s_store;
static esp_timer_handle_t s_timer;
static void msc_event(tinyusb_msc_storage_handle_t h, tinyusb_msc_event_t *ev, void *arg);
static t5_mutex_t s_mx;

static void phy_log(const char *when)
{
    T5_LOGW(TAG, "%s: LP_SYS.usb_ctrl 0x%08x (sw_hw_sel %u, sw_sel %u), USJ pad %u, OTG pad %u", when,
            (unsigned)LP_SYS.usb_ctrl.val, (unsigned)LP_SYS.usb_ctrl.sw_hw_usb_phy_sel,
            (unsigned)LP_SYS.usb_ctrl.sw_usb_phy_sel, (unsigned)USB_SERIAL_JTAG.conf0.usb_pad_enable,
            (unsigned)usb_wrap_ll_phy_is_pad_enabled(&USB_WRAP));
}

static void phy_log(const char *when);

/* The log lines here are load-bearing: without a console write right after
 * USB-Serial/JTAG gets its PHY back, its console never answers again (seen
 * on Unit A, 2026-11-06; the 20 ms settles alone don't fix it). */
static void phy_to_otg(bool otg)
{
    phy_log("before");
    if (otg) {
        usb_serial_jtag_ll_phy_enable_pad(false);
        usb_wrap_ll_phy_select(&USB_WRAP, 0);
    } else {
        usb_wrap_ll_phy_select(&USB_WRAP, 1);
        t5_delay_ms(20);
        usb_serial_jtag_ll_phy_enable_pad(true);
        t5_delay_ms(20);
    }
    phy_log("after");
}

static t5_err_t storage_on(void)
{
    T5_RETURN_ON_ERR(t5_storage_sd_lend(&s_card));
    /* App-side mounting on: it is how an eject is reported (msc_event). */
    const tinyusb_msc_driver_config_t dc = { .callback = msc_event, .callback_arg = NULL };
    tinyusb_msc_storage_config_t sc = { .medium = { .card = s_card }, .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB };
    sc.fat_fs.do_not_format = true;
    tinyusb_config_t tc = TINYUSB_DEFAULT_CONFIG();
    tc.port = TINYUSB_PORT_FULL_SPEED_0;
    esp_err_t e = tinyusb_msc_install_driver(&dc);
    if (e == ESP_OK) e = tinyusb_msc_new_storage_sdmmc(&sc, &s_store);
    /* Swap the PHYs before TinyUSB sets its PHY up, so its pad and pull-up
     * settings land on the USB-C pins (PHY 0). */
    if (e == ESP_OK) phy_to_otg(true);
    if (e == ESP_OK) e = tinyusb_driver_install(&tc);
    if (e == ESP_OK) {
        usb_wrap_ll_phy_select(&USB_WRAP, 0);      /* in case the PHY driver reset the mapping */
        usb_wrap_ll_phy_enable_pad(&USB_WRAP, true);
        phy_log("tinyusb up");
    }
    if (e != ESP_OK) {
        T5_LOGE(TAG, "mass storage start failed: %s", esp_err_to_name(e));
        if (s_store) tinyusb_msc_delete_storage(s_store);
        s_store = NULL;
        tinyusb_msc_uninstall_driver();
        phy_to_otg(false);
        t5_storage_sd_reclaim(s_card);
        s_card = NULL;
        return T5_ERR_IO;
    }
    return T5_OK;
}


/* Where a hand-back got to, kept across a reset (`usbc trace`). */
RTC_NOINIT_ATTR static uint32_t s_trace;

uint32_t t5_usbc_trace(void)
{
    return s_trace;
}

static void storage_off(void)
{
    s_trace = 0x7B000001;
    tinyusb_driver_uninstall();
    s_trace = 0x7B000002;
    phy_to_otg(false);
    s_trace = 0x7B000003;
    if (s_store) tinyusb_msc_delete_storage(s_store);
    s_trace = 0x7B000004;
    s_store = NULL;
    tinyusb_msc_uninstall_driver();
    s_trace = 0x7B000005;
    if (s_card) t5_storage_sd_reclaim(s_card);
    s_trace = 0x7B000006;
    s_card = NULL;
}

static volatile bool s_ejected;

/* Taking the card back unmounts filesystems, stops TinyUSB and remounts
 * /sd: too much for the esp_timer task's small stack, so it runs on its own
 * short-lived task. */
static void handback_task(void *arg)
{
    (void)arg;
    if (s_ejected) T5_LOGI(TAG, "the computer ejected the SD card");
    t5_usbc_set_mode(T5_USBC_CONSOLE, 0);
    vTaskDelete(NULL);
}

static void timeout_cb(void *arg)
{
    (void)arg;
    xTaskCreatePinnedToCore(handback_task, "usbc.back", 8192, NULL, 5, NULL, 0);
}

/* The computer's eject is a SCSI START STOP UNIT (LoEj set, Start clear).
 * esp_tinyusb answers it by mounting the card for the app (here at /usbc,
 * never /data), which reports MOUNT_COMPLETE. The same happens when the
 * cable is pulled; then the host is gone (tud_mounted() false) and the card
 * stays reserved for the computer: plugging back in hands it over again.
 * The switch back runs on the timer task, as TinyUSB can't uninstall itself
 * from its own callback. */
static void msc_event(tinyusb_msc_storage_handle_t h, tinyusb_msc_event_t *ev, void *arg)
{
    (void)h; (void)arg;
    bool to_app = ev->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP &&
                  (ev->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE || ev->id == TINYUSB_MSC_EVENT_MOUNT_FAILED);
    if (!to_app || s_mode != T5_USBC_STORAGE || !s_timer) return;
    if (!tud_mounted()) {
        T5_LOGW(TAG, "USB-C unplugged without an eject: the SD card stays with the computer");
        return;
    }
    s_ejected = true;
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, 300 * 1000);   /* let the host see the command complete */
}

bool t5_usbc_ejected(void)
{
    return s_ejected;
}

t5_err_t t5_usbc_set_mode(t5_usbc_mode_t mode, uint32_t timeout_s)
{
    if (!s_mx) return T5_ERR_STATE;
    t5_mutex_lock(s_mx);
    t5_err_t e = T5_OK;
    if (mode != s_mode) {
        if (mode == T5_USBC_STORAGE) {
            s_ejected = false;
            T5_LOGW(TAG, "USB-C: mass storage (the console goes away %s)",
                    timeout_s ? "until the timeout" : "until the computer ejects the card");
            t5_delay_ms(100);                  /* let the log line out */
            e = storage_on();
        } else {
            storage_off();
            T5_LOGI(TAG, "USB-C: console");
        }
        if (e == T5_OK) {
            s_mode = mode;
            t5_ev_usb_t ev = { .mounted = mode == T5_USBC_STORAGE };
            t5_event_post(T5_EV_USBC_MODE, &ev, sizeof ev);
        }
    }
    if (s_timer) esp_timer_stop(s_timer);
    if (e == T5_OK && mode == T5_USBC_STORAGE && timeout_s && s_timer) {
        esp_timer_start_once(s_timer, (uint64_t)timeout_s * 1000000u);
    }
    t5_mutex_unlock(s_mx);
    return e;
}

t5_usbc_mode_t t5_usbc_mode(void)
{
    return s_mode;
}

void usbc_init(void)
{
    s_mx = t5_mutex_create();
    const esp_timer_create_args_t a = { .callback = timeout_cb, .name = "usbc", .dispatch_method = ESP_TIMER_TASK };
    esp_timer_create(&a, &s_timer);
}
