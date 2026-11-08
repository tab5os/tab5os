/* SPDX-License-Identifier: Apache-2.0
 * Crash guard (plan 5.11). Native apps share the OS address space. Most
 * app faults are now survived without a reboot (t5_app/isolate.h) and
 * recorded here directly; for the rest, which still reboot the device,
 * before running app code the OS writes
 * the app id into RTC no-init memory (survives a panic reset, not a power
 * loss); at boot, a crash-type wake with an id there means that app crashed.
 *
 * Records per app (in /data/sys/crashes.bin): consecutive crashes since the
 * app last exited cleanly, total crashes, the time and kind of the last one,
 * and the saved core dump. After two consecutive crashes the app is not
 * auto-launched (session restore, scheduled alarms); a person can still
 * open it.
 *
 * The decision logic and the record file are pure (host-tested); RTC memory,
 * the reset reason and the core dump partition are in apps_hw.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_CRASH_MAX_APPS 16
#define T5_CRASH_BLOCK_AFTER 2
#define T5_CRASH_ID_LEN 64

typedef enum {
    T5_CRASH_NONE,
    T5_CRASH_PANIC,       /* panic or watchdog reset while the app ran */
    T5_CRASH_ANR,         /* UI watchdog: the person chose to close the app */
    T5_CRASH_ALARM_OVERRUN, /* headless on_alarm ran past max_seconds */
    T5_CRASH_OOM,         /* closed by the OS: memory critically low */
} t5_crash_kind_t;

typedef struct {
    char id[T5_CRASH_ID_LEN];
    uint16_t consecutive;
    uint16_t total;
    uint8_t last_kind;              /* t5_crash_kind_t */
    uint8_t has_dump;
    uint16_t pad;
    int64_t last_time;              /* wall clock, 0 if unknown */
} t5_crash_rec_t;

typedef struct {
    t5_crash_rec_t rec[T5_CRASH_MAX_APPS];
    int n;
} t5_crash_db_t;

/* What RTC memory held at boot (filled by the hw layer). */
typedef struct {
    bool valid;
    char running[T5_CRASH_ID_LEN];  /* "" if no app code was running */
    bool headless;
    uint8_t pending_kind;           /* set before a deliberate restart (ANR) */
} t5_crash_boot_t;

/* Boot decision: returns the crash kind to record for boot->running (or
 * NONE). crash_reset: the power service classified the boot as a crash. */
t5_crash_kind_t t5_crash_classify(const t5_crash_boot_t *boot, bool crash_reset);

t5_crash_rec_t *t5_crash_find(t5_crash_db_t *db, const char *id);
/* Count a crash; evicts the oldest record if the table is full. */
t5_crash_rec_t *t5_crash_record(t5_crash_db_t *db, const char *id, t5_crash_kind_t kind, int64_t now);
/* The app exited cleanly: consecutive count back to 0. */
void t5_crash_clean_exit(t5_crash_db_t *db, const char *id);
bool t5_crash_autolaunch_allowed(const t5_crash_db_t *db, const char *id);
void t5_crash_forget(t5_crash_db_t *db, const char *id);
const char *t5_crash_kind_name(t5_crash_kind_t k);

T5_MUST_CHECK t5_err_t t5_crash_db_load(t5_crash_db_t *db, const char *path);
T5_MUST_CHECK t5_err_t t5_crash_db_save(const t5_crash_db_t *db, const char *path);

#ifdef __cplusplus
}
#endif
