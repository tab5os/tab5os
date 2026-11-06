/* SPDX-License-Identifier: Apache-2.0
 * The stable app ABI (plan 5.5, 5.6).
 *
 * sdk/abi/api_v<N>.sym lists every symbol an app may import, with its full
 * prototype. tools/abi/abi.py turns it into abi_table.c at build time: a
 * sorted table of names and addresses, plus a redeclaration of every
 * prototype so the build fails if a header drifts from the published ABI.
 * CI (abi.py check) fails if a symbol of a published level is removed or
 * its prototype changes.
 *
 * Some symbols are bound to tracking wrappers (malloc, xTaskCreate,
 * lv_timer_create...), and some need a permission group: an app whose
 * manifest doesn't declare it gets a stub that returns T5_ERR_DENIED (or
 * NULL, false, 0) instead. That is consent and correctness, not security:
 * native code can still reach hardware directly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    T5_ABI_RET_VOID,
    T5_ABI_RET_ERR,      /* t5_err_t or int: denied stub returns T5_ERR_DENIED */
    T5_ABI_RET_PTR,      /* NULL */
    T5_ABI_RET_BOOL,     /* false */
    T5_ABI_RET_ZERO,     /* other scalars: 0 */
} t5_abi_ret_t;

typedef struct {
    const char *name;
    const void *addr;
    uint16_t perm;       /* T5_PERM_* bit needed, 0 = none */
    uint8_t ret;         /* t5_abi_ret_t */
    uint8_t level;       /* API level that added it */
} t5_abi_sym_t;

extern const t5_abi_sym_t t5_abi_table[];
extern const size_t t5_abi_count;

const t5_abi_sym_t *t5_abi_find(const char *name);
/* Compiler runtime helpers (__divdi3, __errno...) come from the loader's
 * libc table rather than ours. */
bool t5_abi_is_runtime_helper(const char *name);
/* The address an app with the given declared permissions gets for name, or
 * 0 if the name isn't exported. *denied says a stub was substituted. */
uintptr_t t5_abi_resolve(const char *name, uint32_t declared_perms, bool *denied);
/* Stub addresses (for tests). */
const void *t5_abi_stub(t5_abi_ret_t ret);

#ifdef __cplusplus
}
#endif
