/* SPDX-License-Identifier: Apache-2.0
 * Stubs for the thin hardware layers: system clock, storage, display, touch.
 */
#include <stdlib.h>
#include <string.h>
#include "t5_mocks.h"
#include "t5_time_hw.h"
#include "t5_storage_hw.h"
#include "display_hw.h"
#include "touch_hw.h"

/* time */
time_t mock_system_time;
void t5_time_hw_set_system(time_t utc) { mock_system_time = utc; }
time_t t5_time_hw_get_system(void) { return mock_system_time; }

/* storage */
mock_sd_t mock_sd = { .mount_result = T5_ERR_NOT_FOUND };
t5_err_t t5_storage_hw_data_mount(void) { return T5_OK; }
t5_err_t t5_storage_hw_data_usage(uint64_t *total, uint64_t *used) { *total = 4 << 20; *used = 8192; return T5_OK; }
t5_err_t t5_storage_hw_sd_mount(void)
{
    if (mock_sd.mount_result == T5_OK) { mock_sd.mounts++; mock_sd.alive = true; }
    return mock_sd.mount_result;
}
bool t5_storage_hw_sd_alive(void) { return mock_sd.alive; }
void t5_storage_hw_sd_unmount(void) { mock_sd.unmounts++; }
t5_err_t t5_storage_hw_sd_format(void) { return T5_OK; }
t5_err_t t5_storage_hw_sd_raw_open(void **card) { *card = NULL; return T5_OK; }
void t5_storage_hw_sd_raw_close(void *card) { (void)card; }
t5_err_t t5_storage_hw_sd_info(t5_sd_info_t *out) { memset(out, 0, sizeof *out); strcpy(out->name, "MOCK"); return T5_OK; }

/* display */
int mock_backlight = -1;
bool mock_panel_asleep;
static uint16_t *s_fbs[T5_DISPLAY_FBS];
t5_err_t t5_display_hw_init(t5_panel_t panel, uint16_t *fbs[T5_DISPLAY_FBS])
{
    (void)panel;
    for (int i = 0; i < T5_DISPLAY_FBS; i++) {
        if (!s_fbs[i]) s_fbs[i] = calloc((size_t)T5_DISPLAY_W * T5_DISPLAY_H, 2);
        if (!s_fbs[i]) return T5_ERR_NO_MEM;
        fbs[i] = s_fbs[i];
    }
    return T5_OK;
}
uint16_t *mock_display_presented;
t5_err_t t5_display_hw_present(uint16_t *fb, bool cache_sync) { (void)cache_sync; mock_display_presented = fb; return T5_OK; }
t5_err_t t5_display_hw_backlight(int percent) { mock_backlight = percent; return T5_OK; }
t5_err_t t5_display_hw_backlight_freq(uint32_t hz) { (void)hz; return T5_OK; }
t5_err_t t5_display_hw_sleep(bool sleep) { mock_panel_asleep = sleep; return T5_OK; }
t5_err_t t5_display_hw_flush(uint16_t *fb, int y0, int y1) { (void)fb; (void)y0; (void)y1; return T5_OK; }
t5_err_t t5_display_hw_read(uint8_t cmd, uint8_t *buf, int len) { (void)cmd; memset(buf, 0x5A, (size_t)len); return T5_OK; }

/* touch */
t5_err_t t5_touch_hw_init(bool use_irq) { (void)use_irq; return T5_OK; }
bool t5_touch_hw_wait(uint32_t timeout_ms) { (void)timeout_ms; return false; }
