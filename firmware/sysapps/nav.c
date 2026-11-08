/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include "sysapps_priv.h"

static void back(lv_event_t *e)
{
    sa_nav_t *nav = lv_event_get_user_data(e);
    if (nav->depth > 1) {
        sa_pop(nav);
    } else if (nav->root_back) {
        sa_pop(nav);
        nav->root_back();
    } else {
        t5_app_exit(nav->app);
    }
}

static void tick(lv_timer_t *t)
{
    sa_nav_t *nav = lv_timer_get_user_data(t);
    if (nav->depth > 0) {
        sa_level_t *l = &nav->lv[nav->depth - 1];
        if (l->refresh) l->refresh(nav, l->arg);
    }
}

void sa_nav_init(sa_nav_t *nav, t5_app_t *app)
{
    memset(nav, 0, sizeof *nav);
    nav->app = app;
    nav->root = t5_app_screen(app);
    lv_obj_set_scrollable(nav->root, false);
    nav->timer = lv_timer_create(tick, 1000, nav);
}

static void free_level(sa_level_t *l)
{
    if (l->free_arg && l->arg) l->free_arg(l->arg);
    memset(l, 0, sizeof *l);
}

void sa_nav_destroy(sa_nav_t *nav)
{
    if (nav->timer) lv_timer_delete(nav->timer);
    nav->timer = NULL;
    for (int i = 0; i < nav->depth; i++) free_level(&nav->lv[i]);
    nav->depth = 0;
}

lv_obj_t *sa_push(sa_nav_t *nav, const char *title)
{
    if (nav->depth >= SA_MAX_DEPTH) sa_pop(nav);
    if (nav->depth > 0) lv_obj_set_hidden(nav->lv[nav->depth - 1].page, true);
    sa_level_t *l = &nav->lv[nav->depth++];
    l->page = md_page_create(nav->root, title, back, nav, &l->bar);
    l->body = md_page_body(l->page);
    return l->body;
}

void sa_pop(sa_nav_t *nav)
{
    if (nav->depth <= 0) return;
    sa_level_t *l = &nav->lv[--nav->depth];
    /* The level's free function first, while its widgets still exist: the
     * Editor's saves the file from its code view and resets its run icon.
     * Deleting the page first made that a use after free (a crash on Back). */
    lv_obj_t *page = l->page;
    free_level(l);
    lv_obj_delete(page);
    if (nav->depth > 0) {
        sa_level_t *top = &nav->lv[nav->depth - 1];
        lv_obj_set_hidden(top->page, false);
        if (top->refresh) top->refresh(nav, top->arg);
    }
}

void sa_pop_to_root(sa_nav_t *nav)
{
    while (nav->depth > 1) sa_pop(nav);
}

void sa_set_refresh(sa_nav_t *nav, sa_refresh_fn_t fn, void *arg, sa_free_fn_t free_arg)
{
    if (!nav->depth) return;
    sa_level_t *l = &nav->lv[nav->depth - 1];
    l->refresh = fn;
    l->arg = arg;
    l->free_arg = free_arg;
    if (fn) fn(nav, arg);
}

lv_obj_t *sa_bar(sa_nav_t *nav)
{
    return nav->depth ? nav->lv[nav->depth - 1].bar : NULL;
}

lv_obj_t *sa_body(sa_nav_t *nav)
{
    return nav->depth ? nav->lv[nav->depth - 1].body : NULL;
}

void sa_report(t5_err_t e, const char *what)
{
    if (e == T5_OK) return;
    char msg[96];
    snprintf(msg, sizeof msg, "Couldn't %s (%s)", what, t5_err_str(e));
    md_snackbar_show(msg, NULL, NULL, NULL);
}

void sa_fmt_bytes(char *out, size_t n, uint64_t b)
{
    if (b >= (1ull << 30)) snprintf(out, n, "%.1f GB", (double)b / (1ull << 30));
    else if (b >= (1u << 20)) snprintf(out, n, "%.1f MB", (double)b / (1u << 20));
    else if (b >= 1024) snprintf(out, n, "%u KB", (unsigned)(b / 1024));
    else snprintf(out, n, "%u bytes", (unsigned)b);
}

void sa_fmt_duration(char *out, size_t n, uint32_t s)
{
    if (s >= 3600) snprintf(out, n, "%u h %u min", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
    else if (s >= 60) snprintf(out, n, "%u min", (unsigned)(s / 60));
    else snprintf(out, n, "%u s", (unsigned)s);
}

void sa_set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

lv_obj_t *sa_note(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = md_label_create(parent, text, MD_TYPE_BODY2);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_pad_hor(l, T5_DP(16), 0);
    lv_obj_set_style_pad_ver(l, T5_DP(8), 0);
    lv_obj_set_style_text_opa(l, md_scheme()->on_surface.medium, 0);
    return l;
}

lv_obj_t *sa_button_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(r, T5_DP(16), 0);
    lv_obj_set_style_pad_ver(r, T5_DP(8), 0);
    lv_obj_set_style_pad_gap(r, T5_DP(8), 0);
    lv_obj_set_scrollable(r, false);
    return r;
}
