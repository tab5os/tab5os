/* SPDX-License-Identifier: Apache-2.0
 * The shell's decisions that need no LVGL (host-tested in test_ui.c).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "shell_sys.h"

/* Recently launched apps, newest first. */
#define SHELL_RECENTS 6
typedef struct {
    char ids[SHELL_RECENTS][T5_APP_ID_LEN];
} shell_recents_t;
/* id goes to the front; if it was listed it moves, else the oldest drops off. */
void shell_recents_note(shell_recents_t *r, const char *id);
int shell_recents_list(const shell_recents_t *r, char ids[][T5_APP_ID_LEN], int max);

/* The name to show: the app's name, or its id when it has none. */
const char *shell_app_name(const shell_app_t *a);
/* qsort order for the launcher: by shown name, A to Z, ignoring case. */
int shell_app_name_cmp(const void *a, const void *b);
/* Installed apps can be removed, while they aren't running. */
bool shell_app_removable(const shell_app_t *a);
/* The status bar's battery glyph (MD_ICON_BATTERY_*). */
const char *shell_battery_icon(const shell_status_t *st);

/* "14:30", and "Sunday, 5 November". */
void shell_fmt_clock(const struct tm *tm, char *out, size_t n);
void shell_fmt_date(const struct tm *tm, char *out, size_t n);
/* What the clock and the date show while the time isn't set. */
#define SHELL_CLOCK_NOT_SET "--:--"
#define SHELL_DATE_NOT_SET "Time not set"
