/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_battery/battery.h"
#include "t5_core/os.h"
#include "t5_display/t5_display.h"
#include "t5_imu/t5_imu.h"
#include "t5_input/t5_touch.h"
#include "t5_power/power.h"
#include "t5_ui/t5_ui.h"
#include "board_power.h"

#define CHARGE_SCREEN_MS 5000
#define CHARGE_SCREEN_PERCENT 30   /* backlight cap while it shows */

static void screen(bool lit, int cap_percent, void *arg)
{
    (void)arg;
    if (!t5_display_ready()) return;
    if (lit) {
        t5_display_dim(cap_percent);
        t5_display_blank(false);
    } else {
        t5_display_blank(true);
    }
}

static void standby(bool enter, bool motion, void *arg)
{
    (void)arg;
    if (enter) {
        if (motion && t5_imu_status() == T5_IMU_READY) {
            /* Active-low, latched any-motion: drives the PMS150G's power-on
             * trigger (bench 0.7). */
            const t5_imu_motion_cfg_t m = { .threshold_mg = 80, .duration_ms = 100, .pins = T5_IMU_INT1 | T5_IMU_INT2,
                                            .active_high = false, .open_drain = false, .latched = true };
            t5_imu_motion_enable(&m);
        }
        if (t5_display_ready()) t5_display_sleep();
    } else {
        if (motion) t5_imu_motion_disable();
        if (t5_display_ready()) t5_display_wake();
    }
}

static void low_power(bool on, void *arg)
{
    (void)arg;
    t5_ui_set_low_power(on);
}

static void on_touch(const t5_touch_state_t *st, void *arg)
{
    (void)st; (void)arg;
    t5_power_user_activity();
}

static void seg_digit(int x, int y, int s, int d, uint16_t c)
{
    /* 7-segment: a b c d e f g */
    static const uint8_t k[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    int w = s, h = s / 5, L = s;
    uint8_t m = k[d % 10];
    if (m & 0x01) t5_display_fill_rect(x + h, y, w, h, c);                 /* a */
    if (m & 0x02) t5_display_fill_rect(x + w + h, y + h, h, L, c);         /* b */
    if (m & 0x04) t5_display_fill_rect(x + w + h, y + 2 * h + L, h, L, c); /* c */
    if (m & 0x08) t5_display_fill_rect(x + h, y + 2 * L + 2 * h, w, h, c); /* d */
    if (m & 0x10) t5_display_fill_rect(x, y + 2 * h + L, h, L, c);         /* e */
    if (m & 0x20) t5_display_fill_rect(x, y + h, h, L, c);                 /* f */
    if (m & 0x40) t5_display_fill_rect(x + h, y + L + h, w, h, c);         /* g */
}

/* Straight into the framebuffer: by now the UI is suspended. Portrait. */
static void draw_charge_screen(int pct)
{
    const int W = T5_DISPLAY_W, H = T5_DISPLAY_H;
    t5_display_fill_rect(0, 0, W, H, 0);
    int bw = 240, bh = 440, bx = (W - bw) / 2, by = (H - bh) / 2 + 80, t = 12;
    uint16_t white = t5_rgb565(0xE0, 0xE0, 0xE0);
    t5_display_fill_rect(bx + bw / 2 - 50, by - 30, 100, 30, white);
    t5_display_fill_rect(bx, by, bw, t, white);
    t5_display_fill_rect(bx, by + bh - t, bw, t, white);
    t5_display_fill_rect(bx, by, t, bh, white);
    t5_display_fill_rect(bx + bw - t, by, t, bh, white);
    int p = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    uint16_t fill = p <= 15 ? t5_rgb565(0xE5, 0x39, 0x35) : p <= 40 ? t5_rgb565(0xFB, 0x8C, 0x00) : t5_rgb565(0x43, 0xA0, 0x47);
    int inner = bh - 4 * t, fh = inner * p / 100;
    t5_display_fill_rect(bx + 2 * t, by + 2 * t + inner - fh, bw - 4 * t, fh, fill);
    if (pct >= 0) {
        int s = 40, dw = s + 2 * (s / 5) + 16, n = p >= 100 ? 3 : p >= 10 ? 2 : 1;
        int x = (W - n * dw) / 2, y = by - 260;
        if (n == 3) { seg_digit(x, y, s, 1, white); x += dw; }
        if (n >= 2) { seg_digit(x, y, s, (p / 10) % 10, white); x += dw; }
        seg_digit(x, y, s, p % 10, white);
    }
}

static void charge_screen(int percent, void *arg)
{
    (void)arg;
    if (!t5_display_ready()) return;
    t5_display_dim(CHARGE_SCREEN_PERCENT);
    t5_display_blank(false);
    draw_charge_screen(percent);
    t5_delay_ms(CHARGE_SCREEN_MS);
    t5_display_sleep();
}

void board_power_wire(void)
{
    const t5_power_outputs_t out = { .screen = screen, .standby = standby, .low_power = low_power };
    t5_power_set_outputs(&out);
    t5_touch_set_activity_hook(on_touch, NULL);
    t5_battery_set_charge_screen(charge_screen, NULL);
}
