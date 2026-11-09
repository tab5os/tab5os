/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "t5_app/abi.h"
#include "t5_core/err.h"
#include "t5_core/log.h"

static const char *TAG = "t5.abi";

static const t5_abi_sym_t *s_table;
static size_t s_count;

void t5_abi_set_table(const t5_abi_sym_t *table, size_t count)
{
    s_table = table;
    s_count = table ? count : 0;
}

size_t t5_abi_size(void)
{
    return s_count;
}

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
    size_t lo = 0, hi = s_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = strcmp(name, s_table[mid].name);
        if (c == 0) return &s_table[mid];
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
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
