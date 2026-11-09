/* SPDX-License-Identifier: Apache-2.0
 * Autosave (plan 2.12). The power button can cut power with no warning
 * (bench 0.6), so anything worth keeping is written atomically at least every
 * T5_AUTOSAVE_PERIOD_MS and on every power state change.
 *
 * Savers decide for themselves whether anything changed; most keep a copy of
 * what they last wrote.
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

typedef t5_err_t (*t5_autosave_fn_t)(void *arg);

T5_MUST_CHECK t5_err_t t5_autosave_register(const char *name, t5_autosave_fn_t fn, void *arg);
/* Run every saver now. Returns the first error; all savers still run. */
T5_MUST_CHECK t5_err_t t5_autosave_now(void);
/* Periodic check (the power task calls it). */
void t5_autosave_poll(uint32_t now_ms);
uint32_t t5_autosave_runs(void);
uint32_t t5_autosave_failures(void);

#ifdef __cplusplus
}
#endif
