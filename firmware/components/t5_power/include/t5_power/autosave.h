/* SPDX-License-Identifier: Apache-2.0
 * Autosave (plan 2.12). The power button can cut power with no warning
 * (bench 0.6), so anything worth keeping is written atomically at least every
 * T5_AUTOSAVE_PERIOD_MS and on every power state change.
 *
 * Savers decide for themselves whether anything changed; most keep a copy of
 * what they last wrote. The session blob is the shell's "last app and its
 * state", restored after a soft off (Phase 6.10 fills it in).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define T5_AUTOSAVE_PERIOD_MS 30000
#define T5_AUTOSAVE_MAX 12
#define T5_SESSION_MAX 4096
#define T5_SYS_DIR "/data/sys"

typedef t5_err_t (*t5_autosave_fn_t)(void *arg);

t5_err_t t5_autosave_register(const char *name, t5_autosave_fn_t fn, void *arg);
/* Run every saver now. Returns the first error; all savers still run. */
t5_err_t t5_autosave_now(void);
/* Periodic check (the power task calls it). */
void t5_autosave_poll(uint32_t now_ms);
uint32_t t5_autosave_runs(void);
uint32_t t5_autosave_failures(void);

/* Session blob: set marks it dirty, the next autosave writes it. */
t5_err_t t5_session_set(const void *data, size_t len);
/* What was saved before the last power-off (loaded at init). */
t5_err_t t5_session_get(void *buf, size_t cap, size_t *len);
/* Load the session from storage (power init calls it). */
t5_err_t t5_session_load(void);

/* Where the session and other system records live. Host tests point this
 * at a temporary directory. */
void t5_autosave_set_dir(const char *dir);
const char *t5_autosave_dir(void);

#ifdef __cplusplus
}
#endif
