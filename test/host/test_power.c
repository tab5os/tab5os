/* SPDX-License-Identifier: Apache-2.0
 * Phase 2: arbiter, rails, wake locks, suspend hooks, state machine,
 * shutdown record and wake reason, soft off, autosave.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"
#include "t5_mocks.h"
#include "t5_bsp/xpin.h"
#include "t5_display/t5_display.h"
#include "t5_pins/pins.h"
#include "t5_power/arbiter.h"
#include "t5_power/autosave.h"
#include "t5_power/power.h"
#include "t5_power/rail.h"
#include "t5_time/rtc.h"
#include "t5_time/t5_time.h"
#include "priv.h"

static char s_dir[64];
static mock_i2c_dev_t *s_rtc;

static void rm_dir(void)
{
    char cmd[96];
    snprintf(cmd, sizeof cmd, "rm -rf %s", s_dir);
    (void)!system(cmd);
}

void setUp(void)
{
    mock_reset_all();
    mock_power_reset();
    t5_rail_reset_for_test();
    t5_wakelock_reset_for_test();
    t5_suspend_reset_for_test();
    t5_autosave_reset_for_test();
    t5_power_reset_for_test();
    t5_display_reset_for_test();
    t5_time_reset_for_test();
    strcpy(s_dir, "/tmp/t5power.XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(s_dir));
    t5_autosave_set_dir(s_dir);
}

void tearDown(void)
{
    rm_dir();
}

/* ---- arbiter -------------------------------------------------------------- */

static t5_arb_in_t arb(uint32_t idle)
{
    t5_arb_in_t in = { .idle_ms = idle };
    t5_power_profile_timeouts(T5_PROFILE_HANDHELD, &in.to);
    return in;
}

static void test_arbiter_idle_ladder(void)
{
    TEST_ASSERT_EQUAL(T5_PWR_ACTIVE, t5_power_arbitrate(&(t5_arb_in_t){ 0 }));   /* all timers "never" */
    t5_arb_in_t in = arb(0);
    TEST_ASSERT_EQUAL(T5_PWR_ACTIVE, t5_power_arbitrate(&in));
    in.idle_ms = 30000;
    TEST_ASSERT_EQUAL(T5_PWR_DIM, t5_power_arbitrate(&in));
    in.idle_ms = 120000;
    TEST_ASSERT_EQUAL(T5_PWR_SCREEN_OFF, t5_power_arbitrate(&in));
    in.idle_ms = 130000;
    TEST_ASSERT_EQUAL(T5_PWR_SLEEP, t5_power_arbitrate(&in));
    in.idle_ms = 130000 + 24u * 3600 * 1000;
    TEST_ASSERT_EQUAL(T5_PWR_SOFT_OFF, t5_power_arbitrate(&in));
}

static void test_arbiter_locks(void)
{
    t5_arb_in_t in = arb(200000);
    in.locks[T5_LOCK_SCREEN] = 1;
    TEST_ASSERT_EQUAL(T5_PWR_ACTIVE, t5_power_arbitrate(&in));
    in.locks[T5_LOCK_SCREEN] = 0;
    in.locks[T5_LOCK_CPU] = 1;
    TEST_ASSERT_EQUAL(T5_PWR_SCREEN_OFF, t5_power_arbitrate(&in));
    in.locks[T5_LOCK_CPU] = 0;
    in.locks[T5_LOCK_AUDIO] = 2;
    TEST_ASSERT_EQUAL(T5_PWR_SCREEN_OFF, t5_power_arbitrate(&in));
    in.locks[T5_LOCK_AUDIO] = 0;
    in.locks[T5_LOCK_NET] = 1;     /* net allows sleep... */
    TEST_ASSERT_EQUAL(T5_PWR_SLEEP, t5_power_arbitrate(&in));
    in.idle_ms = UINT32_MAX - 1;   /* ...but never soft off */
    TEST_ASSERT_EQUAL(T5_PWR_SLEEP, t5_power_arbitrate(&in));
    in.locks[T5_LOCK_NET] = 0;
    in.usb_power = true;           /* nor on USB */
    TEST_ASSERT_EQUAL(T5_PWR_SLEEP, t5_power_arbitrate(&in));
}

static void test_arbiter_mounted_never_soft_off(void)
{
    t5_arb_in_t in = { .idle_ms = UINT32_MAX - 1 };
    t5_power_profile_timeouts(T5_PROFILE_MOUNTED, &in.to);
    TEST_ASSERT_EQUAL(T5_PWR_SLEEP, t5_power_arbitrate(&in));
    in.idle_ms = 5 * 60 * 1000;
    TEST_ASSERT_EQUAL(T5_PWR_DIM, t5_power_arbitrate(&in));
}

static void test_arbiter_screen_off_before_dim(void)
{
    t5_arb_in_t in = { .idle_ms = 15000, .to = { .dim_ms = 60000, .screen_off_ms = 10000, .sleep_ms = 0 } };
    TEST_ASSERT_EQUAL(T5_PWR_SCREEN_OFF, t5_power_arbitrate(&in));   /* sleep never */
}

/* ---- rails ---------------------------------------------------------------- */

static void test_rail_refcount_and_owners(void)
{
    t5_rail_hold_t *a, *b;
    TEST_ASSERT_EQUAL(T5_OK, t5_rail_acquire(T5_RAIL_USB5V, "usbhost", 0, &a));
    TEST_ASSERT_TRUE(mock_power.rail[T5_RAIL_USB5V]);
    TEST_ASSERT_EQUAL(T5_OK, t5_rail_acquire(T5_RAIL_USB5V, "a-very-long-owner-name", 0, &b));
    TEST_ASSERT_EQUAL(2, t5_rail_count(T5_RAIL_USB5V));
    const char *own[4];
    TEST_ASSERT_EQUAL(2, t5_rail_owners(T5_RAIL_USB5V, own, 4));
    TEST_ASSERT_EQUAL_STRING("usbhost", own[0]);
    TEST_ASSERT_EQUAL(T5_RAIL_OWNER_LEN - 1, strlen(own[1]));
    int writes = mock_power.rail_writes;
    TEST_ASSERT_EQUAL(T5_OK, t5_rail_release(a));
    TEST_ASSERT_TRUE(mock_power.rail[T5_RAIL_USB5V]);
    TEST_ASSERT_EQUAL(writes, mock_power.rail_writes);   /* no glitch while one holder remains */
    TEST_ASSERT_EQUAL(T5_OK, t5_rail_release(b));
    TEST_ASSERT_FALSE(mock_power.rail[T5_RAIL_USB5V]);
    TEST_ASSERT_EQUAL(T5_ERR_INVALID_ARG, t5_rail_release(b));   /* double release */
}

static void test_rail_failure_counts_and_leaves_no_hold(void)
{
    mock_power.rail_fail = true;
    t5_rail_hold_t *h = (t5_rail_hold_t *)1;
    TEST_ASSERT_EQUAL(T5_ERR_IO, t5_rail_acquire(T5_RAIL_EXT5V, "x", 0, &h));
    TEST_ASSERT_NULL(h);
    TEST_ASSERT_EQUAL(0, t5_rail_count(T5_RAIL_EXT5V));
    TEST_ASSERT_EQUAL(1, t5_rail_errors(T5_RAIL_EXT5V));
}

static void test_rail_suspend_keeps_marked_rails(void)
{
    t5_rail_hold_t *u, *e, *w, *bl;
    t5_rail_acquire(T5_RAIL_USB5V, "kbd", 0, &u);
    t5_rail_acquire(T5_RAIL_EXT5V, "logger", T5_RAIL_KEEP_IN_SLEEP, &e);
    t5_rail_acquire(T5_RAIL_WLAN, "net", 0, &w);
    t5_rail_acquire(T5_RAIL_BACKLIGHT, "power", 0, &bl);
    TEST_ASSERT_EQUAL(T5_OK, t5_rail_suspend(false));
    TEST_ASSERT_FALSE(mock_power.rail[T5_RAIL_USB5V]);
    TEST_ASSERT_TRUE(mock_power.rail[T5_RAIL_EXT5V]);
    TEST_ASSERT_FALSE(mock_power.rail[T5_RAIL_WLAN]);
    TEST_ASSERT_TRUE(mock_power.rail[T5_RAIL_BACKLIGHT]);    /* follows the display, not Sleep */
    TEST_ASSERT_EQUAL(1, t5_rail_count(T5_RAIL_USB5V));      /* holds survive */
    TEST_ASSERT_EQUAL(T5_OK, t5_rail_resume());
    TEST_ASSERT_TRUE(mock_power.rail[T5_RAIL_USB5V]);
    TEST_ASSERT_TRUE(mock_power.rail[T5_RAIL_WLAN]);
    /* keep_all (a RAIL wake lock) leaves everything up. */
    TEST_ASSERT_EQUAL(T5_OK, t5_rail_suspend(true));
    TEST_ASSERT_TRUE(mock_power.rail[T5_RAIL_USB5V]);
    t5_rail_resume();
    /* A rail released while suspended stays off after resume. */
    t5_rail_suspend(false);
    t5_rail_release(u);
    t5_rail_resume();
    TEST_ASSERT_FALSE(mock_power.rail[T5_RAIL_USB5V]);
}

/* ---- wake locks ----------------------------------------------------------- */

static void test_wakelock_timeout_and_list(void)
{
    t5_wakelock_t *a = t5_wakelock_acquire(T5_LOCK_AUDIO, "podcast");
    t5_wakelock_t *b = t5_wakelock_acquire_timeout(T5_LOCK_CPU, "sync", 5000);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL(1, t5_wakelock_count(T5_LOCK_CPU));
    mock_time_advance_ms(4999);
    TEST_ASSERT_EQUAL(0, t5_wakelock_expire(t5_time_ms()));
    t5_wakelock_info_t info[4];
    TEST_ASSERT_EQUAL(2, t5_wakelock_list(info, 4));
    TEST_ASSERT_EQUAL_STRING("podcast", info[0].owner);
    TEST_ASSERT_EQUAL(1, info[1].left_ms);
    mock_time_advance_ms(1);
    TEST_ASSERT_EQUAL(1, t5_wakelock_expire(t5_time_ms()));
    TEST_ASSERT_EQUAL(0, t5_wakelock_count(T5_LOCK_CPU));
    t5_wakelock_release(a);
    TEST_ASSERT_EQUAL(0, t5_wakelock_count(T5_LOCK_AUDIO));
    t5_wakelock_release(NULL);   /* harmless */
}

static void test_wakelock_table_full(void)
{
    for (int i = 0; i < T5_WAKELOCK_MAX; i++) TEST_ASSERT_NOT_NULL(t5_wakelock_acquire(T5_LOCK_NET, "n"));
    TEST_ASSERT_NULL(t5_wakelock_acquire(T5_LOCK_NET, "one-too-many"));
}

/* ---- suspend hooks -------------------------------------------------------- */

static char s_order[64];

static t5_err_t hook_s(void *arg) { strcat(s_order, "s"); strcat(s_order, arg); return T5_OK; }
static t5_err_t hook_r(void *arg) { strcat(s_order, "r"); strcat(s_order, arg); return T5_OK; }
static t5_err_t hook_slow(void *arg) { (void)arg; mock_time_advance_ms(700); strcat(s_order, "S"); return T5_OK; }
static t5_err_t hook_fail(void *arg) { (void)arg; strcat(s_order, "F"); return T5_ERR_IO; }

static void test_suspend_order_budget_and_resume(void)
{
    s_order[0] = 0;
    t5_suspend_hook_t net = { "net", T5_SUSPEND_PRIO_NET, 100, hook_s, hook_r, "N" };
    t5_suspend_hook_t app = { "app", T5_SUSPEND_PRIO_APPS, 500, hook_slow, hook_r, "A" };
    t5_suspend_hook_t sto = { "storage", T5_SUSPEND_PRIO_STORAGE, 500, hook_fail, hook_r, "T" };
    TEST_ASSERT_EQUAL(T5_OK, t5_power_register_hook(&net));
    TEST_ASSERT_EQUAL(T5_OK, t5_power_register_hook(&app));
    TEST_ASSERT_EQUAL(T5_OK, t5_power_register_hook(&sto));
    TEST_ASSERT_EQUAL(T5_ERR_BUSY, t5_power_register_hook(&net));
    uint32_t ms = 0;
    TEST_ASSERT_EQUAL(1, t5_suspend_run(0, &ms));
    TEST_ASSERT_EQUAL_STRING("SFsN", s_order);
    TEST_ASSERT_EQUAL(700, ms);
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_POWER_SUSPEND_OVERRUN));
    TEST_ASSERT_EQUAL(0, t5_suspend_run(0, NULL));    /* already suspended: nothing runs twice */
    TEST_ASSERT_EQUAL_STRING("SFsN", s_order);
    s_order[0] = 0;
    TEST_ASSERT_EQUAL(0, t5_resume_run(0, NULL));
    TEST_ASSERT_EQUAL_STRING("rNrTrA", s_order);      /* reverse, failed suspend still resumed */
    t5_suspend_stats_t st[4];
    TEST_ASSERT_EQUAL(3, t5_power_hook_stats(st, 4));
    TEST_ASSERT_EQUAL_STRING("app", st[0].name);
    TEST_ASSERT_EQUAL(1, st[0].overruns);
    TEST_ASSERT_EQUAL(1, st[1].failures);
    /* Emergency: app hooks skipped. */
    s_order[0] = 0;
    t5_suspend_run(T5_SUSPEND_PRIO_SYSTEM, NULL);
    TEST_ASSERT_EQUAL_STRING("FsN", s_order);
    TEST_ASSERT_EQUAL(T5_OK, t5_power_unregister_hook("app"));
}

/* ---- shutdown record and wake reason -------------------------------------- */

static void test_offrec_roundtrip_and_corruption(void)
{
    t5_offrec_t r = { .reason = T5_OFF_BATTERY, .flags = T5_OFFREC_FLAG_ALARM }, o;
    uint8_t raw[4];
    t5_offrec_encode(&r, raw);
    TEST_ASSERT_TRUE(t5_offrec_decode(raw, &o));
    TEST_ASSERT_EQUAL(T5_OFF_BATTERY, o.reason);
    raw[2] ^= 0x10;
    TEST_ASSERT_FALSE(t5_offrec_decode(raw, &o));
    uint8_t zero[4] = {0};
    TEST_ASSERT_FALSE(t5_offrec_decode(zero, &o));
}

static void test_classify_wake(void)
{
    t5_wake_in_t in = { .reset = T5_RST_WDT, .rtc_ok = true };
    TEST_ASSERT_EQUAL(T5_WAKE_POWER_CYCLE, t5_power_classify_wake(&in));
    in.have_rec = true;
    TEST_ASSERT_EQUAL(T5_WAKE_BUTTON, t5_power_classify_wake(&in));
    in.rec.flags = T5_OFFREC_FLAG_ALARM;
    TEST_ASSERT_EQUAL(T5_WAKE_BUTTON, t5_power_classify_wake(&in));     /* alarm armed, button came first */
    in.rtc_flags = T5_RTC_FLAG_AF;
    TEST_ASSERT_EQUAL(T5_WAKE_RTC_ALARM, t5_power_classify_wake(&in));
    in.rtc_flags |= T5_RTC_FLAG_VLF;                                    /* RTC lost power: RAM is noise */
    TEST_ASSERT_EQUAL(T5_WAKE_COLD, t5_power_classify_wake(&in));
    in = (t5_wake_in_t){ .reset = T5_RST_POWERON, .rtc_ok = true };
    TEST_ASSERT_EQUAL(T5_WAKE_COLD, t5_power_classify_wake(&in));
    in.reset = T5_RST_PANIC;
    TEST_ASSERT_EQUAL(T5_WAKE_CRASH, t5_power_classify_wake(&in));
    in.reset = T5_RST_SW;
    TEST_ASSERT_EQUAL(T5_WAKE_REBOOT, t5_power_classify_wake(&in));
    in = (t5_wake_in_t){ .reset = T5_RST_DEEPSLEEP, .have_rec = true, .rec.flags = T5_OFFREC_FLAG_CHARGE };
    TEST_ASSERT_EQUAL(T5_WAKE_CHARGE_TIMER, t5_power_classify_wake(&in));
    in.reset = T5_RST_WDT;                                              /* button during charge mode */
    TEST_ASSERT_EQUAL(T5_WAKE_BUTTON, t5_power_classify_wake(&in));
}

/* ---- the service ---------------------------------------------------------- */

static void start_power(uint8_t rtc_flags)
{
    mock_expanders_start();
    s_rtc = mock_rtc_start(rtc_flags);
    t5_display_init();
    TEST_ASSERT_TRUE(t5_display_ready());
    TEST_ASSERT_EQUAL(T5_OK, t5_power_init());
}

static void advance(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 200) {
        mock_time_advance_ms(200);
        t5_power_tick();
    }
}

static void test_state_machine_walks_and_resumes(void)
{
    mock_i2c_add(0x55, true)->regs[0] = 3;   /* ST7123 for the display probe */
    start_power(0);
    TEST_ASSERT_TRUE(mock_power.rail[T5_RAIL_BACKLIGHT]);
    TEST_ASSERT_EQUAL(80, t5_display_effective_brightness());
    s_order[0] = 0;
    t5_suspend_hook_t h = { "app", T5_SUSPEND_PRIO_APPS, 500, hook_s, hook_r, "A" };
    t5_power_register_hook(&h);

    advance(30000);
    TEST_ASSERT_EQUAL(T5_PWR_DIM, t5_power_state());
    TEST_ASSERT_EQUAL(10, t5_display_effective_brightness());
    advance(90000);
    TEST_ASSERT_EQUAL(T5_PWR_SCREEN_OFF, t5_power_state());
    TEST_ASSERT_FALSE(mock_power.rail[T5_RAIL_BACKLIGHT]);
    TEST_ASSERT_EQUAL(0, mock_backlight);
    TEST_ASSERT_FALSE(mock_panel_asleep);       /* video keeps running for touch */
    advance(10000);
    TEST_ASSERT_EQUAL(T5_PWR_SLEEP, t5_power_state());
    TEST_ASSERT_TRUE(mock_power.cpu_low);
    TEST_ASSERT_EQUAL_STRING("sA", s_order);

    mock_time_advance_ms(1000);
    t5_power_user_activity();
    mock_time_advance_ms(40);
    t5_power_tick();
    TEST_ASSERT_EQUAL(T5_PWR_ACTIVE, t5_power_state());
    TEST_ASSERT_FALSE(mock_power.cpu_low);
    TEST_ASSERT_EQUAL_STRING("sArA", s_order);
    TEST_ASSERT_EQUAL(80, mock_backlight);
    t5_power_stats_t st;
    t5_power_stats(&st);
    TEST_ASSERT_EQUAL(1, st.sleeps);
    TEST_ASSERT_EQUAL(1, st.resumes);
    TEST_ASSERT_EQUAL(40, st.last_resume_ms);
    TEST_ASSERT_TRUE(mock_events_count(T5_EV_POWER_STATE) >= 4);
}

static void test_wake_from_sleep_keeps_screen_off(void)
{
    mock_i2c_add(0x55, true)->regs[0] = 3;
    start_power(0);
    t5_power_request_screen_off();
    advance(10200);
    TEST_ASSERT_EQUAL(T5_PWR_SLEEP, t5_power_state());
    t5_power_wake(T5_PWR_CAUSE_WAKE_NET, 3000);
    t5_power_tick();
    TEST_ASSERT_EQUAL(T5_PWR_SCREEN_OFF, t5_power_state());   /* CPU back, screen stays dark */
    TEST_ASSERT_FALSE(mock_power.cpu_low);
    advance(3200);
    TEST_ASSERT_EQUAL(T5_PWR_SLEEP, t5_power_state());
}

static void test_screen_lock_and_low_power(void)
{
    mock_i2c_add(0x55, true)->regs[0] = 3;
    start_power(0);
    t5_wakelock_t *l = t5_wakelock_acquire(T5_LOCK_SCREEN, "video");
    advance(200000);
    TEST_ASSERT_EQUAL(T5_PWR_ACTIVE, t5_power_state());
    t5_power_set_low_power(true);
    TEST_ASSERT_EQUAL(30, t5_display_effective_brightness());
    t5_wakelock_release(l);
    t5_power_user_activity();
    advance(15200);
    TEST_ASSERT_EQUAL(T5_PWR_DIM, t5_power_state());          /* low-power mode dims at 15 s */
    t5_power_set_low_power(false);
    t5_power_user_activity();
    t5_power_tick();
    TEST_ASSERT_EQUAL(80, t5_display_effective_brightness());
}

static void test_settings_persist(void)
{
    mock_i2c_add(0x55, true)->regs[0] = 3;
    start_power(0);
    t5_power_set_profile(T5_PROFILE_MOUNTED);
    t5_power_timeouts_t to = { 1000, 2000, 3000, 0 };
    t5_power_set_timeouts(&to);
    TEST_ASSERT_EQUAL(T5_OK, t5_autosave_now());
    t5_power_reset_for_test();
    t5_suspend_reset_for_test();
    t5_autosave_reset_for_test();
    t5_rail_reset_for_test();
    TEST_ASSERT_EQUAL(T5_OK, t5_power_init());
    TEST_ASSERT_EQUAL(T5_PROFILE_MOUNTED, t5_power_profile());
    t5_power_get_timeouts(&to);
    TEST_ASSERT_EQUAL(2000, to.screen_off_ms);
}

static void test_soft_off_records_and_pulses(void)
{
    mock_i2c_add(0x55, true)->regs[0] = 3;
    start_power(0);
    t5_time_set(1793880000, T5_TIME_SRC_USER);
    TEST_ASSERT_EQUAL(T5_OK, t5_power_schedule_wake(1793880000 + 3600, T5_WAKE_FLAG_BACKGROUND));
    mock_i2c_dev_t *exp1 = mock_i2c_get(0x44);
    exp1->wlog_n = 0;
    /* On the host nothing cuts power, so soft off comes back with FAIL and undoes. */
    TEST_ASSERT_EQUAL(T5_ERR_FAIL, t5_power_soft_off(T5_OFF_USER, false));
    int highs = 0;
    for (int i = 0; i < exp1->wlog_n; i++) {
        if ((exp1->wlog[i] >> 8) == 0x05 && (exp1->wlog[i] & 0x10)) highs++;   /* OUT register, P4 high */
    }
    TEST_ASSERT_EQUAL(5, highs);
    TEST_ASSERT_FALSE(t5_xpin_out_level(T5_XP_PWROFF_PULSE));
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_POWER_SHUTDOWN));
    /* Undo cleared the record and the alarm. */
    uint8_t raw[4];
    t5_rtc_ram_read(raw);
    t5_offrec_t r;
    TEST_ASSERT_FALSE(t5_offrec_decode(raw, &r));
    TEST_ASSERT_EQUAL_HEX8(0, s_rtc->regs[0x1E] & 0x08);   /* AIE off */
    TEST_ASSERT_FALSE(mock_panel_asleep);
}

static uint8_t s_written_rec[4];
static bool rtc_capture(mock_i2c_dev_t *d, uint32_t reg, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint32_t a = reg + i;
        if (a == 0x1D) d->regs[a] &= data[i];
        else d->regs[a] = data[i];
        if (a >= 0x20 && a < 0x24 && data[i]) memcpy(s_written_rec, &d->regs[0x20], 4);
    }
    return true;
}

static void test_soft_off_arms_alarm_and_next_boot_sees_it(void)
{
    mock_i2c_add(0x55, true)->regs[0] = 3;
    start_power(0);
    s_rtc->on_write = rtc_capture;
    memset(s_written_rec, 0, 4);
    t5_time_set(1793880000, T5_TIME_SRC_USER);
    t5_power_schedule_wake(1793880000 + 600, 0);
    t5_power_soft_off(T5_OFF_IDLE, false);
    t5_offrec_t r;
    TEST_ASSERT_TRUE(t5_offrec_decode(s_written_rec, &r));
    TEST_ASSERT_EQUAL(T5_OFF_IDLE, r.reason);
    TEST_ASSERT_EQUAL(T5_OFFREC_FLAG_ALARM, r.flags);

    /* Next boot: record in RAM, AF set, PMS150G power-up. */
    t5_power_reset_for_test();
    memcpy(&s_rtc->regs[0x20], s_written_rec, 4);
    s_rtc->regs[0x1D] = T5_RTC_FLAG_AF;
    t5_rtc_reset_for_test();
    t5_rtc_init();
    mock_power.reset = T5_RST_WDT;
    t5_power_read_wake_reason();
    TEST_ASSERT_EQUAL(T5_WAKE_RTC_ALARM, t5_power_wake_reason());
    TEST_ASSERT_TRUE(t5_power_should_restore_session());
    t5_off_reason_t why;
    TEST_ASSERT_TRUE(t5_power_last_off(&why));
    TEST_ASSERT_EQUAL(T5_OFF_IDLE, why);
    TEST_ASSERT_EQUAL_HEX8(0, s_rtc->regs[0x20]);   /* one boot only */
}

static bool s_handler_called;
static bool take_over(t5_off_reason_t r, void *arg) { (void)r; (void)arg; s_handler_called = true; return true; }

static void test_shutdown_handler_and_emergency(void)
{
    mock_i2c_add(0x55, true)->regs[0] = 3;
    start_power(0);
    t5_power_set_shutdown_handler(take_over, NULL);
    s_order[0] = 0;
    t5_suspend_hook_t h = { "app", T5_SUSPEND_PRIO_APPS, 500, hook_s, hook_r, "A" };
    t5_power_register_hook(&h);
    TEST_ASSERT_EQUAL(T5_OK, t5_power_soft_off(T5_OFF_USER, false));
    TEST_ASSERT_TRUE(s_handler_called);
    TEST_ASSERT_EQUAL_STRING("sArA", s_order);     /* apps saved before charge mode */
    s_handler_called = false;
    s_order[0] = 0;
    t5_power_soft_off(T5_OFF_BATTERY_CRITICAL, true);
    TEST_ASSERT_FALSE(s_handler_called);           /* emergency skips charge mode... */
    TEST_ASSERT_EQUAL_STRING("", s_order);         /* ...and app callbacks */
}

static void test_session_blob(void)
{
    const char blob[] = "launcher:settings/wifi";
    TEST_ASSERT_EQUAL(T5_OK, t5_session_set(blob, sizeof blob));
    TEST_ASSERT_EQUAL(T5_OK, t5_autosave_now());
    t5_autosave_reset_for_test();
    char buf[64];
    size_t len = 0;
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_session_get(buf, sizeof buf, &len));
    TEST_ASSERT_EQUAL(T5_OK, t5_session_load());
    TEST_ASSERT_EQUAL(T5_OK, t5_session_get(buf, sizeof buf, &len));
    TEST_ASSERT_EQUAL(sizeof blob, len);
    TEST_ASSERT_EQUAL_STRING(blob, buf);
    /* A torn file is rejected, not half-read. */
    char path[96];
    snprintf(path, sizeof path, "%s/session.bin", s_dir);
    FILE *f = fopen(path, "r+b");
    fseek(f, 20, SEEK_SET);
    fputc('X', f);
    fclose(f);
    TEST_ASSERT_EQUAL(T5_ERR_IO, t5_session_load());
}

static int s_saves;
static t5_err_t count_save(void *arg) { (void)arg; s_saves++; return T5_OK; }

static void test_autosave_period(void)
{
    s_saves = 0;
    t5_autosave_register("x", count_save, NULL);
    t5_autosave_poll(1000);
    t5_autosave_poll(30999);
    TEST_ASSERT_EQUAL(0, s_saves);
    mock_time_set_us(31000 * 1000LL);
    t5_autosave_poll(31000);
    TEST_ASSERT_EQUAL(1, s_saves);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_arbiter_idle_ladder);
    RUN_TEST(test_arbiter_locks);
    RUN_TEST(test_arbiter_mounted_never_soft_off);
    RUN_TEST(test_arbiter_screen_off_before_dim);
    RUN_TEST(test_rail_refcount_and_owners);
    RUN_TEST(test_rail_failure_counts_and_leaves_no_hold);
    RUN_TEST(test_rail_suspend_keeps_marked_rails);
    RUN_TEST(test_wakelock_timeout_and_list);
    RUN_TEST(test_wakelock_table_full);
    RUN_TEST(test_suspend_order_budget_and_resume);
    RUN_TEST(test_offrec_roundtrip_and_corruption);
    RUN_TEST(test_classify_wake);
    RUN_TEST(test_state_machine_walks_and_resumes);
    RUN_TEST(test_wake_from_sleep_keeps_screen_off);
    RUN_TEST(test_screen_lock_and_low_power);
    RUN_TEST(test_settings_persist);
    RUN_TEST(test_soft_off_records_and_pulses);
    RUN_TEST(test_soft_off_arms_alarm_and_next_boot_sees_it);
    RUN_TEST(test_shutdown_handler_and_emergency);
    RUN_TEST(test_session_blob);
    RUN_TEST(test_autosave_period);
    return UNITY_END();
}
