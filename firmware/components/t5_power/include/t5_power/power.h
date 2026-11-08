/* SPDX-License-Identifier: Apache-2.0
 * Power service (plan Phase 2): states, wake locks, suspend hooks, soft off.
 *
 * Apps don't pick a state. They hold wake locks, and the service picks the
 * deepest state the locks and idle timers allow:
 *
 *   Active      input within the dim timeout, or a SCREEN lock
 *   Dim         backlight capped, everything runs
 *   Screen off  backlight off, panel and video stay on so touch still wakes
 *   Sleep       Screen off with no CPU or AUDIO lock: suspend hooks run, CPU
 *               drops to its low clock. Not chip light sleep: the ST7123 needs
 *               the video stream to scan touch (bench 0.8), so the panel stays
 *               fed and touch, timers and the C6 wake line all work.
 *   Soft off    main power cut by PWROFF_PLUSE; the PMS150G, RTC and IMU stay
 *               on the standby rail. Wakes by button, RTC alarm or IMU.
 *   Charge mode soft off requested with USB attached (t5_battery handles it).
 *
 * Locks:
 *   SCREEN  keeps Active (no dim, no screen off)
 *   CPU     allows Screen off, blocks Sleep
 *   AUDIO   as CPU; also tells media services to keep playing
 *   NET     allows Sleep; the net service keeps Wi-Fi up in power save
 *           instead of powering the C6 down
 *   RAIL    allows Sleep; every held rail stays on
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_PWR_ACTIVE,
    T5_PWR_DIM,
    T5_PWR_SCREEN_OFF,
    T5_PWR_SLEEP,
    T5_PWR_SOFT_OFF,
    T5_PWR_CHARGE_MODE,
    T5_PWR_STATE_COUNT,
} t5_power_state_t;

typedef enum {
    T5_PWR_CAUSE_BOOT,
    T5_PWR_CAUSE_IDLE,        /* an idle timer ran out */
    T5_PWR_CAUSE_INPUT,       /* touch or keyboard */
    T5_PWR_CAUSE_LOCK,        /* a wake lock was taken or released */
    T5_PWR_CAUSE_REQUEST,     /* t5_power_request_*() */
    T5_PWR_CAUSE_WAKE_NET,    /* the C6 raised its wake line */
    T5_PWR_CAUSE_WAKE_TIMER,  /* t5_power_schedule_wake() while asleep */
    T5_PWR_CAUSE_BATTERY,     /* discharge policy */
} t5_power_cause_t;

typedef enum {
    T5_LOCK_SCREEN,
    T5_LOCK_CPU,
    T5_LOCK_AUDIO,
    T5_LOCK_NET,
    T5_LOCK_RAIL,
    T5_LOCK_TYPE_COUNT,
} t5_lock_type_t;

typedef enum {
    T5_PROFILE_HANDHELD,      /* battery first: short timers */
    T5_PROFILE_MOUNTED,       /* wall or desk panel: long timers, no idle soft off */
} t5_power_profile_t;

/* Idle timers, each measured from the last input. 0 = never. */
typedef struct {
    uint32_t dim_ms;
    uint32_t screen_off_ms;
    uint32_t sleep_ms;        /* after screen off, with no CPU/AUDIO lock */
    uint32_t soft_off_ms;     /* after sleep starts */
} t5_power_timeouts_t;

void t5_power_profile_timeouts(t5_power_profile_t p, t5_power_timeouts_t *out);

/* ---- service ------------------------------------------------------------ */
/* Read the wake reason, load settings, start the state task. Needs storage,
 * time (RTC) and the expanders; call after them and after the display. */
T5_MUST_CHECK t5_err_t t5_power_init(void);
t5_power_state_t t5_power_state(void);
const char *t5_power_state_name(t5_power_state_t s);
const char *t5_power_cause_name(t5_power_cause_t c);

/* Input happened (touch calls this through its activity hook). Wakes from
 * Dim, Screen off and Sleep. Cheap; safe from any task. */
void t5_power_user_activity(void);
/* Something other than input needs the CPU for ms (C6 traffic, a timer):
 * leaves Sleep for Screen off without lighting the screen. */
void t5_power_wake(t5_power_cause_t cause, uint32_t ms);

/* Power menu actions. */
void t5_power_request_screen_off(void);
void t5_power_request_active(void);
/* Screen off and straight into Sleep (locks still apply). */
void t5_power_request_sleep(void);

T5_MUST_CHECK t5_err_t t5_power_set_profile(t5_power_profile_t p);
t5_power_profile_t t5_power_profile(void);
/* Override one timer set; t5_power_set_profile() restores the profile's. */
T5_MUST_CHECK t5_err_t t5_power_set_timeouts(const t5_power_timeouts_t *t);
void t5_power_get_timeouts(t5_power_timeouts_t *out);

/* Low-power mode (battery 10%, or the user): shorter dim, capped
 * brightness. Services read t5_power_low_power() to cap frame rate etc. */
void t5_power_set_low_power(bool on);
bool t5_power_low_power(void);

/* t5_battery reports USB power; the arbiter never soft-offs from idle on it. */
void t5_power_set_usb_power(bool present);

/* ---- wake locks (plan 2.2) ---------------------------------------------- */
typedef struct t5_wakelock t5_wakelock_t;
#define T5_WAKELOCK_MAX 32
#define T5_WAKELOCK_OWNER_LEN 16

/* NULL if the table is full. owner is copied. */
t5_wakelock_t *t5_wakelock_acquire(t5_lock_type_t type, const char *owner);
/* Released by the service after timeout_ms unless released earlier. */
t5_wakelock_t *t5_wakelock_acquire_timeout(t5_lock_type_t type, const char *owner, uint32_t timeout_ms);
void t5_wakelock_release(t5_wakelock_t *lock);
int t5_wakelock_count(t5_lock_type_t type);
const char *t5_lock_type_name(t5_lock_type_t t);

typedef struct {
    t5_lock_type_t type;
    char owner[T5_WAKELOCK_OWNER_LEN];
    uint32_t age_ms;
    uint32_t left_ms;         /* 0 = no timeout */
    void *app;                /* app context of the acquirer */
} t5_wakelock_info_t;
int t5_wakelock_list(t5_wakelock_info_t *out, int max);

/* ---- suspend protocol (plan 2.3) ---------------------------------------- */
/* Hooks run in ascending priority on the way into Sleep (and soft off) and in
 * descending priority on the way out. A hook that runs past its budget is
 * logged and counted, and T5_EV_POWER_SUSPEND_OVERRUN is posted; the sequence
 * continues. Priorities follow the design's sleep sequence: */
#define T5_SUSPEND_PRIO_APPS     100   /* on_suspend, 500 ms budget */
#define T5_SUSPEND_PRIO_STORAGE  200   /* sync /data and /sd */
#define T5_SUSPEND_PRIO_MEDIA    300   /* camera, codecs, speaker amp */
#define T5_SUSPEND_PRIO_DISPLAY  400
#define T5_SUSPEND_PRIO_NET      500   /* C6 power save or off */
#define T5_SUSPEND_PRIO_RAILS    600   /* USB 5V, ext 5V */
/* Hooks below this priority are app callbacks, skipped by an emergency off. */
#define T5_SUSPEND_PRIO_SYSTEM   T5_SUSPEND_PRIO_STORAGE

typedef t5_err_t (*t5_suspend_fn_t)(void *arg);

typedef struct {
    const char *name;         /* static */
    int prio;
    uint32_t budget_ms;
    t5_suspend_fn_t suspend;  /* may be NULL */
    t5_suspend_fn_t resume;   /* may be NULL */
    void *arg;
} t5_suspend_hook_t;

#define T5_SUSPEND_MAX_HOOKS 24
T5_MUST_CHECK t5_err_t t5_power_register_hook(const t5_suspend_hook_t *hook);
T5_MUST_CHECK t5_err_t t5_power_unregister_hook(const char *name);

typedef struct {
    const char *name;
    int prio;
    uint32_t budget_ms;
    uint32_t last_suspend_ms, last_resume_ms;
    uint32_t max_ms;
    uint32_t overruns;
    uint32_t failures;
} t5_suspend_stats_t;
int t5_power_hook_stats(t5_suspend_stats_t *out, int max);

/* ---- soft off (plan 2.6) ------------------------------------------------ */
typedef enum {
    T5_OFF_USER,              /* power menu */
    T5_OFF_IDLE,              /* asleep for soft_off_ms */
    T5_OFF_BATTERY,           /* 5% or 6.5 V */
    T5_OFF_BATTERY_CRITICAL,  /* 6.3 V: emergency, no app callbacks */
    T5_OFF_CHARGE_UNPLUG,     /* charge mode saw USB go away */
    T5_OFF_TEST,
} t5_off_reason_t;

/* Does not return on success. Runs the suspend hooks (system ones only when
 * emergency), autosaves, records the reason in RTC RAM, arms the RTC alarm
 * for a scheduled wake, puts the panel to sleep and pulses PWROFF_PLUSE.
 * Shutdown handlers (charge mode) get a chance first unless emergency.
 * Returns an error only if power could not be cut. */
T5_MUST_CHECK t5_err_t t5_power_soft_off(t5_off_reason_t reason, bool emergency);
const char *t5_off_reason_name(t5_off_reason_t r);

/* t5_battery registers one of these: return true to take over the shutdown
 * (charge mode). It may return false to let soft off continue. */
typedef bool (*t5_shutdown_handler_t)(t5_off_reason_t reason, void *arg);
void t5_power_set_shutdown_handler(t5_shutdown_handler_t fn, void *arg);

/* Charge mode (t5_battery): mark the RTC record and deep sleep the P4 for ms.
 * The expanders keep CHG_EN. Does not return on the device. */
void t5_power_charge_sleep(uint32_t ms);
/* Leaving charge mode: clear the record so the next boot isn't mistaken. */
void t5_power_charge_mode_end(void);

/* Wake at utc. From soft off this programs the RTC alarm (minute resolution,
 * rounded up); while running or asleep it wakes the CPU. flags: */
#define T5_WAKE_FLAG_BACKGROUND 0x01   /* don't restore the session on that boot */
T5_MUST_CHECK t5_err_t t5_power_schedule_wake(time_t utc, uint32_t flags);
/* Arm a latched, active-low any-motion interrupt on the way into soft off,
 * so picking the device up turns it on (bench 0.7). Off by default; saved. */
void t5_power_set_motion_wake(bool on);
bool t5_power_motion_wake(void);
void t5_power_cancel_wake(void);
time_t t5_power_next_wake(uint32_t *flags);

/* ---- wake reason (plan 2.6, 2.11) --------------------------------------- */
typedef enum {
    T5_WAKE_COLD,             /* first power, pack inserted */
    T5_WAKE_BUTTON,           /* soft off, then the power button */
    T5_WAKE_RTC_ALARM,        /* soft off, then a scheduled RTC alarm */
    T5_WAKE_POWER_CYCLE,      /* lost power while running: button press or pack pulled */
    T5_WAKE_REBOOT,           /* software restart */
    T5_WAKE_CRASH,            /* panic or watchdog */
    T5_WAKE_BROWNOUT,
    T5_WAKE_CHARGE_TIMER,     /* charge mode's periodic deep-sleep check */
    T5_WAKE_UNKNOWN,
} t5_wake_reason_t;

/* Classify the boot from the reset reason, RTC flags and the shutdown record
 * in RTC RAM. t5_power_init() calls it; main calls it earlier to catch a
 * charge-mode timer wake before bringing up the display. Needs the RTC. */
void t5_power_read_wake_reason(void);
t5_wake_reason_t t5_power_wake_reason(void);
const char *t5_wake_reason_name(t5_wake_reason_t r);
/* The soft off reason recorded before the last power cut, if there was one. */
bool t5_power_last_off(t5_off_reason_t *reason);
/* Restore the last app and its state? False after a background alarm wake,
 * a crash, or a cold start. */
bool t5_power_should_restore_session(void);

/* ---- statistics --------------------------------------------------------- */
typedef struct {
    uint32_t state_ms[T5_PWR_STATE_COUNT];  /* time spent in each state */
    uint32_t sleeps;                        /* entries into Sleep */
    uint32_t resumes;                       /* exits from Sleep */
    uint32_t resume_failures;               /* a resume hook failed */
    uint32_t last_resume_ms;                /* wake trigger to backlight on */
    uint32_t max_resume_ms;
    uint32_t last_suspend_ms;
    uint32_t idle_ms;                       /* since the last input */
    uint32_t cpu_mhz;
} t5_power_stats_t;
void t5_power_stats(t5_power_stats_t *out);

/* One step of the state machine (the service task calls it; host tests too). */
void t5_power_tick(void);

#ifdef __cplusplus
}
#endif
