/* SPDX-License-Identifier: Apache-2.0
 * App resource tracking hooks (plan 5.4).
 *
 * Services that hand out something an app must give back (a port claim, a
 * rail hold, a wake lock, a task, an LVGL timer...) report it here when the
 * caller is an app (t5_app_ctx_get() != NULL), and report the release. The
 * app runtime (t5_app) installs the hooks and frees whatever is left, in a
 * fixed order, when the app exits or crashes.
 *
 * Without hooks installed (host tests, early boot) these do nothing, so a
 * service never depends on t5_app.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Release order on app exit is the order of this enum: tasks stop first so
 * nothing re-acquires what is being freed, memory goes last. */
typedef enum {
    T5_RES_TASK,
    T5_RES_TIMER,      /* lv_timer_t */
    T5_RES_ANIM,       /* lv_anim_t variable */
    T5_RES_UI,         /* LVGL root objects */
    T5_RES_PORT,
    T5_RES_RAIL,
    T5_RES_LOCK,
    T5_RES_OTHER,
    T5_RES_KIND_COUNT,
} t5_res_kind_t;

typedef void (*t5_res_release_fn_t)(void *handle);

typedef struct {
    void (*add)(void *app, t5_res_kind_t kind, void *handle, t5_res_release_fn_t release);
    void (*remove)(void *app, void *handle);
} t5_res_hooks_t;

/* Installed once by the app runtime. NULL removes them. */
void t5_res_set_hooks(const t5_res_hooks_t *hooks);

/* Record handle for the calling task's app. No-op for system callers. */
void t5_res_add(t5_res_kind_t kind, void *handle, t5_res_release_fn_t release);
/* Forget handle (it was released normally). Safe for unknown handles. */
void t5_res_remove(void *handle);
/* Same, for a handle released by someone other than its app (a timeout). */
void t5_res_remove_app(void *app, void *handle);

/* Every app context starts with this header, so a service can name the
 * owner of a claim without depending on t5_app. */
typedef struct {
    uint32_t magic;          /* T5_APP_CTX_MAGIC */
    const char *id;          /* "com.example.app" */
} t5_app_ctx_hdr_t;

#define T5_APP_CTX_MAGIC 0x54354150u   /* "T5AP" */

/* Owner name for the calling task: the app id, or "system". */
const char *t5_app_ctx_id(void *ctx);
const char *t5_caller_id(void);

#ifdef __cplusplus
}
#endif
