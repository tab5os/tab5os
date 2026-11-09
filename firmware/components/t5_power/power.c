/* SPDX-License-Identifier: Apache-2.0
 * Power service: the state machine, soft off and the boot wake reason.
 */
#include <stdio.h>
#include <string.h>
#include "t5_bsp/xpin.h"
#include "t5_core/event.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_pins/pins.h"
#include "t5_power/arbiter.h"
#include "t5_power/autosave.h"
#include "t5_power/rail.h"
#include "t5_storage/t5_storage.h"
#include "t5_time/rtc.h"
#include "t5_time/t5_time.h"
#include "priv.h"

static const char *TAG = "t5.power";

#define DIM_PERCENT 10            /* backlight cap in Dim */
#define LOW_POWER_PERCENT 30      /* backlight cap in low-power mode */
#define LOW_POWER_DIM_MS 15000
#define LOW_POWER_OFF_MS 60000
#define TICK_AWAKE_MS 200
#define TICK_ASLEEP_MS 1000
#define SOFT_OFF_RETRY_MS (60u * 60 * 1000)  /* after a failed idle soft off */
#define ALARM_WAKE_LOCK_MS 10000
#define PWROFF_TOGGLES 10         /* M5Unified's sequence: 10 x 50 ms toggles */
#define PWROFF_STEP_MS 50
#define PWROFF_GRACE_MS 1500      /* still running after this: the cut failed */
/* T5_EV_POWER_SHUTDOWN is delivered on the event task, apps' handlers among
 * them; this is their time to act before the suspend hooks stop the apps. */
#define SHUTDOWN_NOTICE_MS 50
#define SETTINGS_VERSION 1

typedef struct {
    uint8_t profile;
    uint8_t custom;               /* timeouts differ from the profile's */
    uint8_t motion_wake;
    uint8_t reserved;
    t5_power_timeouts_t to;
} settings_t;

static t5_power_state_t s_state = T5_PWR_ACTIVE;
static volatile uint32_t s_last_input_ms;
static volatile bool s_forced_off;
static volatile uint32_t s_forced_ms;     /* when "screen off" was asked for */
static volatile bool s_forced_sleep;      /* ...and straight on into Sleep */
static volatile bool s_input_pending;
static settings_t s_set, s_saved;
static bool s_low_power;
static bool s_usb;
static bool s_inited;
static t5_power_outputs_t s_out;
static bool s_lit;
static int64_t s_wake_trigger_us;       /* when something asked to leave Sleep */
static uint32_t s_state_since_ms;
static uint32_t s_soft_off_block_until;
static t5_power_stats_t s_stats;
static t5_shutdown_handler_t s_shutdown_fn;
static void *s_shutdown_arg;
static bool s_in_soft_off;
static volatile uint32_t s_cpu_until_ms;   /* t5_power_wake() deadline, 0 = none */
static volatile t5_power_cause_t s_wake_cause = T5_PWR_CAUSE_LOCK;

static time_t s_wake_at;
static uint32_t s_wake_flags;

static t5_wake_reason_t s_wake = T5_WAKE_UNKNOWN;
static bool s_have_last_off;
static t5_off_reason_t s_last_off;
static bool s_restore_session;

const char *t5_power_state_name(t5_power_state_t s)
{
    switch (s) {
    case T5_PWR_ACTIVE: return "active";
    case T5_PWR_DIM: return "dim";
    case T5_PWR_SCREEN_OFF: return "screen-off";
    case T5_PWR_SLEEP: return "sleep";
    case T5_PWR_SOFT_OFF: return "soft-off";
    case T5_PWR_CHARGE_MODE: return "charge-mode";
    default: return "?";
    }
}

const char *t5_power_cause_name(t5_power_cause_t c)
{
    switch (c) {
    case T5_PWR_CAUSE_BOOT: return "boot";
    case T5_PWR_CAUSE_IDLE: return "idle";
    case T5_PWR_CAUSE_INPUT: return "input";
    case T5_PWR_CAUSE_LOCK: return "lock";
    case T5_PWR_CAUSE_REQUEST: return "request";
    case T5_PWR_CAUSE_WAKE_NET: return "net";
    case T5_PWR_CAUSE_WAKE_TIMER: return "timer";
    case T5_PWR_CAUSE_BATTERY: return "battery";
    default: return "?";
    }
}

const char *t5_off_reason_name(t5_off_reason_t r)
{
    switch (r) {
    case T5_OFF_USER: return "user";
    case T5_OFF_IDLE: return "idle";
    case T5_OFF_BATTERY: return "battery";
    case T5_OFF_BATTERY_CRITICAL: return "battery-critical";
    case T5_OFF_CHARGE_UNPLUG: return "charge-unplug";
    default: return "?";
    }
}

const char *t5_wake_reason_name(t5_wake_reason_t r)
{
    switch (r) {
    case T5_WAKE_COLD: return "cold";
    case T5_WAKE_BUTTON: return "button";
    case T5_WAKE_RTC_ALARM: return "rtc-alarm";
    case T5_WAKE_POWER_CYCLE: return "power-cycle";
    case T5_WAKE_REBOOT: return "reboot";
    case T5_WAKE_CRASH: return "crash";
    case T5_WAKE_BROWNOUT: return "brownout";
    case T5_WAKE_CHARGE_TIMER: return "charge-timer";
    default: return "unknown";
    }
}

static void settings_path(char *out, size_t cap)
{
    T5_IGNORE(t5_sys_path(out, cap, "power.bin"));
}

static t5_err_t save_settings(void *arg)
{
    (void)arg;
    if (!memcmp(&s_set, &s_saved, sizeof s_set)) {
        return T5_OK;
    }
    char path[96];
    settings_path(path, sizeof path);
    t5_err_t err = t5_storage_blob_write(path, SETTINGS_VERSION, &s_set, sizeof s_set);
    if (err == T5_OK) s_saved = s_set;
    return err;
}

static void load_settings(void)
{
    char path[96];
    settings_path(path, sizeof path);
    settings_t s;
    size_t len = 0;
    if (t5_storage_blob_read(path, SETTINGS_VERSION, &s, sizeof s, &len) == T5_OK && len == sizeof s &&
        s.profile <= T5_PROFILE_MOUNTED) {
        s_set = s;
        if (!s_set.custom) t5_power_profile_timeouts((t5_power_profile_t)s_set.profile, &s_set.to);
    } else {
        memset(&s_set, 0, sizeof s_set);
        s_set.profile = T5_PROFILE_HANDHELD;
        t5_power_profile_timeouts(T5_PROFILE_HANDHELD, &s_set.to);
    }
    s_saved = s_set;
}

t5_err_t t5_power_set_profile(t5_power_profile_t p)
{
    if (p > T5_PROFILE_MOUNTED) return T5_ERR_INVALID_ARG;
    s_set.profile = (uint8_t)p;
    s_set.custom = 0;
    t5_power_profile_timeouts(p, &s_set.to);
    t5_power_hw_notify(false);
    return T5_OK;
}

t5_power_profile_t t5_power_profile(void)
{
    return (t5_power_profile_t)s_set.profile;
}

t5_err_t t5_power_set_timeouts(const t5_power_timeouts_t *t)
{
    if (!t) return T5_ERR_INVALID_ARG;
    s_set.to = *t;
    s_set.custom = 1;
    t5_power_hw_notify(false);
    return T5_OK;
}

void t5_power_get_timeouts(t5_power_timeouts_t *out)
{
    *out = s_set.to;
}

static int active_dim_cap(void)
{
    return s_low_power ? LOW_POWER_PERCENT : -1;
}

static void screen(bool lit, int cap)
{
    s_lit = lit;
    if (s_out.screen) s_out.screen(lit, cap, s_out.arg);
}

void t5_power_set_low_power(bool on)
{
    if (on == s_low_power) return;
    s_low_power = on;
    T5_LOGI(TAG, "low-power mode %s", on ? "on" : "off");
    if (s_state == T5_PWR_ACTIVE) screen(true, active_dim_cap());
    if (s_out.low_power) s_out.low_power(on, s_out.arg);
    t5_power_hw_notify(false);
}

bool t5_power_low_power(void)
{
    return s_low_power;
}

void t5_power_set_usb_power(bool present)
{
    s_usb = present;
}

void t5_power_user_activity(void)
{
    s_last_input_ms = t5_time_ms();
    s_forced_off = false;
    s_forced_sleep = false;
    if (s_state != T5_PWR_ACTIVE) {
        if (!s_wake_trigger_us) s_wake_trigger_us = t5_time_us();
        s_input_pending = true;
        t5_power_hw_notify(false);
    }
}

void t5_power_wake(t5_power_cause_t cause, uint32_t ms)
{
    if (s_state == T5_PWR_SLEEP && !s_wake_trigger_us) {
        s_wake_trigger_us = t5_time_us();
    }
    /* A deadline rather than a wake lock, so a chatty wake source can't fill
     * the lock table: the arbiter counts it as one CPU lock until it passes. */
    uint32_t until = t5_time_ms() + (ms ? ms : 1000);
    if (!s_cpu_until_ms || (int32_t)(until - s_cpu_until_ms) > 0) s_cpu_until_ms = until;
    s_wake_cause = cause;
    t5_power_hw_notify(false);
}

void t5_power_request_screen_off(void)
{
    s_forced_ms = t5_time_ms();
    s_forced_off = true;
    t5_power_hw_notify(false);
}

void t5_power_request_sleep(void)
{
    s_forced_sleep = true;
    t5_power_request_screen_off();
}

void t5_power_set_shutdown_handler(t5_shutdown_handler_t fn, void *arg)
{
    s_shutdown_arg = arg;
    s_shutdown_fn = fn;
}

void t5_power_set_outputs(const t5_power_outputs_t *out)
{
    s_out = out ? *out : (t5_power_outputs_t){ 0 };
}

bool t5_power_screen_lit(void)
{
    return s_lit;
}

t5_err_t t5_power_schedule_wake(time_t utc, uint32_t flags)
{
    if (!t5_time_valid()) return T5_ERR_STATE;   /* an alarm needs real wall time */
    if (utc <= t5_time_now()) return T5_ERR_INVALID_ARG;
    s_wake_at = utc;
    s_wake_flags = flags;
    t5_power_hw_notify(false);
    return T5_OK;
}

void t5_power_cancel_wake(void)
{
    s_wake_at = 0;
    s_wake_flags = 0;
}

time_t t5_power_next_wake(uint32_t *flags)
{
    if (flags) *flags = s_wake_flags;
    return s_wake_at;
}

static void effective_timeouts(t5_power_timeouts_t *to)
{
    *to = s_set.to;
    if (s_low_power) {
        if (!to->dim_ms || to->dim_ms > LOW_POWER_DIM_MS) to->dim_ms = LOW_POWER_DIM_MS;
        if (!to->screen_off_ms || to->screen_off_ms > LOW_POWER_OFF_MS) to->screen_off_ms = LOW_POWER_OFF_MS;
    }
}

/* One step deeper, from s_state. */
static void enter_deeper(void)
{
    switch (s_state) {
    case T5_PWR_ACTIVE:                         /* -> Dim */
        screen(true, DIM_PERCENT);
        s_state = T5_PWR_DIM;
        break;
    case T5_PWR_DIM:                            /* -> Screen off */
        screen(false, DIM_PERCENT);
        s_state = T5_PWR_SCREEN_OFF;
        break;
    case T5_PWR_SCREEN_OFF: {                   /* -> Sleep */
        uint32_t ms = 0;
        int failed = t5_suspend_run(0, &ms);
        s_stats.last_suspend_ms = ms;
        s_stats.sleeps++;
        t5_power_hw_cpu_low(true);
        T5_LOGI(TAG, "sleep: suspend took %u ms%s", (unsigned)ms, failed ? " (some hooks failed)" : "");
        s_state = T5_PWR_SLEEP;
        break;
    }
    default:
        break;
    }
}

/* One step shallower, from s_state. */
static void exit_to_shallower(void)
{
    switch (s_state) {
    case T5_PWR_SLEEP: {                        /* -> Screen off */
        t5_power_hw_cpu_low(false);
        uint32_t ms = 0;
        int failed = t5_resume_run(0, &ms);
        s_stats.resumes++;
        if (failed) s_stats.resume_failures++;
        T5_LOGI(TAG, "resume took %u ms%s", (unsigned)ms, failed ? " (some hooks failed)" : "");
        s_state = T5_PWR_SCREEN_OFF;
        break;
    }
    case T5_PWR_SCREEN_OFF:                     /* -> Dim */
        screen(true, DIM_PERCENT);
        if (s_wake_trigger_us) {
            uint32_t ms = (uint32_t)((t5_time_us() - s_wake_trigger_us) / 1000);
            s_stats.last_resume_ms = ms;
            if (ms > s_stats.max_resume_ms) s_stats.max_resume_ms = ms;
            s_wake_trigger_us = 0;
        }
        s_state = T5_PWR_DIM;
        break;
    case T5_PWR_DIM:                            /* -> Active */
        screen(true, active_dim_cap());
        s_state = T5_PWR_ACTIVE;
        break;
    default:
        break;
    }
}

static void account(uint32_t now)
{
    if (s_state < T5_PWR_STATE_COUNT) {
        s_stats.state_ms[s_state] += now - s_state_since_ms;
    }
    s_state_since_ms = now;
}

void t5_power_tick(void)
{
    uint32_t now = t5_time_ms();
    t5_wakelock_expire(now);   /* the arbiter below sees the new counts */

    /* A scheduled wake while running: hold the CPU so the alarm work can run. */
    if (s_wake_at && t5_time_valid() && t5_time_now() >= s_wake_at) {
        T5_LOGI(TAG, "scheduled wake");
        s_wake_at = 0;
        t5_power_wake(T5_PWR_CAUSE_WAKE_TIMER, ALARM_WAKE_LOCK_MS);
        t5_event_post(T5_EV_RTC_ALARM, NULL, 0);
    }

    t5_arb_in_t in = { .usb_power = s_usb };
    t5_wakelock_counts(in.locks);
    bool woken = false;
    if (s_cpu_until_ms) {
        if ((int32_t)(now - s_cpu_until_ms) < 0) {
            in.locks[T5_LOCK_CPU]++;
            woken = true;
        } else {
            s_cpu_until_ms = 0;
        }
    }
    effective_timeouts(&in.to);
    in.idle_ms = now - s_last_input_ms;
    if (s_forced_off) {
        /* "Screen off" acts as if the screen-off timer ran out when it was
         * pressed; the later timers count from there. */
        if (!in.to.screen_off_ms) in.to.screen_off_ms = 1;
        uint32_t since = now - s_forced_ms;
        uint32_t forced = in.to.screen_off_ms + since;
        if (s_forced_sleep && in.to.sleep_ms) forced += in.to.sleep_ms;
        if (forced < since) forced = UINT32_MAX;
        if (forced > in.idle_ms) in.idle_ms = forced;
    }
    s_stats.idle_ms = in.idle_ms;
    t5_power_state_t target = t5_power_arbitrate(&in);

    if (target == T5_PWR_SOFT_OFF) {
        if ((int32_t)(now - s_soft_off_block_until) >= 0) {
            T5_LOGI(TAG, "asleep for %u ms: soft off", (unsigned)in.to.soft_off_ms);
            if (t5_power_soft_off(T5_OFF_IDLE, false) != T5_OK) {
                s_soft_off_block_until = now + SOFT_OFF_RETRY_MS;
            }
        }
        target = T5_PWR_SLEEP;
    }

    t5_power_state_t from = s_state;
    if (from == target) {
        t5_autosave_poll(now);
        return;
    }
    t5_power_cause_t cause = s_input_pending ? T5_PWR_CAUSE_INPUT
                           : t5_power_depth(target) > t5_power_depth(from) ? T5_PWR_CAUSE_IDLE
                           : woken ? s_wake_cause : T5_PWR_CAUSE_LOCK;
    s_input_pending = false;
    account(now);
    /* The steps only know Active..Sleep (soft off became Sleep above). Each
     * one moves exactly one level, so at most SLEEP steps either way
     * (docs/coding-standard.md, rules 2 and 5). */
    T5_ASSERT(target <= T5_PWR_SLEEP && s_state <= T5_PWR_SLEEP);
    for (int i = 0; i < T5_PWR_SLEEP && t5_power_depth(s_state) < t5_power_depth(target); i++) {
        int d = t5_power_depth(s_state);
        enter_deeper();
        T5_ASSERT(t5_power_depth(s_state) == d + 1);
    }
    for (int i = 0; i < T5_PWR_SLEEP && t5_power_depth(s_state) > t5_power_depth(target); i++) {
        int d = t5_power_depth(s_state);
        exit_to_shallower();
        T5_ASSERT(t5_power_depth(s_state) == d - 1);
    }
    T5_ASSERT(s_state == target);
    if (s_state == T5_PWR_ACTIVE) s_wake_trigger_us = 0;
    T5_LOGI(TAG, "%s -> %s (%s)", t5_power_state_name(from), t5_power_state_name(s_state), t5_power_cause_name(cause));
    t5_ev_power_t ev = { (uint8_t)from, (uint8_t)s_state, (uint8_t)cause };
    t5_event_post(T5_EV_POWER_STATE, &ev, sizeof ev);
    /* Every state change is a save point (plan 2.12). */
    T5_LOG_ON_ERR(TAG, t5_autosave_now());
}

t5_power_state_t t5_power_state(void)
{
    return s_state;
}

void t5_power_stats(t5_power_stats_t *out)
{
    *out = s_stats;
    uint32_t now = t5_time_ms();
    if (s_state < T5_PWR_STATE_COUNT) out->state_ms[s_state] += now - s_state_since_ms;
    out->idle_ms = now - s_last_input_ms;
    out->cpu_mhz = t5_power_hw_cpu_mhz();
}

static void alarm_tm(time_t at, struct tm *out)
{
    /* The RX8130 alarm has minute resolution and ignores seconds: round up,
     * and keep it at least a minute out so it can't match while we shut down. */
    time_t now = t5_time_now();
    if (at < now + 60) at = now + 60;
    at = (at + 59) / 60 * 60;
    gmtime_r(&at, out);
}

static void pwroff_pulse(void)
{
    for (int i = 0; i < PWROFF_TOGGLES; i++) {
        t5_xpin_output(T5_XP_PWROFF_PULSE, (i & 1) == 0);
        t5_delay_ms(PWROFF_STEP_MS);
    }
}

t5_err_t t5_power_soft_off(t5_off_reason_t reason, bool emergency)
{
    if (s_in_soft_off) return T5_ERR_BUSY;
    s_in_soft_off = true;
    T5_LOGW(TAG, "soft off: %s%s", t5_off_reason_name(reason), emergency ? " (emergency)" : "");
    t5_event_post(T5_EV_POWER_SHUTDOWN, NULL, 0);
    if (!emergency) t5_delay_ms(SHUTDOWN_NOTICE_MS);

    /* Apps first (skipped in an emergency), then the system hooks, then save. */
    uint32_t ms = 0;
    int min_prio = emergency ? T5_SUSPEND_PRIO_SYSTEM : 0;
    t5_suspend_run(min_prio, &ms);
    T5_LOG_ON_ERR(TAG, t5_autosave_now());

    /* Charge mode takes over here when USB is attached. On the device it
     * doesn't return; if it declines, soft off carries on. */
    if (!emergency && s_shutdown_fn && s_shutdown_fn(reason, s_shutdown_arg)) {
        t5_resume_run(min_prio, &ms);
        s_in_soft_off = false;
        return T5_OK;
    }

    t5_offrec_t rec = { .reason = (uint8_t)reason, .flags = 0 };
    bool alarm = false;
    if (s_wake_at && !emergency && t5_time_valid()) {
        struct tm tm;
        alarm_tm(s_wake_at, &tm);
        if (t5_rtc_alarm_set(&tm) == T5_OK) {
            alarm = true;
            rec.flags |= T5_OFFREC_FLAG_ALARM;
            if (s_wake_flags & T5_WAKE_FLAG_BACKGROUND) rec.flags |= T5_OFFREC_FLAG_BACKGROUND;
            T5_LOGI(TAG, "RTC alarm %04d-%02d-%02d %02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min);
        }
    }
    uint8_t raw[T5_RTC_RAM_LEN];
    t5_offrec_encode(&rec, raw);
    t5_rtc_ram_write(raw);
    if (s_out.standby) s_out.standby(true, s_set.motion_wake && !emergency, s_out.arg);

    pwroff_pulse();
    t5_delay_ms(PWROFF_GRACE_MS);

    /* Still here: something holds main power up (USB, or PWROFF_PLUSE isn't
     * what the schematic says). Undo, so the device stays usable. */
    T5_LOGE(TAG, "power was not cut");
    t5_xpin_output(T5_XP_PWROFF_PULSE, false);
    if (alarm) t5_rtc_alarm_clear();
    memset(raw, 0, sizeof raw);
    t5_rtc_ram_write(raw);
    if (s_out.standby) s_out.standby(false, s_set.motion_wake, s_out.arg);
    t5_resume_run(min_prio, &ms);
    s_in_soft_off = false;
    t5_power_user_activity();
    return T5_ERR_FAIL;
}

void t5_power_charge_sleep(uint32_t ms)
{
    t5_offrec_t rec = { .reason = T5_OFF_USER, .flags = T5_OFFREC_FLAG_CHARGE };
    uint8_t raw[T5_RTC_RAM_LEN];
    t5_offrec_encode(&rec, raw);
    t5_rtc_ram_write(raw);
    t5_power_hw_halt(ms);
}

void t5_power_charge_mode_end(void)
{
    uint8_t raw[T5_RTC_RAM_LEN] = {0};
    t5_rtc_ram_write(raw);
}

void t5_power_set_motion_wake(bool on)
{
    s_set.motion_wake = on;
}

bool t5_power_motion_wake(void)
{
    return s_set.motion_wake;
}

static bool s_wake_read;

void t5_power_read_wake_reason(void)
{
    if (s_wake_read) return;
    s_wake_read = true;
    t5_wake_in_t in = { .reset = t5_power_hw_reset_reason(), .deep_sleep_timer = t5_power_hw_woke_from_deep_sleep_timer() };
    uint8_t raw[T5_RTC_RAM_LEN] = {0};
    if (t5_rtc_ram_read(raw) == T5_OK) {
        in.rtc_ok = true;
        in.rtc_flags = t5_rtc_boot_flags();
        in.have_rec = t5_offrec_decode(raw, &in.rec);
    }
    s_wake = t5_power_classify_wake(&in);
    s_have_last_off = in.have_rec && !(in.rec.flags & T5_OFFREC_FLAG_CHARGE);
    s_last_off = (t5_off_reason_t)in.rec.reason;
    s_restore_session = s_wake == T5_WAKE_BUTTON || s_wake == T5_WAKE_POWER_CYCLE ||
                        (s_wake == T5_WAKE_RTC_ALARM && !(in.rec.flags & T5_OFFREC_FLAG_BACKGROUND)) ||
                        s_wake == T5_WAKE_REBOOT;
    /* The record is for one boot only. Charge mode keeps its own until it
     * ends; any other boot (the button, during charge mode) clears it. */
    if (in.have_rec && s_wake != T5_WAKE_CHARGE_TIMER) {
        memset(raw, 0, sizeof raw);
        t5_rtc_ram_write(raw);
    }
    T5_LOGI(TAG, "wake reason: %s%s%s", t5_wake_reason_name(s_wake), s_have_last_off ? ", last off: " : "",
            s_have_last_off ? t5_off_reason_name(s_last_off) : "");
}

t5_wake_reason_t t5_power_wake_reason(void)
{
    return s_wake;
}

bool t5_power_last_off(t5_off_reason_t *reason)
{
    if (s_have_last_off && reason) *reason = s_last_off;
    return s_have_last_off;
}

bool t5_power_should_restore_session(void)
{
    return s_restore_session;
}

static t5_err_t rails_suspend(void *arg)
{
    (void)arg;
    return t5_rail_suspend(t5_wakelock_count(T5_LOCK_RAIL) > 0);
}

static t5_err_t rails_resume(void *arg)
{
    (void)arg;
    return t5_rail_resume();
}

static void task_step(void)
{
    t5_power_tick();
    t5_power_hw_wait(s_state == T5_PWR_SLEEP ? TICK_ASLEEP_MS : TICK_AWAKE_MS);
}

t5_err_t t5_power_init(void)
{
    if (s_inited) return T5_OK;
    t5_power_read_wake_reason();
    load_settings();
    T5_RETURN_ON_ERR(t5_power_hw_init());

    s_last_input_ms = t5_time_ms();
    s_state_since_ms = s_last_input_ms;
    s_state = T5_PWR_ACTIVE;
    screen(true, active_dim_cap());

    const t5_suspend_hook_t hooks[] = {
        { .name = "rails", .prio = T5_SUSPEND_PRIO_RAILS, .budget_ms = 100, .suspend = rails_suspend, .resume = rails_resume },
    };
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++) {
        T5_LOG_ON_ERR(TAG, t5_power_register_hook(&hooks[i]));
    }
    T5_LOG_ON_ERR(TAG, t5_autosave_register("power", save_settings, NULL));
    s_inited = true;
    T5_LOGI(TAG, "profile %s: dim %us, screen off %us, sleep +%us, soft off +%uh",
            s_set.profile == T5_PROFILE_MOUNTED ? "mounted" : "handheld", (unsigned)(s_set.to.dim_ms / 1000),
            (unsigned)(s_set.to.screen_off_ms / 1000), (unsigned)(s_set.to.sleep_ms / 1000),
            (unsigned)(s_set.to.soft_off_ms / 3600000));
    return t5_power_hw_start_task(task_step);
}

void t5_power_reset_for_test(void)
{
    s_state = T5_PWR_ACTIVE;
    s_last_input_ms = t5_time_ms();
    s_forced_off = false;
    s_forced_sleep = false;
    s_input_pending = false;
    s_low_power = false;
    s_usb = false;
    s_inited = false;
    s_out = (t5_power_outputs_t){ 0 };
    s_lit = false;
    s_wake_trigger_us = 0;
    s_state_since_ms = s_last_input_ms;
    s_soft_off_block_until = 0;
    memset(&s_stats, 0, sizeof s_stats);
    s_shutdown_fn = NULL;
    s_in_soft_off = false;
    s_wake_at = 0;
    s_wake_flags = 0;
    s_cpu_until_ms = 0;
    s_wake = T5_WAKE_UNKNOWN;
    s_wake_read = false;
    s_have_last_off = false;
    s_restore_session = false;
    memset(&s_set, 0, sizeof s_set);
    t5_power_profile_timeouts(T5_PROFILE_HANDHELD, &s_set.to);
    s_saved = s_set;
}
