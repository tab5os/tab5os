/* SPDX-License-Identifier: Apache-2.0
 * The board table (t5_pins/table.h): the I2C broker's reserved list, the
 * names the console shows people debugging hardware and the rail enable
 * pins all come from it.
 */
#include <string.h>
#include "unity.h"
#include "t5_pins/pins.h"
#include "t5_pins/table.h"
#include "t5_power/rail.h"

void setUp(void) {}
void tearDown(void) {}

static void test_chips_have_unique_addresses_and_names(void)
{
    for (size_t i = 0; i < t5_pins_chip_count; i++) {
        const t5_pins_chip_t *c = &t5_pins_chips[i];
        TEST_ASSERT_TRUE(c->addr >= 0x08 && c->addr <= 0x77);
        TEST_ASSERT_NOT_NULL(c->name);
        TEST_ASSERT_EQUAL_PTR(c, t5_pins_chip(c->addr));   /* first and only entry */
        TEST_ASSERT_EQUAL_STRING(c->name, t5_pins_i2c_name(c->addr));
    }
}

static void test_unknown_address_has_no_name(void)
{
    TEST_ASSERT_NULL(t5_pins_chip(0x77));
    TEST_ASSERT_NULL(t5_pins_i2c_name(0x77));
}

static void test_xpins_are_on_the_expanders_and_unique(void)
{
    for (size_t i = 0; i < t5_pins_xpin_count; i++) {
        uint16_t x = t5_pins_xpins[i].xpin;
        uint8_t a = T5_XPIN_ADDR(x), n = T5_XPIN_NUM(x);
        TEST_ASSERT_TRUE(a == T5_I2C_ADDR_EXP0 || a == T5_I2C_ADDR_EXP1);
        TEST_ASSERT_NOT_NULL(t5_pins_chip(a));
        TEST_ASSERT_EQUAL_STRING(t5_pins_xpins[i].name, t5_pins_xpin_name(a, n));
        for (size_t j = i + 1; j < t5_pins_xpin_count; j++) {
            TEST_ASSERT_NOT_EQUAL(x, t5_pins_xpins[j].xpin);
        }
    }
}

static void test_xpin_name_rejects_bad_input(void)
{
    TEST_ASSERT_NULL(t5_pins_xpin_name(T5_I2C_ADDR_EXP0, 8));
    TEST_ASSERT_NULL(t5_pins_xpin_name(T5_I2C_ADDR_EXP0, 3));   /* not connected */
    TEST_ASSERT_NULL(t5_pins_xpin_name(0x42, 0));
}

static const char *rail_pin(t5_rail_t r)
{
    uint16_t x = t5_pins_rail_en[r];
    return t5_pins_xpin_name(T5_XPIN_ADDR(x), T5_XPIN_NUM(x));
}

static void test_rail_enables_follow_the_rail_order(void)
{
    TEST_ASSERT_EQUAL(T5_RAIL_BACKLIGHT, T5_PINS_RAIL_COUNT);
    TEST_ASSERT_EQUAL_STRING("USB5V_EN", rail_pin(T5_RAIL_USB5V));
    TEST_ASSERT_EQUAL_STRING("EXT5V_EN", rail_pin(T5_RAIL_EXT5V));
    TEST_ASSERT_EQUAL_STRING("WLAN_PWR_EN", rail_pin(T5_RAIL_WLAN));
    TEST_ASSERT_EQUAL_STRING("SPK_EN", rail_pin(T5_RAIL_SPK));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_chips_have_unique_addresses_and_names);
    RUN_TEST(test_unknown_address_has_no_name);
    RUN_TEST(test_xpins_are_on_the_expanders_and_unique);
    RUN_TEST(test_xpin_name_rejects_bad_input);
    RUN_TEST(test_rail_enables_follow_the_rail_order);
    return UNITY_END();
}
