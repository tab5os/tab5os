/* SPDX-License-Identifier: Apache-2.0
 * Phase 2 and 3 services on real hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "t5_battery/gauge.h"
#include "t5_battery/ina226.h"
#include "t5_bsp/xpin.h"
#include "t5_core/event.h"
#include "t5_i2c/t5_i2c.h"
#include "t5_net/net.h"
#include "t5_pins/pins.h"
#include "t5_power/power.h"
#include "t5_power/rail.h"
#include "t5_time/rtc.h"
#include "t5_time/t5_time.h"

static void bus_up(void)
{
    t5_event_init();
    TEST_ASSERT_EQUAL(T5_OK, t5_i2c_init());
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_init());
}

TEST_CASE("ina226 reads a plausible pack or the USB node", "[battery]")
{
    bus_up();
    TEST_ASSERT_EQUAL(T5_OK, t5_ina226_init());
    vTaskDelay(pdMS_TO_TICKS(80));
    t5_ina226_sample_t s;
    int plausible = 0;
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL(T5_OK, t5_ina226_read(&s));
        /* The pack (6-8.5 V) or, on USB, the ~4.4 V node (bench 0.4). */
        TEST_ASSERT_TRUE(s.mv > 4000 && s.mv < 8600);
        TEST_ASSERT_TRUE(s.ma10 > -40000 && s.ma10 < 40000);   /* within +-4 A */
        plausible += t5_gauge_sample_plausible(s.mv);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    printf("ina226: last %u mV %ld.%ld mA, %d/10 plausible\n", s.mv, (long)(s.ma10 / 10), (long)labs(s.ma10 % 10),
           plausible);
}

TEST_CASE("rail holds switch the expander pin", "[power]")
{
    bus_up();
    t5_rail_hold_t *a, *b;
    TEST_ASSERT_EQUAL(T5_OK, t5_rail_acquire(T5_RAIL_USB5V, "test-a", 0, &a));
    TEST_ASSERT_EQUAL(T5_OK, t5_rail_acquire(T5_RAIL_USB5V, "test-b", 0, &b));
    bool lvl = false;
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_read(T5_XP_USB5V_EN, &lvl));
    TEST_ASSERT_TRUE(lvl);
    t5_rail_release(a);
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_read(T5_XP_USB5V_EN, &lvl));
    TEST_ASSERT_TRUE(lvl);
    t5_rail_release(b);
    TEST_ASSERT_EQUAL(T5_OK, t5_xpin_read(T5_XP_USB5V_EN, &lvl));
    TEST_ASSERT_FALSE(lvl);
}

TEST_CASE("rtc user ram survives a write and read", "[power]")
{
    bus_up();
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_init());
    uint8_t old[4], w[4] = { 0x12, 0x34, 0x56, 0x78 }, r[4];
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_ram_read(old));
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_ram_write(w));
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_ram_read(r));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(w, r, 4);
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_ram_write(old));
}

TEST_CASE("wake reason is read without error", "[power]")
{
    bus_up();
    TEST_ASSERT_EQUAL(T5_OK, t5_rtc_init());
    t5_power_read_wake_reason();
    t5_wake_reason_t w = t5_power_wake_reason();
    printf("wake reason: %s\n", t5_wake_reason_name(w));
    TEST_ASSERT_TRUE(w != T5_WAKE_CHARGE_TIMER);
}

static volatile bool s_up, s_scanned;
static void on_net(t5_event_id_t id, const void *d, size_t l, void *a)
{
    (void)d; (void)l; (void)a;
    if (id == T5_EV_NET_C6_UP) s_up = true;
    if (id == T5_EV_NET_SCAN_DONE) s_scanned = true;
}

TEST_CASE("c6 link comes up and scans", "[net]")
{
    bus_up();
    TEST_ASSERT_EQUAL(T5_OK, t5_time_init());
    s_up = s_scanned = false;
    t5_event_subscribe(T5_EV_ANY, on_net, NULL);
    TEST_ASSERT_EQUAL(T5_OK, t5_net_init());
    t5_net_enable(true);
    for (int i = 0; i < 100 && !s_up; i++) vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_TRUE_MESSAGE(s_up, "C6 link did not come up in 10 s");
    t5_net_status_t st;
    t5_net_status(&st);
    printf("C6 firmware %s, link up in %u ms\n", st.c6_version, (unsigned)st.link_up_ms);
    TEST_ASSERT_EQUAL(T5_OK, t5_net_scan());
    for (int i = 0; i < 100 && !s_scanned; i++) vTaskDelay(pdMS_TO_TICKS(100));
    t5_net_ap_t ap[T5_NET_MAX_SCAN];
    int n = t5_net_scan_results(ap, T5_NET_MAX_SCAN);
    printf("%d access points\n", n);
    TEST_ASSERT_TRUE(s_scanned);
    t5_event_unsubscribe(T5_EV_ANY, on_net, NULL);
}
