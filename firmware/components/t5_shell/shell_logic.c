/* SPDX-License-Identifier: Apache-2.0 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "md/md_icons.h"
#include "shell_logic.h"
#include "t5_battery/policy.h"

void shell_recents_note(shell_recents_t *r, const char *id)
{
    int at = SHELL_RECENTS - 1;
    for (int i = 0; i < SHELL_RECENTS; i++) {
        if (!strcmp(r->ids[i], id)) {
            at = i;
            break;
        }
    }
    memmove(&r->ids[1], &r->ids[0], (size_t)at * sizeof r->ids[0]);
    snprintf(r->ids[0], sizeof r->ids[0], "%s", id);
}

int shell_recents_list(const shell_recents_t *r, char ids[][T5_APP_ID_LEN], int max)
{
    int n = 0;
    for (int i = 0; i < SHELL_RECENTS && n < max; i++) {
        if (r->ids[i][0]) memcpy(ids[n++], r->ids[i], T5_APP_ID_LEN);
    }
    return n;
}

const char *shell_app_name(const shell_app_t *a)
{
    return a->name[0] ? a->name : a->id;
}

int shell_app_name_cmp(const void *a, const void *b)
{
    const char *x = shell_app_name(a), *y = shell_app_name(b);
    for (; *x && *y; x++, y++) {
        int d = tolower((unsigned char)*x) - tolower((unsigned char)*y);
        if (d) return d;
    }
    return (unsigned char)*x - (unsigned char)*y;
}

bool shell_app_removable(const shell_app_t *a)
{
    return !a->builtin && !a->running;
}

const char *shell_battery_icon(const shell_status_t *st)
{
    if (st->charging || (st->usb_power && st->battery_pct >= 99)) return MD_ICON_BATTERY_CHARGING_FULL;
    if (st->low_power) return MD_ICON_BATTERY_SAVER;
    if (st->battery_pct >= 0 && st->battery_pct <= T5_DIS_WARN_PCT) return MD_ICON_BATTERY_ALERT;
    if (st->battery_pct >= 95) return MD_ICON_BATTERY_FULL;
    return MD_ICON_BATTERY_STD;
}

void shell_fmt_clock(const struct tm *tm, char *out, size_t n)
{
    snprintf(out, n, "%02d:%02d", tm->tm_hour, tm->tm_min);
}

void shell_fmt_date(const struct tm *tm, char *out, size_t n)
{
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March",     "April",   "May",      "June",
                                          "July",    "August",   "September", "October", "November", "December" };
    snprintf(out, n, "%s, %d %s", days[(unsigned)tm->tm_wday % 7], tm->tm_mday, months[(unsigned)tm->tm_mon % 12]);
}
