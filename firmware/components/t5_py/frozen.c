/* SPDX-License-Identifier: Apache-2.0
 * Frozen-module lookup for a string-only build (replaces py/frozenmod.c,
 * see CMakeLists.txt). Upstream counts its string entries only when .mpy
 * freezing is on too, so with strings alone nothing frozen would ever load.
 * Same table layout as tools/py/freeze_str.py writes: NUL-separated names
 * ending in an empty one, their sizes, then the sources.
 */
#include <string.h>
#include "py/frozenmod.h"
#include "py/lexer.h"
#include "py/qstr.h"

extern const char mp_frozen_names[];
extern const uint32_t mp_frozen_str_sizes[];
extern const char mp_frozen_str_content[];

mp_import_stat_t mp_find_frozen_module(const char *str, int *frozen_type, void **data)
{
    size_t len = strlen(str);
    const char *name = mp_frozen_names;
    size_t offset = 0;
    if (frozen_type) *frozen_type = MP_FROZEN_NONE;
    for (size_t i = 0; *name; i++) {
        size_t n = strlen(name);
        if (n >= len && memcmp(str, name, len) == 0) {
            if (n == len) {
                if (frozen_type) {
                    *frozen_type = MP_FROZEN_STR;
                    *data = mp_lexer_new_from_str_len(qstr_from_strn(str, len), mp_frozen_str_content + offset,
                                                      mp_frozen_str_sizes[i], 0);
                }
                return MP_IMPORT_STAT_FILE;
            }
            if (name[len] == '/') return MP_IMPORT_STAT_DIR;
        }
        offset += mp_frozen_str_sizes[i] + 1;
        name += n + 1;
    }
    return MP_IMPORT_STAT_NO_EXIST;
}
