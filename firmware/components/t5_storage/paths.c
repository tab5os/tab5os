/* SPDX-License-Identifier: Apache-2.0 */
#include "t5_storage/paths.h"

#include <stdio.h>
#include <string.h>

static char s_sys[64] = T5_SYS_DIR;

const char *t5_sys_dir(void)
{
    return s_sys;
}

void t5_sys_dir_set(const char *dir)
{
    strncpy(s_sys, dir, sizeof s_sys - 1);
    s_sys[sizeof s_sys - 1] = '\0';
}

t5_err_t t5_sys_path(char *out, size_t cap, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", s_sys, name);
    return n >= 0 && (size_t)n < cap ? T5_OK : T5_ERR_INVALID_ARG;
}
