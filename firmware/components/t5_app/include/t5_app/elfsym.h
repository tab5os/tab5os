/* SPDX-License-Identifier: Apache-2.0
 * Undefined symbols of an app ELF (plan 5.6: install-time ABI check).
 * Pure ELF32 parsing; walks .dynsym (and .symtab if present) and reports
 * every undefined, non-local symbol name once.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "t5_core/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Calls fn for each undefined symbol; weak ones get weak = true. Stops if
 * fn returns false. Returns T5_ERR_INVALID_ARG for anything that isn't a
 * well-formed little-endian ELF32 RISC-V file. */
typedef bool (*t5_elfsym_fn_t)(const char *name, bool weak, void *arg);
t5_err_t t5_elfsym_undefined(const uint8_t *elf, size_t len, t5_elfsym_fn_t fn, void *arg);
/* True if the ELF defines a global function symbol called name. */
bool t5_elfsym_defines(const uint8_t *elf, size_t len, const char *name);

#ifdef __cplusplus
}
#endif
