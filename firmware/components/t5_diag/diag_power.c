/* SPDX-License-Identifier: Apache-2.0
 * power, rail, bat: Phase 2 services.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "esp_pm.h"
#include "sdkconfig.h"
#include "t5_battery/battery.h"
#include "t5_power/autosave.h"
#include "t5_power/power.h"
#include "t5_power/rail.h"
#include "t5_time/t5_time.h"
#include "diag_priv.h"

/* Locks taken from the console, so they can be released by type. */
static t5_wakelock_t *s_console_locks[T5_LOCK_TYPE_COUNT];
static t5_rail_hold_t *s_console_rails[T5_RAIL_COUNT];

static bool parse_lock(const char *s, t5_lock_type_t *out)
{
    for (int t = 0; t < T5_LOCK_TYPE_COUNT; t++) {
        if (!strcmp(s, t5_lock_type_name((t5_lock_type_t)t))) { *out = (t5_lock_type_t)t; return true; }
    }
    return false;
}

static bool parse_rail(const char *s, t5_rail_t *out)
{
    for (int r = 0; r < T5_RAIL_COUNT; r++) {
        if (!strcmp(s, t5_rail_name((t5_rail_t)r))) { *out = (t5_rail_t)r; return true; }
    }
    return false;
}

static void print_status(void)
{
    t5_power_stats_t st;
    t5_power_stats(&st);
    t5_power_timeouts_t to;
    t5_power_get_timeouts(&to);
    t5_off_reason_t off;
    bool had_off = t5_power_last_off(&off);
    printf("state %s, idle %u ms, cpu %u MHz, low-power %s, profile %s\n", t5_power_state_name(t5_power_state()),
           (unsigned)st.idle_ms, (unsigned)st.cpu_mhz, t5_power_low_power() ? "on" : "off",
           t5_power_profile() == T5_PROFILE_MOUNTED ? "mounted" : "handheld");
    printf("timers: dim %u s, screen off %u s, sleep +%u s, soft off +%u s\n", (unsigned)(to.dim_ms / 1000),
           (unsigned)(to.screen_off_ms / 1000), (unsigned)(to.sleep_ms / 1000), (unsigned)(to.soft_off_ms / 1000));
    printf("wake reason %s%s%s, restore session %s, motion wake %s\n", t5_wake_reason_name(t5_power_wake_reason()),
           had_off ? ", last off " : "", had_off ? t5_off_reason_name(off) : "",
           t5_power_should_restore_session() ? "yes" : "no", t5_power_motion_wake() ? "on" : "off");
    printf("sleeps %u, resumes %u (failures %u), last suspend %u ms, last resume %u ms (max %u)\n",
           (unsigned)st.sleeps, (unsigned)st.resumes, (unsigned)st.resume_failures, (unsigned)st.last_suspend_ms,
           (unsigned)st.last_resume_ms, (unsigned)st.max_resume_ms);
    printf("time in state (s):");
    for (int s = 0; s <= T5_PWR_SLEEP; s++) {
        printf(" %s %u", t5_power_state_name((t5_power_state_t)s), (unsigned)(st.state_ms[s] / 1000));
    }
    printf("\nautosave runs %u, failures %u\n", (unsigned)t5_autosave_runs(), (unsigned)t5_autosave_failures());
    uint32_t flags;
    time_t w = t5_power_next_wake(&flags);
    if (w) printf("scheduled wake at %lld (in %lld s)%s\n", (long long)w, (long long)(w - t5_time_now()),
                  (flags & T5_WAKE_FLAG_BACKGROUND) ? " background" : "");
}

static void print_locks(void)
{
    t5_wakelock_info_t l[T5_WAKELOCK_MAX];
    int n = t5_wakelock_list(l, T5_WAKELOCK_MAX);
    printf("%d wake lock(s)\n", n);
    for (int i = 0; i < n && i < T5_WAKELOCK_MAX; i++) {
        printf("  %-7s %-16s age %u ms%s", t5_lock_type_name(l[i].type), l[i].owner, (unsigned)l[i].age_ms,
               l[i].left_ms ? "" : "\n");
        if (l[i].left_ms) printf(", %u ms left\n", (unsigned)l[i].left_ms);
    }
}

static void print_hooks(void)
{
    t5_suspend_stats_t h[T5_SUSPEND_MAX_HOOKS];
    int n = t5_power_hook_stats(h, T5_SUSPEND_MAX_HOOKS);
    printf("prio  hook            budget  last-susp last-res  max  overruns fails\n");
    for (int i = 0; i < n; i++) {
        printf("%-5d %-15s %4u ms %6u ms %6u ms %4u %8u %5u\n", h[i].prio, h[i].name, (unsigned)h[i].budget_ms,
               (unsigned)h[i].last_suspend_ms, (unsigned)h[i].last_resume_ms, (unsigned)h[i].max_ms,
               (unsigned)h[i].overruns, (unsigned)h[i].failures);
    }
}

static int cmd_power(int argc, char **argv)
{
    uint32_t v;
    t5_lock_type_t lt;
    if (argc == 1) { print_status(); return 0; }
    const char *c = argv[1];
    if (!strcmp(c, "locks")) { print_locks(); return 0; }
    if (!strcmp(c, "hooks")) { print_hooks(); return 0; }
#if CONFIG_PM_ENABLE
    if (!strcmp(c, "pm")) { esp_pm_dump_locks(stdout); return 0; }
#endif
    if (!strcmp(c, "screenoff")) { t5_power_request_screen_off(); return 0; }
    if (!strcmp(c, "sleep")) { t5_power_request_sleep(); return 0; }
    if (!strcmp(c, "wake")) { t5_power_request_active(); return 0; }
    if (!strcmp(c, "profile") && argc >= 3) {
        t5_power_profile_t p = !strcmp(argv[2], "mounted") ? T5_PROFILE_MOUNTED : T5_PROFILE_HANDHELD;
        return t5_power_set_profile(p) != T5_OK;
    }
    if (!strcmp(c, "timers") && argc >= 6) {
        uint32_t d, o, s, f;
        if (!diag_parse_u32(argv[2], 0, 86400, &d) || !diag_parse_u32(argv[3], 0, 86400, &o) ||
            !diag_parse_u32(argv[4], 0, 86400, &s) || !diag_parse_u32(argv[5], 0, 30 * 86400, &f)) goto usage;
        t5_power_timeouts_t to = { d * 1000, o * 1000, s * 1000, f * 1000 };
        return t5_power_set_timeouts(&to) != T5_OK;
    }
    if (!strcmp(c, "lowpower") && argc >= 3) { t5_power_set_low_power(!strcmp(argv[2], "on")); return 0; }
    if (!strcmp(c, "motionwake") && argc >= 3) { t5_power_set_motion_wake(!strcmp(argv[2], "on")); return 0; }
    if (!strcmp(c, "lock") && argc >= 3 && parse_lock(argv[2], &lt)) {
        uint32_t ms = 0;
        if (argc >= 4 && !diag_parse_u32(argv[3], 1, 86400000, &ms)) goto usage;
        if (s_console_locks[lt]) t5_wakelock_release(s_console_locks[lt]);
        s_console_locks[lt] = ms ? NULL : t5_wakelock_acquire(lt, "console");
        if (ms) t5_wakelock_acquire_timeout(lt, "console", ms);
        return 0;
    }
    if (!strcmp(c, "unlock") && argc >= 3 && parse_lock(argv[2], &lt)) {
        t5_wakelock_release(s_console_locks[lt]);
        s_console_locks[lt] = NULL;
        return 0;
    }
    if (!strcmp(c, "alarm") && argc >= 3 && diag_parse_u32(argv[2], 60, 30 * 86400, &v)) {
        bool bg = argc >= 4 && !strcmp(argv[3], "bg");
        t5_err_t e = t5_power_schedule_wake(t5_time_now() + v, bg ? T5_WAKE_FLAG_BACKGROUND : 0);
        printf("%s\n", t5_err_str(e));
        return e != T5_OK;
    }
    if (!strcmp(c, "cancel")) { t5_power_cancel_wake(); return 0; }
    if (!strcmp(c, "save")) { printf("%s\n", t5_err_str(t5_autosave_now())); return 0; }
    if (!strcmp(c, "off")) {
        if (argc < 3 || strcmp(argv[2], "--force")) {
            printf("refusing: this cuts main power. The board comes back on the power button, or at a\n"
                   "'power alarm' time (that arms an RTC interrupt). With USB attached it enters charge\n"
                   "mode instead. Add --force.\n");
            return 1;
        }
        printf("soft off...\n");
        fflush(stdout);
        t5_err_t e = t5_power_soft_off(T5_OFF_USER, false);
        printf("soft off returned %s\n", t5_err_str(e));
        return e != T5_OK;
    }
usage:
    printf("usage: power [locks | hooks | pm | screenoff | sleep | wake | lowpower on|off | motionwake on|off\n"
           "             | profile handheld|mounted | timers <dim> <off> <sleep> <softoff> (s, 0 = never)\n"
           "             | lock <screen|cpu|audio|net|rail> [ms] | unlock <type>\n"
           "             | alarm <s> [bg] | cancel | save | off --force]\n");
    return 1;
}

static int cmd_rail(int argc, char **argv)
{
    t5_rail_t r;
    if (argc == 1) {
        printf("rail       hw   holders  errors  owners\n");
        for (int i = 0; i < T5_RAIL_COUNT; i++) {
            const char *own[8];
            int n = t5_rail_owners((t5_rail_t)i, own, 8);
            printf("%-10s %-4s %-8d %-7u", t5_rail_name((t5_rail_t)i), t5_rail_is_on((t5_rail_t)i) ? "on" : "off", n,
                   (unsigned)t5_rail_errors((t5_rail_t)i));
            for (int k = 0; k < n && k < 8; k++) printf(" %s", own[k]);
            printf("\n");
        }
        return 0;
    }
    if (argc >= 3 && parse_rail(argv[2], &r) && r != T5_RAIL_BACKLIGHT) {
        if (!strcmp(argv[1], "on")) {
            if (s_console_rails[r]) return 0;
            bool keep = argc >= 4 && !strcmp(argv[3], "keep");
            t5_err_t e = t5_rail_acquire(r, "console", keep ? T5_RAIL_KEEP_IN_SLEEP : 0, &s_console_rails[r]);
            printf("%s\n", t5_err_str(e));
            return e != T5_OK;
        }
        if (!strcmp(argv[1], "off")) {
            t5_err_t e = s_console_rails[r] ? t5_rail_release(s_console_rails[r]) : T5_OK;
            s_console_rails[r] = NULL;
            printf("%s\n", t5_err_str(e));
            return e != T5_OK;
        }
    }
    printf("usage: rail | rail on <usb5v|ext5v|wlan|spk> [keep] | rail off <rail>\n"
           "  holds are the console's own; services' holds stay\n");
    return 1;
}

static void print_battery(void)
{
    t5_battery_status_t b;
    t5_battery_status(&b);
    if (!b.ready) { printf("battery service not running\n"); return; }
    long ma = (long)b.ma10;
    printf("%u mV  %s%ld.%ld mA  %ld mW  %d%%  %ld/%ld mAh", b.mv, ma < 0 ? "-" : "", labs(ma) / 10, labs(ma) % 10,
           (long)b.mw, b.percent, (long)b.charge_mah, (long)b.capacity_mah);
    if (b.minutes_left >= 0) printf("  %ldh%02ld left", (long)(b.minutes_left / 60), (long)(b.minutes_left % 60));
    printf("\nusb %s, %s, CHG_EN %d, fast %d, limit %s, low-power %s%s\n", b.usb ? "yes" : "no",
           t5_charge_state_name(b.charge), b.chg_en, b.fast, t5_battery_charge_limit() ? "80%" : "off",
           b.low_power ? "on" : "off", b.swapped ? ", pack swapped at boot" : "");
    printf("CHG_STAT %d (hi: %u charging / %u discharging samples, lo: %u / %u)\n", b.chg_stat,
           (unsigned)b.chg_stat_hi_charging, (unsigned)b.chg_stat_hi_discharging, (unsigned)b.chg_stat_lo_charging,
           (unsigned)b.chg_stat_lo_discharging);
    printf("temps: imu ");
    if (b.imu_c10 == T5_CHG_TEMP_UNKNOWN) printf("-"); else printf("%d.%d C", b.imu_c10 / 10, abs(b.imu_c10) % 10);
    printf(", chip ");
    if (b.chip_c10 == T5_CHG_TEMP_UNKNOWN) printf("-"); else printf("%d.%d C", b.chip_c10 / 10, abs(b.chip_c10) % 10);
    printf("\nsamples %u, implausible %u, read errors %u, learned cycles %u, trace %s\n", (unsigned)b.samples,
           (unsigned)b.invalid_samples, (unsigned)b.read_errors, b.cycles,
           t5_battery_trace_path() ? t5_battery_trace_path() : "off");
}

static int cmd_bat(int argc, char **argv)
{
    if (argc == 1) { print_battery(); return 0; }
    if (!strcmp(argv[1], "limit") && argc >= 3) return t5_battery_set_charge_limit(!strcmp(argv[2], "on")) != T5_OK;
    if (!strcmp(argv[1], "fast") && argc >= 3) return t5_battery_set_fast_charge(!strcmp(argv[2], "on")) != T5_OK;
    if (!strcmp(argv[1], "log") && argc >= 3) {
        if (!strcmp(argv[2], "stop")) { t5_battery_trace_stop(); return 0; }
        if (!strcmp(argv[2], "start")) {
            t5_err_t e = t5_battery_trace_start(argc >= 4 ? argv[3] : NULL);
            printf("%s %s\n", t5_err_str(e), t5_battery_trace_path() ? t5_battery_trace_path() : "");
            return e != T5_OK;
        }
    }
    printf("usage: bat | bat limit on|off | bat fast on|off | bat log start [path] | bat log stop\n");
    return 1;
}

void diag_register_power(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "power", .help = "Power states, wake locks, hooks, soft off", .hint = "[locks|hooks|lock|...]", .func = cmd_power },
        { .command = "rail", .help = "Switched rails and their holders", .hint = "[on|off <rail>]", .func = cmd_rail },
        { .command = "bat", .help = "Battery gauge, charging, trace log", .hint = "[limit|fast|log]", .func = cmd_bat },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}
