/* SPDX-License-Identifier: Apache-2.0 */
#include "unity.h"
#include "t5_bsp/xpin.h"
#include "t5_bsp/xpin_test.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_mocks.h"

static mock_i2c_dev_t *e0, *e1;

void setUp(void)
{
    mock_reset_all();
    t5_xpin_reset_for_test();
    e0 = mock_expander_add(T5_I2C_ADDR_EXP0);
    e1 = mock_expander_add(T5_I2C_ADDR_EXP1);
}
void tearDown(void) {}

static void test_init_adopts_current_state(void)
{
    e0->regs[0x03] = 0x20;   /* TP_RST already an output... */
    e0->regs[0x05] = 0x20;   /* ...driven high */
    e0->regs[0x07] = 0x00;
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_init());
    TEST_ASSERT_EQUAL(0, e0->wlog_n);   /* nothing written: no glitch on P4 reset */
    TEST_ASSERT_EQUAL(T5_XPIN_MODE_OUTPUT, t5_xpin_mode(T5_XP_TP_RST));
    TEST_ASSERT_TRUE(t5_xpin_out_level(T5_XP_TP_RST));
}

static void test_init_rejects_wrong_chip(void)
{
    e1->regs[0x01] = 0x00;
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_xpin_init());
}

static void test_output_sets_level_before_direction(void)
{
    t5_xpin_init();
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_output(T5_XP_CHG_EN, true));
    TEST_ASSERT_EQUAL_HEX8(0x80, e1->regs[0x05] & 0x80);
    TEST_ASSERT_EQUAL_HEX8(0x80, e1->regs[0x03] & 0x80);
    TEST_ASSERT_EQUAL_HEX8(0x00, e1->regs[0x07] & 0x80);
    /* write order: OUT (0x05) before HIZ (0x07) before DIR (0x03) */
    int out_i = -1, hiz_i = -1, dir_i = -1;
    for (int i = 0; i < e1->wlog_n; i++) {
        int reg = (int)(e1->wlog[i] >> 8);
        if (reg == 0x05 && out_i < 0) out_i = i;
        if (reg == 0x07 && hiz_i < 0) hiz_i = i;
        if (reg == 0x03 && dir_i < 0) dir_i = i;
    }
    TEST_ASSERT_TRUE(out_i >= 0 && out_i < hiz_i && hiz_i < dir_i);
}

static void test_input_with_pull(void)
{
    t5_xpin_init();
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_input(T5_XP_LCD_RST, T5_XPIN_PULL_UP));
    TEST_ASSERT_EQUAL(T5_XPIN_MODE_INPUT, t5_xpin_mode(T5_XP_LCD_RST));
    TEST_ASSERT_EQUAL(T5_XPIN_PULL_UP, t5_xpin_pull(T5_XP_LCD_RST));
    TEST_ASSERT_EQUAL_HEX8(0x10, e0->regs[0x0D] & 0x10);
    TEST_ASSERT_EQUAL_HEX8(0x10, e0->regs[0x0B] & 0x10);
}

static void test_read_input(void)
{
    t5_xpin_init();
    e1->regs[0x0F] = 0x40;
    bool lvl = false;
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_read(T5_XP_CHG_STAT, &lvl));
    TEST_ASSERT_TRUE(lvl);
}

static void test_read_output_uses_output_register(void)
{
    t5_xpin_init();
    t5_xpin_output(T5_XP_CHG_EN, true);
    e1->regs[0x0F] = 0x00;            /* chip reports 0 on the input register for outputs */
    bool lvl = false;
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_read(T5_XP_CHG_EN, &lvl));
    TEST_ASSERT_TRUE(lvl);
    e1->regs[0x05] &= 0x7F;           /* chip lost its output state */
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_read(T5_XP_CHG_EN, &lvl));
    TEST_ASSERT_FALSE(lvl);
}

static void test_reapply_after_chip_reset(void)
{
    t5_xpin_init();
    t5_xpin_apply_boot_defaults();
    uint8_t want_dir = e1->regs[0x03], want_out = e1->regs[0x05];
    mock_expander_power_on(e1);   /* the chip lost power: registers back to defaults */
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_recover());   /* recovery callback re-applies the shadow */
    TEST_ASSERT_EQUAL_HEX8(want_dir, e1->regs[0x03]);
    TEST_ASSERT_EQUAL_HEX8(want_out, e1->regs[0x05]);
    TEST_ASSERT_EQUAL(1, t5_xpin_reapply_count());
}

static void test_boot_defaults(void)
{
    t5_xpin_init();
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_apply_boot_defaults());
    TEST_ASSERT_TRUE(t5_xpin_out_level(T5_XP_CHG_EN));
    TEST_ASSERT_FALSE(t5_xpin_out_level(T5_XP_PWROFF_PULSE));
    TEST_ASSERT_FALSE(t5_xpin_out_level(T5_XP_EXT5V_EN));
    TEST_ASSERT_FALSE(t5_xpin_out_level(T5_XP_WLAN_PWR_EN));
    TEST_ASSERT_EQUAL(T5_XPIN_MODE_INPUT, t5_xpin_mode(T5_XP_CHG_STAT));
    /* PWROFF_PULSE never driven high at any point */
    for (int i = 0; i < e1->wlog_n; i++) {
        if ((e1->wlog[i] >> 8) == 0x05) TEST_ASSERT_EQUAL_HEX8(0, e1->wlog[i] & 0x10);
    }
}

static void test_unknown_pin(void)
{
    t5_xpin_init();
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_xpin_output(T5_XPIN(0x42, 0), true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_adopts_current_state);
    RUN_TEST(test_init_rejects_wrong_chip);
    RUN_TEST(test_output_sets_level_before_direction);
    RUN_TEST(test_input_with_pull);
    RUN_TEST(test_read_input);
    RUN_TEST(test_read_output_uses_output_register);
    RUN_TEST(test_reapply_after_chip_reset);
    RUN_TEST(test_boot_defaults);
    RUN_TEST(test_unknown_pin);
    return UNITY_END();
}
