/* SPDX-License-Identifier: Apache-2.0
 * The saved accent: one byte, the md_accent_t, in <sys dir>/accent.
 */
#include <stdint.h>
#include <stdio.h>
#include "md/md.h"
#include "t5_core/log.h"
#include "t5_shell/accent.h"
#include "shell_priv.h"
#include "t5_storage/paths.h"
#include "t5_storage/t5_storage.h"

static const char *TAG = "t5.shell";

#define ACCENT_FILE "accent"
#define SECONDARY MD_ACCENT_TEAL

void t5_shell_accent_set(md_accent_t accent)
{
    if ((unsigned)accent >= MD_ACCENT_COUNT) return;
    md_set_accent(accent, SECONDARY);
    launcher_restyle();
    char p[96];
    uint8_t v = (uint8_t)accent;
    if (t5_sys_path(p, sizeof p, ACCENT_FILE) == T5_OK) T5_LOG_ON_ERR(TAG, t5_storage_write_atomic(p, &v, 1));
}

void t5_shell_accent_restore(void)
{
    char p[96];
    if (t5_sys_path(p, sizeof p, ACCENT_FILE) != T5_OK) return;
    FILE *f = fopen(p, "rb");
    if (!f) return;
    int v = fgetc(f);
    fclose(f);
    if (v >= 0 && v < MD_ACCENT_COUNT) md_set_accent((md_accent_t)v, SECONDARY);
    launcher_restyle();   /* the home screen is built before the prefs are applied */
}
