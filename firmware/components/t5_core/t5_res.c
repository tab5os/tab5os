/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>
#include "t5_core/os.h"
#include "t5_core/res.h"

static const t5_res_hooks_t *s_hooks;

const char *t5_res_kind_name(t5_res_kind_t kind)
{
    switch (kind) {
    case T5_RES_TASK: return "tasks";
    case T5_RES_TIMER: return "timers";
    case T5_RES_ANIM: return "anims";
    case T5_RES_UI: return "ui";
    case T5_RES_PORT: return "ports";
    case T5_RES_RAIL: return "rails";
    case T5_RES_LOCK: return "locks";
    case T5_RES_OTHER: return "other";
    default: return "?";
    }
}

void t5_res_set_hooks(const t5_res_hooks_t *hooks)
{
    s_hooks = hooks;
}

void t5_res_add(t5_res_kind_t kind, void *handle, t5_res_release_fn_t release)
{
    void *app = t5_app_ctx_get();
    if (app && handle && s_hooks && s_hooks->add) {
        s_hooks->add(app, kind, handle, release);
    }
}

void t5_res_remove_app(void *app, void *handle)
{
    if (app && handle && s_hooks && s_hooks->remove) {
        s_hooks->remove(app, handle);
    }
}

void t5_res_remove(void *handle)
{
    t5_res_remove_app(t5_app_ctx_get(), handle);
}

const char *t5_app_ctx_id(void *ctx)
{
    const t5_app_ctx_hdr_t *h = ctx;
    if (!h) return "system";
    return (h->magic == T5_APP_CTX_MAGIC && h->id) ? h->id : "app";
}

const char *t5_caller_id(void)
{
    return t5_app_ctx_id(t5_app_ctx_get());
}

static t5_perm_check_fn_t s_perm;

void t5_core_set_perm_check(t5_perm_check_fn_t fn)
{
    s_perm = fn;
}

int t5_core_perm_check(uint32_t perm)
{
    return s_perm ? s_perm(perm) : 0;
}

static t5_path_check_fn_t s_path;

void t5_core_set_path_check(t5_path_check_fn_t fn)
{
    s_path = fn;
}

bool t5_core_path_check(const char *path, bool write)
{
    return s_path ? s_path(path, write) : true;
}
