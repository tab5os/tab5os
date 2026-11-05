/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include "t5_mocks.h"

int t5_log_host_verbose = 0;

struct t5_mutex { int depth; };
static int64_t s_now_us;
static void *s_ctx;
static mock_task_t s_tasks[32];
static int s_ntasks;
static t5_task_hook_t s_hook;

void mock_time_set_us(int64_t us) { s_now_us = us; }
void mock_time_advance_ms(uint32_t ms) { s_now_us += (int64_t)ms * 1000; }

t5_mutex_t t5_mutex_create(void) { return calloc(1, sizeof(struct t5_mutex)); }
void t5_mutex_delete(t5_mutex_t m) { free(m); }
void t5_mutex_lock(t5_mutex_t m) { m->depth++; }
bool t5_mutex_lock_timeout(t5_mutex_t m, uint32_t ms) { (void)ms; m->depth++; return true; }
void t5_mutex_unlock(t5_mutex_t m) { m->depth--; }

int64_t t5_time_us(void) { return s_now_us; }
void t5_delay_ms(uint32_t ms) { mock_time_advance_ms(ms); }

void *t5_app_ctx_get(void) { return s_ctx; }
void t5_app_ctx_set(void *ctx) { s_ctx = ctx; }
void t5_core_set_task_hook(t5_task_hook_t hook) { s_hook = hook; }

t5_err_t t5_task_create(const char *name, t5_task_fn_t fn, void *arg, uint32_t stack_bytes,
                        int prio, int core, t5_task_t *out)
{
    (void)stack_bytes; (void)prio; (void)core;
    if (!fn || s_ntasks >= 32) return T5_ERR_NO_MEM;
    s_tasks[s_ntasks] = (mock_task_t){ name, fn, arg, s_ctx };
    t5_task_t h = (t5_task_t)&s_tasks[s_ntasks++];
    if (s_hook) s_hook(h, s_ctx);
    if (out) *out = h;
    return T5_OK;
}

int mock_tasks_count(void) { return s_ntasks; }
const mock_task_t *mock_task(int i) { return i < s_ntasks ? &s_tasks[i] : NULL; }
void mock_tasks_reset(void) { s_ntasks = 0; s_hook = NULL; s_ctx = NULL; }
