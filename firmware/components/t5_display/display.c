/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_bsp/xpin.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_display/t5_display.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_pins/pins.h"
#include "display_hw.h"

static const char *TAG = "t5.display";

#define ST712X_REG_FW_VERSION 0x0000
#define TOUCH_BOOT_MS 500   /* Espressif's BSP waits this long after TP_RST before probing */

static t5_panel_t s_panel = T5_PANEL_UNKNOWN;
static bool s_ready, s_asleep, s_blank;
static int s_brightness = 80;
static int s_dim = -1;
static uint16_t *s_fb;

static t5_err_t apply_backlight(void);

const char *t5_display_panel_name(t5_panel_t p)
{
    switch (p) {
    case T5_PANEL_ILI9881C_GT911: return "ILI9881C + GT911";
    case T5_PANEL_ST7123: return "ST7123 (touch fw 3)";
    case T5_PANEL_ST7121: return "ST7121 (touch fw 1)";
    default: return "unknown";
    }
}

t5_panel_t t5_display_variant_decide(bool st712x_acks, int st712x_fw, bool gt911_acks)
{
    if (st712x_acks) {
        if (st712x_fw == 3) return T5_PANEL_ST7123;
        if (st712x_fw == 1) return T5_PANEL_ST7121;
        return T5_PANEL_UNKNOWN;     /* new firmware: refuse rather than guess timings */
    }
    return gt911_acks ? T5_PANEL_ILI9881C_GT911 : T5_PANEL_UNKNOWN;
}

t5_err_t t5_display_probe(t5_panel_t *out)
{
    if (s_panel != T5_PANEL_UNKNOWN) {
        if (out) *out = s_panel;
        return T5_OK;
    }
    /* LCD reset released the way the BSP does it: open-drain with pull-up. */
    if (t5_xpin_mode(T5_XP_LCD_RST) != T5_XPIN_MODE_INPUT || t5_xpin_pull(T5_XP_LCD_RST) != T5_XPIN_PULL_UP) {
        T5_RETURN_ON_ERR(t5_xpin_input(T5_XP_LCD_RST, T5_XPIN_PULL_UP));
        t5_delay_ms(20);
    }
    bool tp_was_up = t5_xpin_mode(T5_XP_TP_RST) == T5_XPIN_MODE_OUTPUT && t5_xpin_out_level(T5_XP_TP_RST);
    if (!tp_was_up) {
        T5_RETURN_ON_ERR(t5_xpin_output(T5_XP_TP_RST, true));
        t5_delay_ms(TOUCH_BOOT_MS);
    }
    bool st = t5_i2c_probe(T5_I2C_ADDR_ST712X) == T5_OK;
    int fw = -1;
    if (st) {
        t5_i2c_dev_t *d;
        if (t5_i2c_open(T5_I2C_ADDR_ST712X, 400000, "st712x-probe", &d) == T5_OK) {
            uint8_t v;
            if (t5_i2c_read16(d, ST712X_REG_FW_VERSION, &v, 1) == T5_OK) {
                fw = v;
            }
            t5_i2c_close(d);
        }
    }
    bool gt = !st && (t5_i2c_probe(T5_I2C_ADDR_GT911) == T5_OK || t5_i2c_probe(T5_I2C_ADDR_GT911_ALT) == T5_OK);
    s_panel = t5_display_variant_decide(st, fw, gt);
    T5_LOGI(TAG, "panel: %s (st712x %s fw %d, gt911 %s)", t5_display_panel_name(s_panel),
            st ? "acks" : "absent", fw, gt ? "acks" : "absent");
    if (out) *out = s_panel;
    return s_panel == T5_PANEL_UNKNOWN ? T5_ERR_NOT_SUPPORTED : T5_OK;
}

t5_panel_t t5_display_panel(void)
{
    return s_panel;
}

t5_err_t t5_display_init(void)
{
    if (s_ready) {
        return T5_OK;
    }
    T5_RETURN_ON_ERR(t5_display_probe(NULL));
    T5_RETURN_ON_ERR(t5_display_hw_init(s_panel, &s_fb));
    s_ready = true;
    s_asleep = false;
    T5_RETURN_ON_ERR(apply_backlight());
    t5_event_post(T5_EV_DISPLAY_READY, NULL, 0);
    return T5_OK;
}

bool t5_display_ready(void)
{
    return s_ready;
}

int t5_display_effective_brightness(void)
{
    if (!s_ready || s_asleep || s_blank) {
        return 0;
    }
    return (s_dim >= 0 && s_dim < s_brightness) ? s_dim : s_brightness;
}

static t5_err_t apply_backlight(void)
{
    return s_ready ? t5_display_hw_backlight(t5_display_effective_brightness()) : T5_OK;
}

t5_err_t t5_display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_brightness = percent;
    return apply_backlight();   /* a no-op until init; 0 while asleep or blanked */
}

t5_err_t t5_display_blank(bool blank)
{
    if (!s_ready) return T5_ERR_STATE;
    s_blank = blank;
    return apply_backlight();
}

bool t5_display_blanked(void)
{
    return s_blank;
}

t5_err_t t5_display_dim(int percent)
{
    if (percent > 100) percent = 100;
    s_dim = percent < 0 ? -1 : percent;
    return apply_backlight();
}

t5_err_t t5_display_set_backlight_freq(uint32_t hz)
{
    if (!s_ready) return T5_ERR_STATE;
    if (hz < 100 || hz > 40000) return T5_ERR_INVALID_ARG;
    T5_RETURN_ON_ERR(t5_display_hw_backlight_freq(hz));
    return apply_backlight();
}

int t5_display_brightness(void)
{
    return s_brightness;
}

t5_err_t t5_display_sleep(void)
{
    if (!s_ready) return T5_ERR_STATE;
    if (s_asleep) return T5_OK;
    T5_RETURN_ON_ERR(t5_display_hw_backlight(0));
    T5_RETURN_ON_ERR(t5_display_hw_sleep(true));
    s_asleep = true;
    return T5_OK;
}

t5_err_t t5_display_wake(void)
{
    if (!s_ready) return T5_ERR_STATE;
    if (!s_asleep) return T5_OK;
    T5_RETURN_ON_ERR(t5_display_hw_sleep(false));
    s_asleep = false;
    return apply_backlight();
}

bool t5_display_asleep(void)
{
    return s_asleep;
}

uint16_t *t5_display_fb(void)
{
    return s_fb;
}

t5_err_t t5_display_flush_rows(int y0, int y1)
{
    if (!s_fb) return T5_ERR_STATE;
    if (y0 < 0) y0 = 0;
    if (y1 > T5_DISPLAY_H) y1 = T5_DISPLAY_H;
    if (y0 >= y1) return T5_OK;
    return t5_display_hw_flush(s_fb, y0, y1);
}

t5_err_t t5_display_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    if (!s_fb) return T5_ERR_STATE;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > T5_DISPLAY_W ? T5_DISPLAY_W : x + w;
    int y1 = y + h > T5_DISPLAY_H ? T5_DISPLAY_H : y + h;
    if (x0 >= x1 || y0 >= y1) return T5_OK;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = s_fb + (size_t)yy * T5_DISPLAY_W;
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = c;
        }
    }
    return t5_display_flush_rows(y0, y1);
}

t5_err_t t5_display_test_pattern(void)
{
    if (!s_fb) return T5_ERR_STATE;
    static const uint8_t bars[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
    };
    const int W = T5_DISPLAY_W, H = T5_DISPLAY_H;
    for (int y = 0; y < H; y++) {
        uint16_t *row = s_fb + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            uint16_t c;
            if (y < H / 2) {
                const uint8_t *b = bars[x * 8 / W];
                c = t5_rgb565(b[0], b[1], b[2]);
            } else if (y < H * 5 / 8) {
                uint8_t g = (uint8_t)(x * 255 / (W - 1));
                c = t5_rgb565(g, g, g);
            } else {
                c = t5_rgb565(0x12, 0x12, 0x12);   /* MD2 dark surface */
                if (x % 40 == 0 || y % 40 == 0) c = t5_rgb565(0x40, 0x40, 0x40);
            }
            /* 4 px white border, red top-left marker: shows orientation and overscan. */
            if (x < 4 || y < 4 || x >= W - 4 || y >= H - 4) c = 0xFFFF;
            if (x >= 8 && x < 72 && y >= 8 && y < 72) c = t5_rgb565(255, 0, 0);
            row[x] = c;
        }
    }
    return t5_display_flush_rows(0, H);
}

t5_err_t t5_display_read_ids(uint8_t id[3], uint8_t *power_mode)
{
    if (!s_ready) return T5_ERR_STATE;
    T5_RETURN_ON_ERR(t5_display_hw_read(0x04, id, 3));
    return t5_display_hw_read(0x0A, power_mode, 1);
}

void t5_display_reset_for_test(void)
{
    s_panel = T5_PANEL_UNKNOWN;
    s_ready = false;
    s_asleep = false;
    s_blank = false;
    s_dim = -1;
    s_fb = NULL;
    s_brightness = 80;
}
