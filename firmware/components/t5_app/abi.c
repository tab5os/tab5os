/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_app/abi.h"
#include "t5_core/err.h"
#include "t5_core/log.h"

static const char *TAG = "t5.abi";

static int stub_err(void) { T5_LOGW(TAG, "denied call"); return T5_ERR_DENIED; }
static void *stub_ptr(void) { T5_LOGW(TAG, "denied call"); return NULL; }
static bool stub_bool(void) { T5_LOGW(TAG, "denied call"); return false; }
static void stub_void(void) { T5_LOGW(TAG, "denied call"); }
static int stub_zero(void) { T5_LOGW(TAG, "denied call"); return 0; }

const void *t5_abi_stub(t5_abi_ret_t r)
{
    switch (r) {
    case T5_ABI_RET_ERR: return (const void *)stub_err;
    case T5_ABI_RET_PTR: return (const void *)stub_ptr;
    case T5_ABI_RET_BOOL: return (const void *)stub_bool;
    case T5_ABI_RET_VOID: return (const void *)stub_void;
    default: return (const void *)stub_zero;
    }
}

const t5_abi_sym_t *t5_abi_find(const char *name)
{
    if (!name) return NULL;
    size_t lo = 0, hi = t5_abi_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(name, t5_abi_table[mid].name);
        if (c == 0) return &t5_abi_table[mid];
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

bool t5_abi_is_runtime_helper(const char *name)
{
    /* The libgcc helpers apps need are published in api_v1.sym like any
     * other symbol, so install and load agree: an app that imports a helper
     * the OS doesn't export is refused at install, not at launch. */
    (void)name;
    return false;
}

uintptr_t t5_abi_resolve(const char *name, uint32_t perms, bool *denied)
{
    if (denied) *denied = false;
    const t5_abi_sym_t *s = t5_abi_find(name);
    if (!s) return 0;
    if (s->perm && !(perms & s->perm)) {
        if (denied) *denied = true;
        return (uintptr_t)t5_abi_stub((t5_abi_ret_t)s->ret);
    }
    return (uintptr_t)s->addr;
}
