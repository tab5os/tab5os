/* SPDX-License-Identifier: Apache-2.0
 * t5ui's promise is that apps compile in nothing of LVGL. The host build has
 * no LVGL on its include path: if t5ui.h ever pulled in an LVGL header, this
 * file wouldn't compile. The checks below pin what apps do compile in.
 */
#include <string.h>
#include "unity.h"
#include "t5_gui/t5ui.h"
#include "t5_gui/t5ui_icons.h"

void setUp(void) {}
void tearDown(void) {}

#if defined(LV_VERSION_MAJOR) || defined(LVGL_VERSION_MAJOR) || defined(LV_SIZE_CONTENT)
#error "t5ui.h reaches LVGL"
#endif

static void test_encodings(void)
{
    TEST_ASSERT_EQUAL_HEX32(0x123456, T5UI_RGB(0x12, 0x34, 0x56));
    TEST_ASSERT_EQUAL_HEX32(0x40000032, (uint32_t)T5UI_PCT(50));
    TEST_ASSERT_EQUAL_HEX32(0x400003e8, (uint32_t)T5UI_PCT(1000));
    TEST_ASSERT_TRUE(T5UI_CONTENT > T5UI_PCT(1000));
    TEST_ASSERT_EQUAL(16, sizeof(t5ui_rect_t));
    TEST_ASSERT_EQUAL(3, strlen(T5UI_ICON_ARROW_BACK));        /* one UTF-8 glyph */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encodings);
    return UNITY_END();
}
