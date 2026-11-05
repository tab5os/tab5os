/* SPDX-License-Identifier: Apache-2.0
 * The pin map and the name tables must agree, since the console and later the
 * Port Inspector show these names to people debugging hardware.
 */
#include <string.h>
#include "unity.h"
#include "t5_pins/pins.h"
#include "t5_pins/names.h"

void setUp(void) {}
void tearDown(void) {}

static void test_every_internal_address_has_a_name(void)
{
    const uint8_t addrs[] = {
        T5_I2C_ADDR_ES8388, T5_I2C_ADDR_GT911, T5_I2C_ADDR_UNKNOWN28, T5_I2C_ADDR_RX8130,
        T5_I2C_ADDR_SC2356, T5_I2C_ADDR_ES7210, T5_I2C_ADDR_INA226, T5_I2C_ADDR_EXP0,
        T5_I2C_ADDR_EXP1, T5_I2C_ADDR_ST712X, T5_I2C_ADDR_BMI270,
    };
    for (size_t i = 0; i < sizeof addrs; i++) {
        TEST_ASSERT_NOT_NULL_MESSAGE(t5_bsp_i2c_name(addrs[i]), "address without a name");
    }
}

static void test_internal_addresses_are_unique(void)
{
    const uint8_t addrs[] = {
        T5_I2C_ADDR_ES8388, T5_I2C_ADDR_GT911, T5_I2C_ADDR_UNKNOWN28, T5_I2C_ADDR_RX8130,
        T5_I2C_ADDR_SC2356, T5_I2C_ADDR_ES7210, T5_I2C_ADDR_INA226, T5_I2C_ADDR_EXP0,
        T5_I2C_ADDR_EXP1, T5_I2C_ADDR_ST712X, T5_I2C_ADDR_BMI270,
    };
    for (size_t i = 0; i < sizeof addrs; i++) {
        for (size_t j = i + 1; j < sizeof addrs; j++) {
            TEST_ASSERT_NOT_EQUAL(addrs[i], addrs[j]);
        }
    }
}

static void test_unknown_address_has_no_name(void)
{
    TEST_ASSERT_NULL(t5_bsp_i2c_name(0x77));
}

static void check_xpin(uint16_t xpin, const char *want)
{
    const char *got = t5_bsp_xpin_name(T5_XPIN_ADDR(xpin), T5_XPIN_NUM(xpin));
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_STRING(want, got);
}

static void test_expander_names_match_pin_map(void)
{
    check_xpin(T5_XP_RF_ANT_SEL, "RF_ANT_SEL");
    check_xpin(T5_XP_SPK_EN, "SPK_EN");
    check_xpin(T5_XP_EXT5V_EN, "EXT5V_EN");
    check_xpin(T5_XP_LCD_RST, "LCD_RST");
    check_xpin(T5_XP_TP_RST, "TP_RST");
    check_xpin(T5_XP_CAM_RST, "CAM_RST");
    check_xpin(T5_XP_HP_DET, "HP_DET");
    check_xpin(T5_XP_WLAN_PWR_EN, "WLAN_PWR_EN");
    check_xpin(T5_XP_USB5V_EN, "USB5V_EN");
    check_xpin(T5_XP_PWROFF_PULSE, "PWROFF_PULSE?");
    check_xpin(T5_XP_NCHG_QC_EN, "nCHG_QC_EN");
    check_xpin(T5_XP_CHG_STAT, "CHG_STAT");
    check_xpin(T5_XP_CHG_EN, "CHG_EN");
}

static void test_expander_rejects_bad_input(void)
{
    TEST_ASSERT_NULL(t5_bsp_xpin_name(0x43, 8));
    TEST_ASSERT_NULL(t5_bsp_xpin_name(0x42, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_internal_address_has_a_name);
    RUN_TEST(test_internal_addresses_are_unique);
    RUN_TEST(test_unknown_address_has_no_name);
    RUN_TEST(test_expander_names_match_pin_map);
    RUN_TEST(test_expander_rejects_bad_input);
    return UNITY_END();
}
