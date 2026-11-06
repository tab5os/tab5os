/* SPDX-License-Identifier: Apache-2.0
 * Minimal launcher: the list of built-in and installed apps, tap to open.
 * It is where an app exits to and where the crash guard returns. The real
 * launcher, status bar and app switching are Phase 6.3.
 */
#include <stdio.h>
#include <stdlib.h>
#include "md/md.h"
#include "md/md_icons.h"
#include "t5_ui/t5_ui.h"
#include "app_priv.h"

#define MAX_LIST 48

static lv_obj_t *s_scr, *s_list, *s_empty;
static t5_app_entry_t *s_apps;
static int s_n;

static void bind(md_list_row_t *r, uint32_t i, void *ud)
{
    (void)ud;
    if ((int)i >= s_n) return;
    const t5_app_entry_t *e = &s_apps[i];
    lv_label_set_text(r->primary, e->name[0] ? e->name : e->id);
    if (r->secondary) {
        if (e->autolaunch_blocked) {
            lv_label_set_text_fmt(r->secondary, "%s  ·  crashed %u times in a row", e->version, e->crashes);
        } else {
            lv_label_set_text_fmt(r->secondary, "%s  ·  %s", e->version, e->id);
        }
    }
    lv_label_set_text(r->icon, e->builtin ? MD_ICON_SETTINGS : (e->autolaunch_blocked ? MD_ICON_WARNING : MD_ICON_APPS));
}

static void clicked(uint32_t i, void *ud)
{
    (void)ud;
    if ((int)i < s_n) t5_apps_launch(s_apps[i].id);
}

static void reload(void)
{
    if (!s_apps) s_apps = calloc(MAX_LIST, sizeof *s_apps);
    s_n = s_apps ? t5_apps_list(s_apps, MAX_LIST) : 0;
    if (s_list) md_list_set_count(s_list, (uint32_t)s_n);
    if (s_empty) lv_obj_set_hidden(s_empty, s_n > 0);
}

lv_obj_t *launcher_screen(void)
{
    if (s_scr) return s_scr;
    s_scr = lv_obj_create(NULL);
    lv_obj_set_flex_flow(s_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(s_scr, false);
    md_appbar_create(s_scr, "Tab5 OS", NULL, NULL, NULL);
    reload();
    s_list = md_list_create(s_scr, (uint32_t)s_n, MD_LIST_ROW_2_DP, bind, clicked, NULL);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    s_empty = md_label_create(s_scr, "No apps yet. Install a .t5app with `app install <path>`.", MD_TYPE_BODY1);
    lv_obj_set_style_text_opa(s_empty, md_scheme()->on_surface.medium, 0);
    lv_obj_set_style_pad_all(s_empty, T5_DP(16), 0);
    lv_obj_set_hidden(s_empty, s_n > 0);
    return s_scr;
}

static void refresh_async(void *p)
{
    (void)p;
    reload();
}

void launcher_refresh(void)
{
    if (!s_scr) return;
    if (t5_ui_on_ui_task()) reload();
    else t5_ui_async(refresh_async, NULL);
}
