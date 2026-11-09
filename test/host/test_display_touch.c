/* SPDX-License-Identifier: Apache-2.0 */
#include "unity.h"
#include "t5_bsp/panel.h"
#include "t5_bsp/panel_test.h"
#include "t5_bsp/xpin.h"
#include "t5_display/t5_display.h"
#include "t5_display/t5_display_test.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_input/t5_touch.h"
#include "t5_input/t5_touch_test.h"
#include "t5_mocks.h"
#include "display_hw.h"

void setUp(void)
{
    mock_reset_all();
    t5_bsp_panel_reset_for_test();
    t5_display_reset_for_test();
    mock_expanders_start();
}
void tearDown(void) {}

static void test_probe_st7121_unmirrored(void)
{
    mock_st712x_add(1);
    TEST_ASSERT_EQUAL(T5_OK, t5_bsp_panel_probe());
    const t5_panel_info_t *p = t5_bsp_panel();
    TEST_ASSERT_EQUAL(T5_PANEL_ST7121, p->panel);
    TEST_ASSERT_FALSE(p->touch_mirror_x || p->touch_mirror_y);
}

static void test_probe_refuses_unknown_st712x_firmware(void)
{
    mock_st712x_add(7);
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_bsp_panel_probe());
    TEST_ASSERT_EQUAL(T5_PANEL_UNKNOWN, t5_bsp_panel()->panel);
}

static void test_probe_st7123_releases_resets_and_reads_once(void)
{
    mock_i2c_dev_t *tp = mock_st712x_add(3);
    TEST_ASSERT_EQUAL(T5_OK, t5_bsp_panel_probe());
    const t5_panel_info_t *p = t5_bsp_panel();
    TEST_ASSERT_EQUAL(T5_PANEL_ST7123, p->panel);
    TEST_ASSERT_EQUAL_HEX8(T5_I2C_ADDR_ST712X, p->touch_addr);
    TEST_ASSERT_EQUAL(3, p->touch_fw);
    TEST_ASSERT_TRUE(p->touch_mirror_x && p->touch_mirror_y);
    TEST_ASSERT_EQUAL(T5_XPIN_MODE_INPUT, t5_xpin_mode(T5_XP_LCD_RST));
    TEST_ASSERT_EQUAL(T5_XPIN_PULL_UP, t5_xpin_pull(T5_XP_LCD_RST));
    TEST_ASSERT_TRUE(t5_xpin_out_level(T5_XP_TP_RST));
    uint32_t reads = tp->reads;
    TEST_ASSERT_EQUAL(T5_OK, t5_bsp_panel_probe());
    TEST_ASSERT_EQUAL(reads, tp->reads);
}

static void test_probe_gt911_alt_address(void)
{
    mock_i2c_add(T5_I2C_ADDR_GT911_ALT, true);
    TEST_ASSERT_EQUAL(T5_OK, t5_bsp_panel_probe());
    const t5_panel_info_t *p = t5_bsp_panel();
    TEST_ASSERT_EQUAL(T5_PANEL_ILI9881C_GT911, p->panel);
    TEST_ASSERT_EQUAL_HEX8(T5_I2C_ADDR_GT911_ALT, p->touch_addr);
    TEST_ASSERT_FALSE(p->touch_mirror_x || p->touch_mirror_y);
}

static void test_probe_nothing(void)
{
    TEST_ASSERT_EQUAL(T5_ERR_NOT_SUPPORTED, t5_bsp_panel_probe());
    TEST_ASSERT_EQUAL(T5_PANEL_UNKNOWN, t5_bsp_panel()->panel);
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_display_init());
}

static void test_touch_uses_the_probed_controller(void)
{
    mock_i2c_dev_t *gt = mock_i2c_add(T5_I2C_ADDR_GT911_ALT, true);
    gt->regs[0x8048] = 0xD0; gt->regs[0x8049] = 0x02;   /* 720 */
    gt->regs[0x804A] = 0x00; gt->regs[0x804B] = 0x05;   /* 1280 */
    TEST_ASSERT_EQUAL(T5_OK, t5_touch_init());
    t5_touch_info_t i;
    t5_touch_info(&i);
    TEST_ASSERT_TRUE(i.polling);
    TEST_ASSERT_EQUAL(720, i.max_x);
    TEST_ASSERT_EQUAL(1280, i.max_y);
    TEST_ASSERT_FALSE(t5_display_ready());   /* touch doesn't need the display */
}

static void test_brightness_and_sleep(void)
{
    mock_st712x_add(3);
    TEST_ASSERT_EQUAL(T5_OK, t5_display_init());
    TEST_ASSERT_EQUAL(T5_OK, t5_display_set_brightness(60));
    TEST_ASSERT_EQUAL(0, mock_backlight);        /* held off until the first frame */
    TEST_ASSERT_EQUAL(0, t5_display_effective_brightness());
    TEST_ASSERT_EQUAL(T5_OK, t5_display_release_hold());
    TEST_ASSERT_EQUAL(60, mock_backlight);
    t5_display_set_brightness(150);
    TEST_ASSERT_EQUAL(100, t5_display_brightness());
    t5_display_set_brightness(-5);
    TEST_ASSERT_EQUAL(0, t5_display_brightness());
    t5_display_set_brightness(100);
    TEST_ASSERT_EQUAL(T5_OK, t5_display_dim(30));
    TEST_ASSERT_EQUAL(30, mock_backlight);
    TEST_ASSERT_EQUAL(100, t5_display_brightness());   /* the cap leaves the setting alone */
    TEST_ASSERT_EQUAL(T5_OK, t5_display_dim(-1));
    TEST_ASSERT_EQUAL(T5_OK, t5_display_sleep());
    TEST_ASSERT_EQUAL(0, mock_backlight);
    TEST_ASSERT_TRUE(mock_panel_asleep);
    t5_display_set_brightness(40);          /* remembered, not applied while asleep */
    TEST_ASSERT_EQUAL(0, mock_backlight);
    TEST_ASSERT_EQUAL(T5_OK, t5_display_wake());
    TEST_ASSERT_EQUAL(40, mock_backlight);
    TEST_ASSERT_FALSE(mock_panel_asleep);
}

static void test_shown_after_the_first_frame_past_the_hold(void)
{
    mock_st712x_add(3);
    TEST_ASSERT_FALSE(t5_display_wait_shown(0));
    TEST_ASSERT_EQUAL(T5_OK, t5_display_init());
    t5_display_frame_isr();                      /* black frames while held */
    TEST_ASSERT_FALSE(t5_display_wait_shown(0));
    TEST_ASSERT_EQUAL(T5_OK, t5_display_release_hold());
    TEST_ASSERT_FALSE(t5_display_wait_shown(0));
    TEST_ASSERT_TRUE(t5_display_frame_isr());
    TEST_ASSERT_FALSE(t5_display_frame_isr());   /* signalled once */
    TEST_ASSERT_TRUE(t5_display_wait_shown(0));
}

static void test_init_black_and_fill_clip(void)
{
    mock_st712x_add(3);
    TEST_ASSERT_EQUAL(T5_OK, t5_display_init());
    uint16_t *fb = t5_display_fb();
    for (int i = 0; i < T5_DISPLAY_FBS; i++) {
        uint16_t *b = t5_display_fb_n(i);
        TEST_ASSERT_EQUAL_HEX16(0, b[0]);
        TEST_ASSERT_EQUAL_HEX16(0, b[T5_DISPLAY_W * T5_DISPLAY_H - 1]);
    }
    TEST_ASSERT_EQUAL(T5_OK, t5_display_fill_rect(-10, -10, 20, 20, 0x1234));    /* clipped */
    TEST_ASSERT_EQUAL_HEX16(0x1234, fb[0]);
    TEST_ASSERT_EQUAL_HEX16(0x1234, fb[9 * T5_DISPLAY_W + 9]);
    TEST_ASSERT_NOT_EQUAL(0x1234, fb[10 * T5_DISPLAY_W + 10]);
    TEST_ASSERT_EQUAL(T5_OK, t5_display_fill_rect(T5_DISPLAY_W - 2, T5_DISPLAY_H - 2, 10, 10, 0x4321));
    TEST_ASSERT_EQUAL_HEX16(0x4321, fb[T5_DISPLAY_W * T5_DISPLAY_H - 1]);
}

static void test_parse_st712x(void)
{
    const uint8_t r[3 * 7] = {
        0x80 | 0x02, 0xCF, 0x04, 0xFF, 30, 99, 0,   /* valid: x 0x2CF = 719, y 0x4FF = 1279 */
        0x00, 0x10, 0x00, 0x10, 0, 0, 0,            /* invalid slot */
        0x80, 0x05, 0x00, 0x06, 12, 50, 0,          /* valid: (5, 6) */
    };
    t5_touch_point_t p[5];
    TEST_ASSERT_EQUAL(2, t5_touch_parse_st712x(r, 3, p, 5));
    TEST_ASSERT_EQUAL(719, p[0].x);
    TEST_ASSERT_EQUAL(1279, p[0].y);
    TEST_ASSERT_EQUAL(30, p[0].strength);
    TEST_ASSERT_EQUAL(0, p[0].id);
    TEST_ASSERT_EQUAL(5, p[1].x);
    TEST_ASSERT_EQUAL(2, p[1].id);
    TEST_ASSERT_EQUAL(1, t5_touch_parse_st712x(r, 3, p, 1));   /* respects max */
}

static void test_parse_gt911(void)
{
    t5_touch_point_t p[5];
    const uint8_t pts[16] = { 7, 0x10, 0x01, 0x20, 0x02, 40, 0, 0, 1, 1, 0, 2, 0, 9, 0, 0 };
    TEST_ASSERT_EQUAL(-1, t5_touch_parse_gt911(0x02, pts, p, 5));   /* not ready */
    TEST_ASSERT_EQUAL(2, t5_touch_parse_gt911(0x82, pts, p, 5));
    TEST_ASSERT_EQUAL(7, p[0].id);
    TEST_ASSERT_EQUAL(0x110, p[0].x);
    TEST_ASSERT_EQUAL(0x220, p[0].y);
    TEST_ASSERT_EQUAL(0, t5_touch_parse_gt911(0x80, pts, p, 5));
}

static void test_scale_and_clamp(void)
{
    t5_touch_point_t p = { .x = 360, .y = 640 };
    t5_touch_scale(&p, 720, 1280);
    TEST_ASSERT_EQUAL(360, p.x);
    t5_touch_point_t q = { .x = 1080, .y = 1920 };
    t5_touch_scale(&q, 1080, 1920);
    TEST_ASSERT_EQUAL(719, q.x);
    TEST_ASSERT_EQUAL(1279, q.y);
    t5_touch_point_t r = { .x = 540, .y = 960 };
    t5_touch_scale(&r, 1080, 1920);
    TEST_ASSERT_EQUAL(360, r.x);
    TEST_ASSERT_EQUAL(640, r.y);
}

static void test_mirror(void)
{
    t5_touch_point_t p = { .x = 719, .y = 0 };   /* controller top-right */
    t5_touch_mirror(&p, true, true);
    TEST_ASSERT_EQUAL(0, p.x);                   /* framebuffer bottom-left */
    TEST_ASSERT_EQUAL(1279, p.y);
}

static void test_sample_st712x_over_i2c(void)
{
    mock_i2c_dev_t *tp = mock_st712x_add(3);
    TEST_ASSERT_EQUAL(T5_OK, t5_bsp_panel_probe());
    t5_i2c_dev_t *d;
    TEST_ASSERT_EQUAL_INT(T5_OK, t5_i2c_open(T5_I2C_ADDR_ST712X, 0, "touch", &d));
    t5_touch_setup_for_test(d, t5_bsp_panel(), 720, 1280, 2);
    tp->regs[0x0010] = 0x08;                     /* report has coordinates */
    const uint8_t rep[7] = { 0x80 | 0x01, 0x00, 0x02, 0x00, 9, 9, 0 };
    for (int i = 0; i < 7; i++) tp->regs[0x0014 + i] = rep[i];
    t5_touch_state_t s;
    TEST_ASSERT_EQUAL(T5_OK, t5_touch_sample(&s));
    TEST_ASSERT_EQUAL(1, s.count);
    TEST_ASSERT_EQUAL(719 - 256, s.p[0].x);      /* ST7123: mirrored into framebuffer space */
    TEST_ASSERT_EQUAL(1279 - 512, s.p[0].y);
    tp->regs[0x0010] = 0x00;                     /* finger lifted */
    TEST_ASSERT_EQUAL(T5_OK, t5_touch_sample(&s));
    TEST_ASSERT_EQUAL(0, s.count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_probe_st7121_unmirrored);
    RUN_TEST(test_probe_refuses_unknown_st712x_firmware);
    RUN_TEST(test_probe_st7123_releases_resets_and_reads_once);
    RUN_TEST(test_probe_gt911_alt_address);
    RUN_TEST(test_probe_nothing);
    RUN_TEST(test_touch_uses_the_probed_controller);
    RUN_TEST(test_brightness_and_sleep);
    RUN_TEST(test_shown_after_the_first_frame_past_the_hold);
    RUN_TEST(test_init_black_and_fill_clip);
    RUN_TEST(test_parse_st712x);
    RUN_TEST(test_parse_gt911);
    RUN_TEST(test_scale_and_clamp);
    RUN_TEST(test_mirror);
    RUN_TEST(test_sample_st712x_over_i2c);
    return UNITY_END();
}
