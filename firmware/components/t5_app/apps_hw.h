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

void apps_hw_task_delete(void *task);
void apps_hw_restart(void);
size_t apps_hw_free_heap(void);
