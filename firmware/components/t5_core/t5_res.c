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

uint32_t t5_core_cap_group(uint32_t cap)
{
    switch (cap) {
    case T5_CORE_CAP_USB_DEVICE:
    case T5_CORE_CAP_USB_TAKEOVER:
    case T5_CORE_CAP_EXCL_USBC:
    case T5_CORE_CAP_EXCL_USBA: return T5_CORE_PERM_USB;
    case T5_CORE_CAP_BLE_HID:
    case T5_CORE_CAP_EXCL_BT: return T5_CORE_PERM_BLE;
    case T5_CORE_CAP_WIFI_MONITOR:
    case T5_CORE_CAP_WIFI_TX:
    case T5_CORE_CAP_EXCL_WIFI: return T5_CORE_PERM_NET;
    default: return 0;
    }
}

static t5_fg_check_fn_t s_fg;

void t5_core_set_fg_check(t5_fg_check_fn_t fn)
{
    s_fg = fn;
}

bool t5_core_app_foreground(void)
{
    return s_fg ? s_fg() : true;
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
