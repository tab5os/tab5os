/* SPDX-License-Identifier: Apache-2.0
 * Phase 2: INA226 driver, fuel gauge (against a synthetic pack until real
 * traces are recorded), discharge and charge policies, the battery service
 * and its charge history.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"
#include "t5_mocks.h"
#include "t5_battery/battery.h"
#include "t5_battery/gauge.h"
#include "t5_battery/history.h"
#include "t5_battery/ina226.h"
#include "t5_battery/pack.h"
#include "t5_battery/policy.h"
#include "t5_bsp/xpin.h"
#include "t5_pins/pins.h"
#include "t5_power/autosave.h"
#include "t5_storage/paths.h"
#include "t5_power/power.h"

static char s_dir[64];

void setUp(void)
{
    mock_reset_all();
    mock_power_reset();
    t5_battery_reset_for_test();
    t5_autosave_reset_for_test();
    t5_power_reset_for_test();
    strcpy(s_dir, "/tmp/t5bat.XXXXXX");
    TEST_ASSERT_NOT_NULL(mkdtemp(s_dir));
    t5_sys_dir_set(s_dir);
}

void tearDown(void)
{
    char cmd[96];
    snprintf(cmd, sizeof cmd, "rm -rf %s", s_dir);
    (void)!system(cmd);
}

/* ---- INA226 ---------------------------------------------------------------- */

static void test_ina226_conversions(void)
{
    TEST_ASSERT_EQUAL(8386, t5_ina226_bus_to_mv(0x1A35));     /* 6709 x 1.25 = 8386.25 */
    TEST_ASSERT_EQUAL(4370, t5_ina226_bus_to_mv(0x0DA8));
    TEST_ASSERT_EQUAL(5, t5_ina226_shunt_to_ma10(1));          /* 2.5 uV / 5 mOhm = 0.5 mA */
    TEST_ASSERT_EQUAL(-5000, t5_ina226_shunt_to_ma10(-1000));  /* -500 mA: charging */
    TEST_ASSERT_EQUAL(163835, t5_ina226_shunt_to_ma10(32767));
}

static void test_ina226_init_and_read(void)
{
    mock_i2c_dev_t *d = mock_ina226_add();
    mock_ina226_set(d, 7600, 3500);
    TEST_ASSERT_EQUAL(T5_OK, t5_ina226_init());
    TEST_ASSERT_TRUE(t5_ina226_ready());
    t5_ina226_sample_t s;
    TEST_ASSERT_EQUAL(T5_OK, t5_ina226_read(&s));
    TEST_ASSERT_EQUAL(7600, s.mv);
    TEST_ASSERT_EQUAL(3500, s.ma10);
    TEST_ASSERT_EQUAL(2660, s.mw);
}

static void test_ina226_wrong_chip(void)
{
    mock_i2c_add(T5_I2C_ADDR_INA226, false);   /* answers, but IDs read 0 */
    TEST_ASSERT_EQUAL(T5_ERR_NOT_FOUND, t5_ina226_init());
    TEST_ASSERT_FALSE(t5_ina226_ready());
}

/* ---- gauge ----------------------------------------------------------------- */

static void test_ocv_curve_spans_the_pack(void)
{
    int32_t prev = -1;
    for (uint16_t mv = T5_PACK_MIN_MV; mv <= T5_PACK_MAX_MV; mv += 10) {
        int32_t p = t5_gauge_ocv_to_permille(mv);
        TEST_ASSERT_TRUE(p >= prev);
        prev = p;
    }
    TEST_ASSERT_EQUAL(0, t5_gauge_ocv_to_permille(T5_PACK_EMPTY_MV));
    TEST_ASSERT_EQUAL(1000, t5_gauge_ocv_to_permille(T5_PACK_FULL_MV));
    TEST_ASSERT_TRUE(t5_gauge_ocv_to_permille(T5_PACK_EMPTY_MV + 100) > 0);
    TEST_ASSERT_TRUE(t5_gauge_ocv_to_permille(T5_PACK_FULL_MV - 100) < 1000);
}

static void test_plausibility_and_compensation(void)
{
    TEST_ASSERT_FALSE(t5_gauge_sample_plausible(4370));    /* the USB anomaly from bench 0.4 */
    TEST_ASSERT_TRUE(t5_gauge_sample_plausible(8386));
    /* Load sags the terminal voltage, charging lifts it: I x R either way. */
    uint16_t loaded = t5_gauge_compensate(7600, 4000), charging = t5_gauge_compensate(7600, -2000);
    TEST_ASSERT_EQUAL(7600 + 400 * T5_PACK_RINT_MOHM / 1000, loaded);
    TEST_ASSERT_EQUAL(7600 - 200 * T5_PACK_RINT_MOHM / 1000, charging);
}

/* A synthetic pack, independent of the gauge's curve: two cells following a
 * generic Li-ion (NMC) rested OCV against state of charge, sagging under load
 * by I x 140 mOhm (not the gauge's 150). The gauge has to cope with a curve
 * and a resistance that aren't its own. */
typedef struct {
    int64_t true_uah;
    int32_t cap_mah;
} pack_t;

static uint16_t cell_ocv_mv(int32_t permille)
{
    static const uint16_t mv[11] = { 3270, 3610, 3690, 3740, 3770, 3810, 3860, 3920, 3990, 4080, 4200 };
    if (permille <= 0) return mv[0];
    if (permille >= 1000) return mv[10];
    int i = permille / 100, f = permille % 100;
    return (uint16_t)(mv[i] + (mv[i + 1] - mv[i]) * f / 100);
}

static uint16_t pack_mv(const pack_t *p, int32_t ma10)
{
    int32_t pm = (int32_t)(p->true_uah / p->cap_mah);
    return (uint16_t)(2 * cell_ocv_mv(pm) - ma10 * 140 / 10000);
}

static void test_gauge_tracks_a_full_discharge(void)
{
    pack_t p = { .true_uah = 2200LL * 1000, .cap_mah = 2200 };   /* bigger than the 2000 nominal */
    t5_gauge_t g;
    t5_gauge_boot(&g, NULL, -1, pack_mv(&p, 0), 0);
    TEST_ASSERT_TRUE(g.valid);
    TEST_ASSERT_INT_WITHIN(2, 100, t5_gauge_percent(&g));
    /* USB at full: anchor. */
    t5_gauge_update(&g, 8380, 0, 1000, true);
    TEST_ASSERT_TRUE(g.full_anchor);
    /* Discharge at 350 mA, 1 Hz, until the pack hits the floor. */
    int worst = 0;
    int samples = 0;
    while (p.true_uah > 0) {
        int32_t ma10 = 3500;
        p.true_uah -= (int64_t)ma10 * 1000 / 36000;
        uint16_t mv = pack_mv(&p, ma10);
        t5_gauge_update(&g, mv, ma10, 1000, false);
        int truth = (int)(p.true_uah * 100 / ((int64_t)p.cap_mah * 1000));
        int err = abs(t5_gauge_percent(&g) - truth);
        if (truth > 10 && err > worst) worst = err;
        samples++;
        if (mv <= T5_PACK_EMPTY_MV + 50) break;
    }
    /* Counting against the wrong (nominal) capacity drifts; the empty anchor
     * then learns the real one. */
    TEST_ASSERT_TRUE(samples > 20000);
    TEST_ASSERT_EQUAL(1, g.cycles);
    TEST_ASSERT_INT_WITHIN(150, 2000 * 7 / 10 + 2200 * 3 / 10, g.capacity_mah);
    TEST_ASSERT_TRUE(t5_gauge_percent(&g) <= 3);
    printf("full discharge: %d samples, worst error above 10%%: %d points, learned %ld mAh\n", samples, worst,
           (long)g.capacity_mah);
}

static void test_gauge_second_discharge_within_5pct(void)
{
    /* With the learned capacity, the plan's exit criterion: under 5% error. */
    pack_t p = { .true_uah = 2200LL * 1000, .cap_mah = 2200 };
    t5_gauge_saved_t saved = { .magic = T5_GAUGE_SAVED_MAGIC, .charge_uah = 2200 * 1000, .capacity_mah = 2200 };
    t5_gauge_t g;
    t5_gauge_boot(&g, &saved, 0, pack_mv(&p, 3500), 3500);
    int worst = 0;
    for (int i = 0; i < 6 * 3600 && p.true_uah > 200 * 1000; i++) {
        int32_t ma10 = (i / 600) % 2 ? 2500 : 5000;   /* load steps every 10 min */
        p.true_uah -= (int64_t)ma10 * 1000 / 36000;
        t5_gauge_update(&g, pack_mv(&p, ma10), ma10, 1000, false);
        int truth = (int)(p.true_uah * 100 / ((int64_t)p.cap_mah * 1000));
        int err = abs(t5_gauge_percent(&g) - truth);
        if (err > worst) worst = err;
    }
    TEST_ASSERT_TRUE(worst < 5);
}

static void test_gauge_soft_off_gap_and_swap(void)
{
    t5_gauge_saved_t saved = { .magic = T5_GAUGE_SAVED_MAGIC, .charge_uah = 1000 * 1000, .capacity_mah = 2000,
                               .standby_ma10 = 20 };
    t5_gauge_t g;
    /* 10 days off at 2 mA = 480 mAh: 50% -> 26%. Voltage agrees roughly (rested OCV for 26%). */
    uint16_t mv = 7520;
    t5_gauge_boot(&g, &saved, 10 * 86400, mv, 0);
    TEST_ASSERT_FALSE(g.swapped);
    TEST_ASSERT_INT_WITHIN(4, 27, t5_gauge_percent(&g));
    /* A fresh pack after the same record: the voltage wins. */
    t5_gauge_boot(&g, &saved, 3600, 8300, 0);
    TEST_ASSERT_TRUE(g.swapped);
    TEST_ASSERT_INT_WITHIN(1, (t5_gauge_ocv_to_permille(8300) + 5) / 10, t5_gauge_percent(&g));
    /* Unknown elapsed time: no gap applied. */
    t5_gauge_boot(&g, &saved, -1, 7680, 0);
    TEST_ASSERT_INT_WITHIN(1, 50, t5_gauge_percent(&g));
}

static void test_gauge_rest_correction(void)
{
    t5_gauge_saved_t saved = { .magic = T5_GAUGE_SAVED_MAGIC, .charge_uah = 1400 * 1000, .capacity_mah = 2000 };
    t5_gauge_t g;
    t5_gauge_boot(&g, &saved, -1, 0, 0);   /* implausible first sample: trust the record */
    TEST_ASSERT_EQUAL(70, t5_gauge_percent(&g));
    for (int i = 0; i < 299; i++) t5_gauge_update(&g, 7680, 50, 1000, false);   /* 5 mA: at rest */
    TEST_ASSERT_INT_WITHIN(1, 70, t5_gauge_percent(&g));
    t5_gauge_update(&g, 7680, 50, 1000, false);
    int by_voltage = t5_gauge_ocv_to_permille(7680) / 10;
    TEST_ASSERT_INT_WITHIN(2, (70 * 3 + by_voltage * 7) / 10, t5_gauge_percent(&g));   /* mostly the voltage */
}

static void test_gauge_ignores_implausible(void)
{
    t5_gauge_t g;
    t5_gauge_boot(&g, NULL, -1, 8380, 0);
    int before = t5_gauge_percent(&g);
    t5_gauge_update(&g, 4370, 0, 1000, true);
    TEST_ASSERT_EQUAL(before, t5_gauge_percent(&g));
    TEST_ASSERT_EQUAL(1, g.invalid_samples);
}

static void test_minutes_left(void)
{
    t5_gauge_t g;
    t5_gauge_boot(&g, NULL, -1, 7680, 0);
    for (int i = 0; i < 600; i++) t5_gauge_update(&g, 7600, 5000, 1000, false);
    int32_t m = t5_gauge_minutes_left(&g);
    int32_t left_mah = (int32_t)(g.charge_uah / 1000);
    TEST_ASSERT_INT_WITHIN(10, left_mah * 60 / 500, m);   /* at 500 mA */
    t5_gauge_update(&g, 8000, -5000, 60000, true);
    TEST_ASSERT_EQUAL(-1, t5_gauge_minutes_left(&g));
}

/* ---- policies -------------------------------------------------------------- */

static void test_discharge_policy(void)
{
    t5_discharge_t st = {0};
    TEST_ASSERT_EQUAL(0, t5_discharge_step(&st, 50, 7680, 3000, false));
    TEST_ASSERT_EQUAL(T5_DIS_ACT_WARN, t5_discharge_step(&st, 15, 7420, 3000, false));
    TEST_ASSERT_EQUAL(0, t5_discharge_step(&st, 14, 7420, 3000, false));      /* once */
    TEST_ASSERT_EQUAL(T5_DIS_ACT_LOW_POWER_ON, t5_discharge_step(&st, 10, 7380, 3000, false));
    for (int i = 0; i < T5_DIS_OFF_SAMPLES - 1; i++) TEST_ASSERT_EQUAL(0, t5_discharge_step(&st, 5, 7220, 3000, false));
    TEST_ASSERT_EQUAL(T5_DIS_ACT_SOFT_OFF, t5_discharge_step(&st, 5, 7220, 3000, false));
    /* Plugging in clears everything. */
    TEST_ASSERT_EQUAL(T5_DIS_ACT_LOW_POWER_OFF, t5_discharge_step(&st, 5, 7900, -8000, true));
    TEST_ASSERT_FALSE(st.warned);
}

static void test_discharge_voltage_floors(void)
{
    t5_discharge_t st = {0};
    /* 6.5 V under heavy load is a sag, not empty. */
    for (int i = 0; i < 10; i++) TEST_ASSERT_EQUAL(0, t5_discharge_step(&st, 30, 6450, 9000, false));
    /* Under light load it is. */
    uint32_t a = 0;
    for (int i = 0; i < T5_DIS_OFF_SAMPLES; i++) a = t5_discharge_step(&st, 30, 6450, 3000, false);
    TEST_ASSERT_EQUAL(T5_DIS_ACT_SOFT_OFF, a);
    /* 6.3 V at any load: emergency after 3 samples, a single dip doesn't count. */
    st = (t5_discharge_t){0};
    TEST_ASSERT_EQUAL(0, t5_discharge_step(&st, 40, 6250, 12000, false));
    TEST_ASSERT_EQUAL(0, t5_discharge_step(&st, 40, 7000, 12000, false));
    t5_discharge_step(&st, 40, 6250, 12000, false);
    t5_discharge_step(&st, 40, 6250, 12000, false);
    TEST_ASSERT_EQUAL(T5_DIS_ACT_EMERGENCY_OFF, t5_discharge_step(&st, 40, 6250, 12000, false) & T5_DIS_ACT_EMERGENCY_OFF);
    /* An implausible reading (0) never triggers a floor. */
    st = (t5_discharge_t){0};
    for (int i = 0; i < 10; i++) TEST_ASSERT_EQUAL(0, t5_discharge_step(&st, 40, 0, 3000, false));
}

static void test_charge_policy(void)
{
    t5_charge_in_t in = { .usb = false, .percent = 50, .mv = 7700, .ma10 = 3000, .temp_c10 = 300 };
    t5_charge_out_t o;
    t5_charge_step(&in, T5_CHG_NO_USB, &o);
    TEST_ASSERT_EQUAL(T5_CHG_NO_USB, o.state);
    TEST_ASSERT_TRUE(o.chg_en);
    in.usb = true;
    in.ma10 = -9000;
    t5_charge_step(&in, o.state, &o);
    TEST_ASSERT_EQUAL(T5_CHG_CHARGING, o.state);
    TEST_ASSERT_FALSE(o.fast);
    in.fast_enabled = true;
    t5_charge_step(&in, o.state, &o);
    TEST_ASSERT_TRUE(o.fast);
    /* Temperature pause with hysteresis. */
    in.temp_c10 = T5_CHG_TEMP_PAUSE_C10;
    t5_charge_step(&in, o.state, &o);
    TEST_ASSERT_EQUAL(T5_CHG_PAUSED_TEMP, o.state);
    TEST_ASSERT_FALSE(o.chg_en);
    in.temp_c10 = T5_CHG_TEMP_RESUME_C10 + 1;
    t5_charge_step(&in, o.state, &o);
    TEST_ASSERT_EQUAL(T5_CHG_PAUSED_TEMP, o.state);
    in.temp_c10 = T5_CHG_TEMP_RESUME_C10;
    t5_charge_step(&in, o.state, &o);
    TEST_ASSERT_EQUAL(T5_CHG_CHARGING, o.state);
    /* 80% limit, restart at 75%. */
    in.limit_enabled = true;
    in.percent = 80;
    t5_charge_step(&in, o.state, &o);
    TEST_ASSERT_EQUAL(T5_CHG_PAUSED_LIMIT, o.state);
    in.percent = 76;
    t5_charge_step(&in, o.state, &o);
    TEST_ASSERT_EQUAL(T5_CHG_PAUSED_LIMIT, o.state);
    in.percent = 75;
    t5_charge_step(&in, o.state, &o);
    TEST_ASSERT_EQUAL(T5_CHG_CHARGING, o.state);
    /* Full: tapered at full voltage. */
    in.limit_enabled = false;
    in.mv = 8390;
    in.ma10 = -300;
    t5_charge_step(&in, o.state, &o);
    TEST_ASSERT_EQUAL(T5_CHG_FULL, o.state);
    TEST_ASSERT_TRUE(o.chg_en);
    /* Unknown temperature never pauses. */
    in.temp_c10 = T5_CHG_TEMP_UNKNOWN;
    t5_charge_step(&in, T5_CHG_PAUSED_TEMP, &o);
    TEST_ASSERT_TRUE(o.chg_en);
}

static void test_usb_guess(void)
{
    TEST_ASSERT_TRUE(t5_usb_power_guess(0, 8380, false));       /* full pack on USB: no current */
    TEST_ASSERT_TRUE(t5_usb_power_guess(-8000, 7800, false));   /* charging */
    TEST_ASSERT_TRUE(t5_usb_power_guess(3000, 4370, false));    /* bus anomaly */
    TEST_ASSERT_TRUE(t5_usb_power_guess(3000, 7800, true));     /* host on USB Serial/JTAG */
    TEST_ASSERT_FALSE(t5_usb_power_guess(3000, 7800, false));   /* running on the pack */
}

/* ---- the service ------------------------------------------------------------ */

static mock_i2c_dev_t *start_battery(uint16_t mv, int32_t ma10)
{
    mock_expanders_start();
    mock_rtc_start(0);
    mock_i2c_dev_t *ina = mock_ina226_add();
    mock_ina226_set(ina, mv, ma10);
    TEST_ASSERT_EQUAL(T5_OK, t5_battery_init());
    return ina;
}

/* The INA226 initialises, then stops answering. */
static mock_i2c_read_hook_t s_ina_read;
static bool ina_read_then_fail(mock_i2c_dev_t *d, uint32_t reg, uint8_t *out, size_t len)
{
    if (t5_ina226_ready()) {
        d->fail_count = 1000;
        d->fail_err = T5_ERR_IO;
    }
    return s_ina_read(d, reg, out, len);
}

static void test_service_init_needs_a_first_sample(void)
{
    /* Without a sample the gauge would boot at 0 mV, read as empty, and
     * power the Tab5 off: init has to fail instead (coding standard rule 7). */
    mock_expanders_start();
    mock_rtc_start(0);
    mock_i2c_dev_t *ina = mock_ina226_add();
    mock_ina226_set(ina, 7680, 3500);
    s_ina_read = ina->on_read;
    ina->on_read = ina_read_then_fail;
    TEST_ASSERT_NOT_EQUAL(T5_OK, t5_battery_init());
    t5_battery_status_t b;
    t5_battery_status(&b);
    TEST_ASSERT_FALSE(b.ready);
    TEST_ASSERT_EQUAL(0, mock_power.halted);   /* and nothing powered off */
}

static void test_service_drives_charger_and_events(void)
{
    mock_i2c_dev_t *ina = start_battery(7680, 3500);
    t5_battery_status_t b;
    t5_battery_status(&b);
    TEST_ASSERT_TRUE(b.ready);
    TEST_ASSERT_FALSE(b.usb);
    TEST_ASSERT_INT_WITHIN(1, (t5_gauge_ocv_to_permille(t5_gauge_compensate(7680, 3500)) + 5) / 10, b.percent);
    TEST_ASSERT_TRUE(t5_xpin_out_level(T5_XP_CHG_EN));
    TEST_ASSERT_TRUE(t5_xpin_out_level(T5_XP_NCHG_QC_EN));   /* fast off by default */
    /* Plug in: charging current. */
    mock_ina226_set(ina, 7900, -8000);
    mock_time_advance_ms(1000);
    t5_battery_step();
    t5_battery_status(&b);
    TEST_ASSERT_TRUE(b.usb);
    TEST_ASSERT_EQUAL(T5_CHG_CHARGING, b.charge);
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_USB_POWER));
    t5_battery_set_fast_charge(true);
    mock_time_advance_ms(1000);
    t5_battery_step();
    TEST_ASSERT_FALSE(t5_xpin_out_level(T5_XP_NCHG_QC_EN));
    /* Saved state round-trips through autosave. */
    TEST_ASSERT_EQUAL(T5_OK, t5_autosave_now());
    t5_battery_reset_for_test();
    t5_autosave_reset_for_test();
    TEST_ASSERT_EQUAL(T5_OK, t5_battery_init());
    TEST_ASSERT_TRUE(t5_battery_fast_charge());
}

static void test_service_low_battery_soft_off(void)
{
    mock_i2c_dev_t *ina = start_battery(7300, 3000);   /* 7345 mV open circuit: 10% */
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_BATTERY_LOW));
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_BATTERY_LOW_POWER));
    TEST_ASSERT_TRUE(t5_power_low_power());
    mock_ina226_set(ina, 7370, 3000);
    mock_time_advance_ms(1000);
    t5_battery_step();
    /* The soft off request runs (and fails on the host: nothing cuts power). */
    mock_ina226_set(ina, 6250, 3000);
    for (int i = 0; i < T5_DIS_EMERGENCY_SAMPLES; i++) {
        mock_time_advance_ms(1000);
        t5_battery_step();
    }
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_BATTERY_CRITICAL));
    TEST_ASSERT_EQUAL(1, mock_events_count(T5_EV_POWER_SHUTDOWN));
}

static int s_screen_pct = -100;
static void record_screen(int pct, void *arg) { (void)arg; s_screen_pct = pct; }

static void test_shutdown_on_usb_enters_charge_mode(void)
{
    s_screen_pct = -100;
    t5_battery_set_charge_screen(record_screen, NULL);
    start_battery(7900, -8000);                         /* charging */
    TEST_ASSERT_EQUAL(T5_OK, t5_power_soft_off(T5_OFF_USER, false));
    t5_battery_status_t b;
    t5_battery_status(&b);
    TEST_ASSERT_EQUAL(b.percent, s_screen_pct);         /* the level, shown once */
    TEST_ASSERT_EQUAL(1, mock_power.halted);            /* then deep sleep */
    TEST_ASSERT_TRUE(t5_xpin_out_level(T5_XP_CHG_EN));
}

static void test_shutdown_on_battery_is_soft_off(void)
{
    s_screen_pct = -100;
    t5_battery_set_charge_screen(record_screen, NULL);
    start_battery(7600, 3500);
    TEST_ASSERT_EQUAL(T5_ERR_FAIL, t5_power_soft_off(T5_OFF_USER, false));   /* the mock never cuts power */
    TEST_ASSERT_EQUAL(-100, s_screen_pct);
    TEST_ASSERT_EQUAL(0, mock_power.halted);
}

static void test_history_ring(void)
{
    t5_bat_hist_t h;
    t5_bat_hist_init(&h);
    TEST_ASSERT_TRUE(t5_bat_hist_valid(&h));
    for (int i = 0; i < T5_BAT_HIST_LEN + 10; i++) {
        t5_bat_hist_pt_t p = { .t = (uint32_t)i, .pct = (int8_t)(i % 100) };
        t5_bat_hist_add(&h, &p);
    }
    t5_bat_hist_pt_t out[T5_BAT_HIST_LEN];
    int n = t5_bat_hist_read(&h, out, T5_BAT_HIST_LEN);
    TEST_ASSERT_EQUAL(T5_BAT_HIST_LEN, n);
    TEST_ASSERT_EQUAL(10, out[0].t);                       /* oldest kept */
    TEST_ASSERT_EQUAL(T5_BAT_HIST_LEN + 9, out[n - 1].t);  /* newest last */
    t5_bat_hist_pt_t few[5];
    TEST_ASSERT_EQUAL(5, t5_bat_hist_read(&h, few, 5));
    TEST_ASSERT_EQUAL(T5_BAT_HIST_LEN + 5, few[0].t);      /* the newest five */
    h.head = 9999;
    TEST_ASSERT_FALSE(t5_bat_hist_valid(&h));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ina226_conversions);
    RUN_TEST(test_ina226_init_and_read);
    RUN_TEST(test_ina226_wrong_chip);
    RUN_TEST(test_ocv_curve_spans_the_pack);
    RUN_TEST(test_plausibility_and_compensation);
    RUN_TEST(test_gauge_tracks_a_full_discharge);
    RUN_TEST(test_gauge_second_discharge_within_5pct);
    RUN_TEST(test_gauge_soft_off_gap_and_swap);
    RUN_TEST(test_gauge_rest_correction);
    RUN_TEST(test_gauge_ignores_implausible);
    RUN_TEST(test_minutes_left);
    RUN_TEST(test_discharge_policy);
    RUN_TEST(test_discharge_voltage_floors);
    RUN_TEST(test_charge_policy);
    RUN_TEST(test_usb_guess);
    RUN_TEST(test_service_init_needs_a_first_sample);
    RUN_TEST(test_service_drives_charger_and_events);
    RUN_TEST(test_service_low_battery_soft_off);
    RUN_TEST(test_shutdown_on_usb_enters_charge_mode);
    RUN_TEST(test_shutdown_on_battery_is_soft_off);
    RUN_TEST(test_history_ring);
    return UNITY_END();
}
