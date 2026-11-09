/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "t5_battery/battery.h"
#include "t5_battery/gauge.h"
#include "t5_battery/battery_test.h"
#include "t5_battery/ina226.h"
#include "t5_bsp/xpin.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_battery/history.h"
#include "t5_core/os.h"
#include "t5_imu/t5_imu.h"
#include "t5_pins/pins.h"
#include "t5_power/autosave.h"
#include "t5_power/power.h"
#include "t5_storage/paths.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/t5_time.h"
#include "battery_hw.h"

static const char *TAG = "t5.battery";

#define STATE_VERSION 1
#define SETTINGS_VERSION 1
#define SAVE_MIN_MS (5u * 60 * 1000)   /* gauge record: at most every 5 min unless the percent moved */
#define TEMP_EVERY 10                   /* samples between temperature reads */
#define TRACE_MAX_DATA (1u << 20)
#define CHARGE_SLEEP_MS 60000
#define HIST_FILE "bathist.bin"

typedef struct {
    uint8_t limit;
    uint8_t fast;
    uint8_t reserved[2];
} settings_t;

static t5_gauge_t s_g;
static t5_discharge_t s_dis;
static t5_battery_status_t s_st = { .percent = -1, .minutes_left = -1, .imu_c10 = T5_CHG_TEMP_UNKNOWN,
                                    .chip_c10 = T5_CHG_TEMP_UNKNOWN };
static settings_t s_set, s_set_saved;
static t5_mutex_t s_lock;
static uint32_t s_last_ms;
static uint32_t s_saved_ms;
static int s_saved_pct = -2;
static uint32_t s_temp_tick;
static bool s_applied;              /* CHG pins written at least once */
static FILE *s_trace;
static char s_trace_path[96];
static uint32_t s_trace_lines;
static bool s_inited;
static t5_bat_hist_t *s_hist;           /* 2.9 KB */
static int64_t s_hist_acc_ma10;
static uint32_t s_hist_acc_n, s_hist_last_ms;
static bool s_hist_dirty;
static t5_battery_charge_screen_fn_t s_screen_fn;
static void *s_screen_arg;

static void path(char *out, size_t cap, const char *name)
{
    T5_IGNORE(t5_sys_path(out, cap, name));
}

static t5_err_t save_state(void *arg)
{
    (void)arg;
    if (!s_g.valid) return T5_OK;
    uint32_t now = t5_time_ms();
    int pct = t5_gauge_percent(&s_g);
    t5_err_t err = T5_OK;
    if (pct != s_saved_pct || now - s_saved_ms >= SAVE_MIN_MS) {
        t5_gauge_saved_t rec;
        t5_gauge_save(&s_g, s_st.mv, t5_time_valid() ? (int64_t)t5_time_now() : 0, &rec);
        char p[96];
        path(p, sizeof p, "battery.bin");
        err = t5_storage_blob_write(p, STATE_VERSION, &rec, sizeof rec);
        if (err == T5_OK) {
            s_saved_ms = now;
            s_saved_pct = pct;
        }
    }
    if (memcmp(&s_set, &s_set_saved, sizeof s_set)) {
        char p[96];
        path(p, sizeof p, "charge.bin");
        t5_err_t e = t5_storage_blob_write(p, SETTINGS_VERSION, &s_set, sizeof s_set);
        if (e == T5_OK) s_set_saved = s_set;
        else if (err == T5_OK) err = e;
    }
    return err;
}

static bool load_state(t5_gauge_saved_t *rec)
{
    char p[96];
    size_t len = 0;
    path(p, sizeof p, "charge.bin");
    if (t5_storage_blob_read(p, SETTINGS_VERSION, &s_set, sizeof s_set, &len) != T5_OK || len != sizeof s_set) {
        memset(&s_set, 0, sizeof s_set);
    }
    s_set_saved = s_set;
    path(p, sizeof p, "battery.bin");
    return t5_storage_blob_read(p, STATE_VERSION, rec, sizeof *rec, &len) == T5_OK && len == sizeof *rec &&
           rec->magic == T5_GAUGE_SAVED_MAGIC;
}

t5_err_t t5_battery_trace_start(const char *p)
{
    t5_battery_trace_stop();
    if (p) {
        strncpy(s_trace_path, p, sizeof s_trace_path - 1);
    } else if (t5_storage_sd_state() == T5_SD_MOUNTED) {
        snprintf(s_trace_path, sizeof s_trace_path, "%s/battery-%lld.csv", T5_SD_PATH,
                 (long long)(t5_time_valid() ? t5_time_now() : t5_time_ms() / 1000));
    } else {
        snprintf(s_trace_path, sizeof s_trace_path, "%s/battery.csv", T5_DATA_PATH);
    }
    s_trace = fopen(s_trace_path, "a");
    if (!s_trace) {
        s_trace_path[0] = 0;
        return T5_ERR_IO;
    }
    fprintf(s_trace, "t_ms,utc,mv,ma,mw,pct,charge_mah,usb,chg_en,fast,chg_stat,state,imu_c,chip_c\n");
    s_trace_lines = 0;
    T5_LOGI(TAG, "trace to %s", s_trace_path);
    return T5_OK;
}

void t5_battery_trace_stop(void)
{
    if (s_trace) {
        fflush(s_trace);
        fsync(fileno(s_trace));
        fclose(s_trace);
        s_trace = NULL;
    }
    s_trace_path[0] = 0;
}

const char *t5_battery_trace_path(void)
{
    return s_trace ? s_trace_path : NULL;
}

static void trace_line(void)
{
    if (!s_trace) return;
    fprintf(s_trace, "%u,%lld,%u,%ld.%ld,%ld,%d,%ld,%d,%d,%d,%d,%d,%d.%d,%d.%d\n", (unsigned)t5_time_ms(),
            (long long)(t5_time_valid() ? t5_time_now() : 0), s_st.mv, (long)(s_st.ma10 / 10),
            (long)((s_st.ma10 < 0 ? -s_st.ma10 : s_st.ma10) % 10), (long)s_st.mw, s_st.percent,
            (long)s_st.charge_mah, s_st.usb, s_st.chg_en, s_st.fast, s_st.chg_stat, (int)s_st.charge,
            s_st.imu_c10 / 10, (s_st.imu_c10 < 0 ? -s_st.imu_c10 : s_st.imu_c10) % 10, s_st.chip_c10 / 10,
            (s_st.chip_c10 < 0 ? -s_st.chip_c10 : s_st.chip_c10) % 10);
    if (++s_trace_lines % 60 == 0) {
        fflush(s_trace);
        fsync(fileno(s_trace));
        /* /data is small: stop before the trace crowds out settings. */
        if (!strncmp(s_trace_path, T5_DATA_PATH, strlen(T5_DATA_PATH)) && ftell(s_trace) > (long)TRACE_MAX_DATA) {
            T5_LOGW(TAG, "trace on /data reached 1 MB, stopped");
            t5_battery_trace_stop();
        }
    }
}

static void read_temps(void)
{
    if (t5_imu_status() == T5_IMU_READY) {
        int32_t cc;
        if (t5_imu_read_temp(&cc) == T5_OK) s_st.imu_c10 = (int16_t)(cc / 10);
    }
    int16_t c10;
    if (t5_battery_hw_chip_temp(&c10) == T5_OK) s_st.chip_c10 = c10;
}

static void apply_charge(const t5_charge_out_t *o)
{
    if (s_applied && o->chg_en == s_st.chg_en && o->fast == s_st.fast) return;
    if (t5_xpin_output(T5_XP_CHG_EN, o->chg_en) == T5_OK && t5_xpin_output(T5_XP_NCHG_QC_EN, !o->fast) == T5_OK) {
        s_st.chg_en = o->chg_en;
        s_st.fast = o->fast;
        s_applied = true;
    }
}

static void post(t5_event_id_t id)
{
    t5_ev_battery_t ev = { .percent = (int16_t)s_st.percent, .mv = s_st.mv, .ma = (int16_t)(s_st.ma10 / 10),
                           .charge = (uint8_t)s_st.charge, .usb = s_st.usb };
    t5_event_post(id, &ev, sizeof ev);
}

void t5_battery_step(void)
{
    if (!t5_ina226_ready()) return;
    t5_ina226_sample_t smp;
    if (t5_ina226_read(&smp) != T5_OK) {
        s_st.read_errors++;
        return;
    }
    uint32_t now = t5_time_ms();
    uint32_t dt = s_last_ms ? now - s_last_ms : T5_BATTERY_PERIOD_MS;
    s_last_ms = now;
    bool plausible = t5_gauge_sample_plausible(smp.mv);
    bool usb = t5_usb_power_guess(smp.ma10, smp.mv, t5_battery_hw_usb_host());
    if (s_temp_tick++ % TEMP_EVERY == 0) read_temps();
    bool lvl = false;
    bool have_stat = t5_xpin_read(T5_XP_CHG_STAT, &lvl) == T5_OK;

    t5_mutex_lock(s_lock);
    int old_pct = s_st.percent;
    bool old_usb = s_st.usb;
    t5_charge_state_t old_charge = s_st.charge;
    t5_gauge_update(&s_g, smp.mv, smp.ma10, dt, usb);
    s_st.samples++;
    s_st.invalid_samples = s_g.invalid_samples;
    if (plausible) s_st.mv = smp.mv;
    s_st.ma10 = smp.ma10;
    s_st.mw = smp.mw;
    s_st.usb = usb;
    s_st.percent = t5_gauge_percent(&s_g);
    s_st.minutes_left = t5_gauge_minutes_left(&s_g);
    s_st.capacity_mah = s_g.capacity_mah;
    s_st.charge_mah = (int32_t)(s_g.charge_uah / 1000);
    s_st.cycles = s_g.cycles;
    if (have_stat) {
        s_st.chg_stat = lvl;
        bool charging = smp.ma10 < -T5_PACK_IDLE_MA10;
        bool discharging = smp.ma10 > T5_PACK_IDLE_MA10;
        if (lvl) { s_st.chg_stat_hi_charging += charging; s_st.chg_stat_hi_discharging += discharging; }
        else { s_st.chg_stat_lo_charging += charging; s_st.chg_stat_lo_discharging += discharging; }
    }

    int16_t t = s_st.imu_c10;   /* the IMU sits nearest the pack; the die runs hot */
    t5_charge_in_t ci = { .usb = usb, .percent = s_st.percent, .mv = smp.mv, .ma10 = smp.ma10, .temp_c10 = t,
                          .limit_enabled = s_set.limit, .fast_enabled = s_set.fast };
    t5_charge_out_t co;
    t5_charge_step(&ci, s_st.charge, &co);
    s_st.charge = co.state;
    apply_charge(&co);
    uint32_t act = t5_discharge_step(&s_dis, s_st.percent, plausible ? smp.mv : 0, smp.ma10, usb);
    s_st.low_power = s_dis.low_power;
    if (s_hist) {
        s_hist_acc_ma10 += smp.ma10;
        s_hist_acc_n++;
        if (!s_hist_last_ms) s_hist_last_ms = now;
        if (now - s_hist_last_ms >= T5_BAT_HIST_PERIOD_S * 1000u) {
            t5_bat_hist_pt_t p = {
                .t = t5_time_valid() ? (uint32_t)t5_time_now() : 0,
                .pct = (int8_t)s_st.percent,
                .flags = (uint8_t)((s_st.charge == T5_CHG_CHARGING ? T5_BAT_HIST_CHARGING : 0) | (usb ? T5_BAT_HIST_USB : 0)),
                .mv = s_st.mv,
                .ma = (int16_t)(s_hist_acc_ma10 / (int64_t)s_hist_acc_n / 10),
            };
            t5_bat_hist_add(s_hist, &p);
            s_hist_dirty = true;
            s_hist_acc_ma10 = 0;
            s_hist_acc_n = 0;
            s_hist_last_ms = now;
        }
    }
    t5_mutex_unlock(s_lock);

    t5_power_set_usb_power(usb);
    trace_line();
    if (usb != old_usb) post(T5_EV_USB_POWER);
    if (co.state != old_charge) post(T5_EV_CHARGE_STATE);
    if (s_st.percent != old_pct) post(T5_EV_BATTERY_UPDATE);
    if (act & T5_DIS_ACT_WARN) post(T5_EV_BATTERY_LOW);
    if (act & T5_DIS_ACT_LOW_POWER_ON) {
        post(T5_EV_BATTERY_LOW_POWER);
        t5_power_set_low_power(true);
    }
    if (act & T5_DIS_ACT_LOW_POWER_OFF) {
        post(T5_EV_BATTERY_LOW_POWER_OFF);
        t5_power_set_low_power(false);
    }
    if (act & (T5_DIS_ACT_SOFT_OFF | T5_DIS_ACT_EMERGENCY_OFF)) {
        bool emergency = act & T5_DIS_ACT_EMERGENCY_OFF;
        T5_LOGE(TAG, "%s at %u mV, %d%%: soft off", emergency ? "critical" : "empty", smp.mv, s_st.percent);
        post(T5_EV_BATTERY_CRITICAL);
        t5_battery_trace_stop();
        T5_LOG_ON_ERR(TAG, t5_power_soft_off(emergency ? T5_OFF_BATTERY_CRITICAL : T5_OFF_BATTERY, emergency));
    }
}

void t5_battery_status(t5_battery_status_t *out)
{
    if (s_lock) t5_mutex_lock(s_lock);
    *out = s_st;
    if (s_lock) t5_mutex_unlock(s_lock);
}

int t5_battery_percent(void)
{
    return s_st.percent;
}

void t5_battery_set_charge_limit(bool on)
{
    s_set.limit = on;
}

void t5_battery_set_fast_charge(bool on)
{
    s_set.fast = on;
}

bool t5_battery_charge_limit(void) { return s_set.limit; }
bool t5_battery_fast_charge(void) { return s_set.fast; }

void t5_battery_set_charge_screen(t5_battery_charge_screen_fn_t fn, void *arg)
{
    s_screen_arg = arg;
    s_screen_fn = fn;
}

static bool shutdown_handler(t5_off_reason_t reason, void *arg)
{
    (void)arg;
    if (reason == T5_OFF_CHARGE_UNPLUG || !s_st.usb) {
        return false;
    }
    T5_LOGI(TAG, "shutdown on USB: charge mode (%d%%)", s_st.percent);
    t5_charge_out_t co = { .state = T5_CHG_CHARGING, .chg_en = true, .fast = s_set.fast };
    apply_charge(&co);
    t5_battery_trace_stop();
    T5_LOG_ON_ERR(TAG, save_state(NULL));
    if (s_screen_fn) s_screen_fn(s_st.percent, s_screen_arg);
    t5_power_charge_sleep(CHARGE_SLEEP_MS);
    return true;   /* host only: the device is asleep */
}

/* The first sample after boot seeds the gauge and the charge-mode check, so
 * it gets a few tries (bounded: rule 2) before giving up on the chip. */
static t5_err_t read_boot_sample(t5_ina226_sample_t *smp)
{
    t5_err_t e = T5_ERR_FAIL;
    for (int i = 0; i < 3; i++) {
        e = t5_ina226_read(smp);
        if (e == T5_OK) break;
        t5_delay_ms(5);
    }
    return e;
}

/* Charge mode leaves the chip running when it boots on, so the boot that
 * follows doesn't configure it and wait for a first conversion again. */
static t5_err_t start_sensor(void)
{
    if (t5_ina226_ready()) return T5_OK;
    T5_RETURN_ON_ERR(t5_ina226_init());
    t5_delay_ms(T5_INA226_FIRST_SAMPLE_MS);
    return T5_OK;
}

void t5_battery_charge_mode_resume(void)
{
    if (start_sensor() != T5_OK) {
        T5_LOGE(TAG, "charge mode: INA226 missing, booting");
        t5_power_charge_mode_end();
        return;
    }
    t5_ina226_sample_t smp = {0};
    t5_err_t re = read_boot_sample(&smp);
    if (re != T5_OK) {
        /* A zero sample would read as "unplugged" and power off: boot instead. */
        T5_LOGE(TAG, "charge mode: INA226 read: %s, booting", t5_err_str(re));
        t5_power_charge_mode_end();
        return;
    }
    int16_t c10 = T5_CHG_TEMP_UNKNOWN;
    t5_battery_hw_chip_temp(&c10);
    bool usb = t5_usb_power_guess(smp.ma10, smp.mv, t5_battery_hw_usb_host());
    T5_LOGI(TAG, "charge mode check: %u mV, %ld.%ld mA, chip %d.%d C, usb %d", smp.mv, (long)(smp.ma10 / 10),
            (long)((smp.ma10 < 0 ? -smp.ma10 : smp.ma10) % 10), c10 / 10, (c10 < 0 ? -c10 : c10) % 10, usb);
    if (!usb) {
        t5_power_charge_mode_end();
        T5_LOG_ON_ERR(TAG, t5_power_soft_off(T5_OFF_CHARGE_UNPLUG, true));
        return;   /* power wasn't cut: boot normally */
    }
    /* Only the die temperature is available this early. Just out of deep
     * sleep the die is close to the board's temperature, so it gets the same
     * threshold the IMU gets when running. */
    load_state(&(t5_gauge_saved_t){0});
    t5_charge_in_t ci = { .usb = true, .percent = -1, .mv = smp.mv, .ma10 = smp.ma10,
                          .temp_c10 = c10,
                          .limit_enabled = false, .fast_enabled = s_set.fast };
    t5_charge_out_t co;
    t5_charge_step(&ci, T5_CHG_CHARGING, &co);
    apply_charge(&co);
    t5_power_charge_sleep(CHARGE_SLEEP_MS);
}

/* Charge history (plan 6.8). */
static t5_err_t hist_save(void *arg)
{
    (void)arg;
    if (!s_hist || !s_hist_dirty) return T5_OK;
    char p[96];
    path(p, sizeof p, HIST_FILE);
    t5_mutex_lock(s_lock);
    s_hist_dirty = false;
    t5_err_t e = t5_storage_blob_write(p, T5_BAT_HIST_VERSION, s_hist, sizeof *s_hist);
    if (e != T5_OK) s_hist_dirty = true;
    t5_mutex_unlock(s_lock);
    return e;
}

static void history_init(void)
{
    s_hist = calloc(1, sizeof *s_hist);
    if (!s_hist) return;
    char p[96];
    path(p, sizeof p, HIST_FILE);
    size_t len = 0;
    if (t5_storage_blob_read(p, T5_BAT_HIST_VERSION, s_hist, sizeof *s_hist, &len) != T5_OK || len != sizeof *s_hist ||
        !t5_bat_hist_valid(s_hist)) {
        t5_bat_hist_init(s_hist);
    }
    T5_LOG_ON_ERR(TAG, t5_autosave_register("bathist", hist_save, NULL));
}

int t5_battery_history(t5_bat_hist_pt_t *out, int max)
{
    if (!s_hist || !s_lock) return 0;
    t5_mutex_lock(s_lock);
    int n = t5_bat_hist_read(s_hist, out, max);
    t5_mutex_unlock(s_lock);
    return n;
}

t5_err_t t5_battery_init(void)
{
    if (s_inited) return T5_OK;
    if (!s_lock) s_lock = t5_mutex_create();
    t5_err_t err = start_sensor();
    if (err != T5_OK) {
        T5_LOGE(TAG, "INA226: %s", t5_err_str(err));
        return err;
    }
    t5_gauge_saved_t rec;
    bool have = load_state(&rec);
    t5_ina226_sample_t smp = {0};
    err = read_boot_sample(&smp);
    if (err != T5_OK) {
        /* A zero sample would boot the gauge at 0 mV, which reads as empty. */
        T5_LOGE(TAG, "INA226 read: %s", t5_err_str(err));
        return err;
    }
    int64_t elapsed = -1;
    if (have && rec.saved_utc && t5_time_valid()) {
        elapsed = (int64_t)t5_time_now() - rec.saved_utc;
        if (elapsed < 0) elapsed = -1;
    }
    t5_gauge_boot(&s_g, have ? &rec : NULL, elapsed, smp.mv, smp.ma10);
    s_st.ready = true;
    s_st.mv = smp.mv;
    s_st.ma10 = smp.ma10;
    s_st.percent = t5_gauge_percent(&s_g);
    s_st.capacity_mah = s_g.capacity_mah;
    s_st.swapped = s_g.swapped;
    s_saved_pct = s_st.percent;
    s_saved_ms = t5_time_ms();
    T5_LOGI(TAG, "%u mV, %ld mA, %d%% of %ld mAh%s%s", smp.mv, (long)(smp.ma10 / 10), s_st.percent,
            (long)s_g.capacity_mah, have ? "" : " (no saved state)", s_g.swapped ? ", pack swapped" : "");
    if (s_g.swapped) post(T5_EV_BATTERY_SWAPPED);
    T5_LOG_ON_ERR(TAG, t5_autosave_register("battery", save_state, NULL));
    history_init();
    t5_power_set_shutdown_handler(shutdown_handler, NULL);
    s_inited = true;
    t5_battery_step();
    return t5_battery_hw_start_task(t5_battery_step, T5_BATTERY_PERIOD_MS);
}

void t5_battery_reset_for_test(void)
{
    t5_ina226_reset_for_test();
    memset(&s_g, 0, sizeof s_g);
    memset(&s_dis, 0, sizeof s_dis);
    memset(&s_st, 0, sizeof s_st);
    s_st.percent = -1;
    s_st.minutes_left = -1;
    s_st.imu_c10 = s_st.chip_c10 = T5_CHG_TEMP_UNKNOWN;
    memset(&s_set, 0, sizeof s_set);
    s_set_saved = s_set;
    s_last_ms = 0;
    s_saved_pct = -2;
    s_temp_tick = 0;
    s_applied = false;
    s_inited = false;
    free(s_hist);
    s_hist = NULL;
    s_hist_acc_ma10 = 0;
    s_hist_acc_n = s_hist_last_ms = 0;
    s_hist_dirty = false;
    s_screen_fn = NULL;
}

