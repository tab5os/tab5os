/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "t5_bsp/xpin.h"
#include "t5_core/event.h"
#include "t5_i2c/t5_i2c.h"

void app_main(void)
{
    /* Bring up the shared pieces once; individual tests start their own service. */
    t5_event_init();
    T5_ASSERT(t5_i2c_init() == T5_OK);   /* outside a test: Unity can't catch a failure here */
    t5_xpin_init();
    t5_xpin_apply_boot_defaults();
    vTaskDelay(pdMS_TO_TICKS(200));
    printf("\n#### T5 DEVICE TESTS BEGIN\n");
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = UNITY_END();
    printf("#### T5 DEVICE TESTS END failures=%d\n", failures);
}
