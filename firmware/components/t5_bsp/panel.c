/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_bsp/panel.h"
#include "t5_bsp/panel_test.h"
#include "t5_bsp/xpin.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_pins/pins.h"

static const char *TAG = "t5.panel";

#define ST712X_REG_FW_VERSION 0x0000
#define LCD_RST_SETTLE_MS 20
#define TOUCH_BOOT_MS 500   /* Espressif's BSP waits this long after TP_RST before probing */

static t5_panel_info_t s_info;

const char *t5_bsp_panel_name(t5_panel_t p)
{
    switch (p) {
    case T5_PANEL_ILI9881C_GT911: return "ILI9881C + GT911";
    case T5_PANEL_ST7123: return "ST7123 (touch fw 3)";
    case T5_PANEL_ST7121: return "ST7121 (touch fw 1)";
    default: return "unknown";
    }
}

/* st712x_fw < 0 if unread. */
static t5_panel_t decide(bool st712x_acks, int st712x_fw, bool gt911_acks)
{
    if (st712x_acks) {
        if (st712x_fw == 3) return T5_PANEL_ST7123;
        if (st712x_fw == 1) return T5_PANEL_ST7121;
        return T5_PANEL_UNKNOWN;     /* new firmware: refuse rather than guess timings */
    }
    return gt911_acks ? T5_PANEL_ILI9881C_GT911 : T5_PANEL_UNKNOWN;
}

static void touch_mirror(t5_panel_t panel, bool *mirror_x, bool *mirror_y)
{
    bool flip = panel == T5_PANEL_ST7123;   /* T5_ST7123_MADCTL */
    *mirror_x = flip;
    *mirror_y = flip;
}

static t5_err_t release_resets(void)
{
    /* LCD reset released the way the BSP does it: open-drain with pull-up. */
    if (t5_xpin_mode(T5_XP_LCD_RST) != T5_XPIN_MODE_INPUT || t5_xpin_pull(T5_XP_LCD_RST) != T5_XPIN_PULL_UP) {
        T5_RETURN_ON_ERR(t5_xpin_input(T5_XP_LCD_RST, T5_XPIN_PULL_UP));
        t5_delay_ms(LCD_RST_SETTLE_MS);
    }
    if (t5_xpin_mode(T5_XP_TP_RST) != T5_XPIN_MODE_OUTPUT || !t5_xpin_out_level(T5_XP_TP_RST)) {
        T5_RETURN_ON_ERR(t5_xpin_output(T5_XP_TP_RST, true));
        t5_delay_ms(TOUCH_BOOT_MS);
    }
    return T5_OK;
}

static int read_st712x_fw(void)
{
    t5_i2c_dev_t *d;
    if (t5_i2c_open(T5_I2C_ADDR_ST712X, T5_SYS_I2C_HZ, "st712x-probe", &d) != T5_OK) {
        return -1;
    }
    uint8_t v;
    int fw = t5_i2c_read16(d, ST712X_REG_FW_VERSION, &v, 1) == T5_OK ? v : -1;
    t5_i2c_close(d);
    return fw;
}

t5_err_t t5_bsp_panel_probe(void)
{
    if (s_info.panel != T5_PANEL_UNKNOWN) {
        return T5_OK;
    }
    T5_RETURN_ON_ERR(release_resets());
    bool st = t5_i2c_probe(T5_I2C_ADDR_ST712X) == T5_OK;
    int fw = st ? read_st712x_fw() : -1;
    uint8_t gt_addr = 0;
    if (!st) {
        if (t5_i2c_probe(T5_I2C_ADDR_GT911) == T5_OK) gt_addr = T5_I2C_ADDR_GT911;
        else if (t5_i2c_probe(T5_I2C_ADDR_GT911_ALT) == T5_OK) gt_addr = T5_I2C_ADDR_GT911_ALT;
    }
    t5_panel_info_t info = {
        .panel = decide(st, fw, gt_addr != 0),
        .touch_addr = st ? T5_I2C_ADDR_ST712X : gt_addr,
        .touch_fw = fw > 0 ? (uint8_t)fw : 0,
    };
    touch_mirror(info.panel, &info.touch_mirror_x, &info.touch_mirror_y);
    T5_LOGI(TAG, "panel: %s (st712x %s fw %d, gt911 %s)", t5_bsp_panel_name(info.panel), st ? "acks" : "absent", fw,
            gt_addr ? "acks" : "absent");
    if (info.panel == T5_PANEL_UNKNOWN) {
        return T5_ERR_NOT_SUPPORTED;
    }
    s_info = info;
    return T5_OK;
}

const t5_panel_info_t *t5_bsp_panel(void)
{
    return &s_info;
}

void t5_bsp_panel_reset_for_test(void)
{
    s_info = (t5_panel_info_t){ 0 };
}
