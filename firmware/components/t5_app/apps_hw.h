/* SPDX-License-Identifier: Apache-2.0
 * Hardware side of the app runtime: RTC no-init memory, core dumps, the
 * ELF loader, restarts. Everything else in t5_app is logic over this.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_app/app.h"
#include "t5_app/crashguard.h"
#include "t5_app/tracker.h"
#include "t5_core/err.h"

typedef struct t5_app t5_app_t;

/* Crash guard record in RTC no-init memory (survives a panic reset). */
void apps_hw_guard_read(t5_crash_boot_t *out);
void apps_hw_guard_set(const char *id, bool headless);
void apps_hw_guard_pending(uint8_t kind);   /* t5_crash_kind_t to record after a deliberate restart */
void apps_hw_guard_clear(void);

/* Copy the core dump in flash to path (and erase it). False if none. */
bool apps_hw_coredump_save(const char *path);

/* Load a native app: relocate into PSRAM, binding imports through the ABI
 * table with app's permissions; then call its entry for the descriptor. */
t5_err_t apps_hw_elf_load(t5_app_t *app, const char *path, void **handle, const t5_app_desc_t **desc);
void apps_hw_elf_unload(void *handle);

/* The allocator under app heaps (tracker.h): placement by isolate.h. */
void apps_hw_heap_install(void);
size_t apps_hw_free_psram(void);
size_t apps_hw_free_l2(void);
void apps_hw_reserve_take(void);
void apps_hw_reserve_release(void);
/* Does p lie in this app's heap (live block or not)? */
bool apps_hw_heap_contains(const t5_tracker_t *t, const void *p);

/* Run time of a task so far (us), for the CPU hog check. */
uint64_t apps_hw_task_runtime_us(void *task);
void apps_hw_task_set_prio(void *task, int prio);
void apps_hw_task_pause(void *task, bool pause);

/* app_held(task): how many of the app's own mutexes task holds, or -1.
 * park: suspend task where it holds no OS mutex; false (and left running)
 * if it holds one now. delete: stop it where it is and delete it (the exit
 * parks every task first, lifecycle.c quiesce). */
bool apps_hw_task_park(void *task, int (*app_held)(void *task));
void apps_hw_task_delete(void *task);
void apps_hw_restart(void);
void apps_hw_periodic(void (*fn)(void), uint32_t period_ms);
