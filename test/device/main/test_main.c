/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "t5_bsp/board.h"
#include "t5_core/event.h"

static bool up(const char *what, t5_err_t e)
{
    if (e != T5_OK) printf("bring-up: %s failed: %s\n", what, t5_err_str(e));
    return e == T5_OK;
}

/* After a reset the USB port re-enumerates, and output before the host has
 * reopened it is lost: start once the host says it's listening (any byte;
 * tools/t5/t5devtest.py sends one), or after 10 s on a bare board. */
static void wait_for_host(void)
{
    for (int i = 0; i < 1000 && getchar() == EOF; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

void app_main(void)
{
    /* The first steps of firmware/main/main.c, which every test needs; each
     * test starts its own service. A failure here fails the run (Unity can't
     * catch it outside a test). */
    bool ok = up("events", t5_event_init()) && up("board", t5_bsp_bringup());
    wait_for_host();
    printf("\n#### T5 DEVICE TESTS BEGIN\n");
    int failures = 1;
    if (ok) {
        UNITY_BEGIN();
        unity_run_all_tests();
        failures = UNITY_END();
    }
    printf("#### T5 DEVICE TESTS END failures=%d\n", failures);
}
