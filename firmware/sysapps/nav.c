/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include <string.h>
#include "sysapps_priv.h"

static void back(t5ui_event_t *e)
{
    sa_nav_t *nav = t5ui_event_user(e);
    if (nav->depth > 1) {
        sa_pop(nav);
    } else if (nav->root_back) {
        sa_pop(nav);
        nav->root_back();
    } else {
        t5_app_exit(nav->app);
    }
}

static void tick(t5ui_timer_t *t, void *user)
{
    (void)t;
    sa_nav_t *nav = user;
    if (nav->depth > 0) {
        sa_level_t *l = &nav->lv[nav->depth - 1];
        if (l->refresh) l->refresh(nav, l->arg);
    }
}

void sa_nav_init(sa_nav_t *nav, t5_app_t *app)
{
    memset(nav, 0, sizeof *nav);
    nav->app = app;
    nav->root = t5ui_root(app);
    t5ui_set_int(nav->root, T5UI_P_SCROLLABLE, false);
    nav->timer = t5ui_timer(1000, tick, nav);
}

static void free_level(sa_level_t *l)
{
    if (l->free_arg && l->arg) l->free_arg(l->arg);
    memset(l, 0, sizeof *l);
}

void sa_nav_destroy(sa_nav_t *nav)
{
    if (nav->timer) t5ui_timer_delete(nav->timer);
    nav->timer = NULL;
    for (int i = 0; i < nav->depth; i++) free_level(&nav->lv[i]);
    nav->depth = 0;
}

t5ui_obj_t *sa_push(sa_nav_t *nav, const char *title)
{
    if (nav->depth >= SA_MAX_DEPTH) sa_pop(nav);
    if (nav->depth > 0) t5ui_set_int(nav->lv[nav->depth - 1].page, T5UI_P_HIDDEN, true);
    sa_level_t *l = &nav->lv[nav->depth++];
    l->page = t5ui_create(nav->root, T5UI_MD_PAGE);
    t5ui_set_str(l->page, T5UI_P_TITLE, title);
    t5ui_on(l->page, T5UI_EV_BACK, back, nav);
    l->body = t5ui_get_obj(l->page, T5UI_P_CONTENT);
    return l->body;
}

void sa_pop(sa_nav_t *nav)
{
    if (nav->depth <= 0) return;
    sa_level_t *l = &nav->lv[--nav->depth];
    /* The level's free function first, while its widgets still exist: the
     * Editor's saves the file from its code view and resets its run icon.
     * Deleting the page first made that a use after free (a crash on Back). */
    t5ui_obj_t *page = l->page;
    free_level(l);
    t5ui_delete(page);
    if (nav->depth > 0) {
        sa_level_t *top = &nav->lv[nav->depth - 1];
        t5ui_set_int(top->page, T5UI_P_HIDDEN, false);
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

t5ui_obj_t *sa_bar(sa_nav_t *nav)
{
    return nav->depth ? nav->lv[nav->depth - 1].page : NULL;
}

t5ui_obj_t *sa_body(sa_nav_t *nav)
{
    return nav->depth ? nav->lv[nav->depth - 1].body : NULL;
}

void sa_report(t5_err_t e, const char *what)
{
    if (e == T5_OK) return;
    char msg[96];
    snprintf(msg, sizeof msg, "Couldn't %s (%s)", what, t5_err_str(e));
    t5ui_snackbar(msg, NULL, NULL, NULL);
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
    if (s >= 3600 && s % 3600 < 60) snprintf(out, n, "%u h", (unsigned)(s / 3600));
    else if (s >= 3600) snprintf(out, n, "%u h %u min", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
    else if (s >= 60) snprintf(out, n, "%u min", (unsigned)(s / 60));
    else snprintf(out, n, "%u s", (unsigned)s);
}

void sa_set_text(t5ui_obj_t *obj, const char *t)
{
    if (obj && strcmp(t5ui_get_str(obj, T5UI_P_TEXT, ""), t)) t5ui_set_str(obj, T5UI_P_TEXT, t);
}

t5ui_obj_t *sa_note(t5ui_obj_t *parent, const char *text)
{
    t5ui_obj_t *l = t5ui_create(parent, T5UI_MD_TEXT);
    t5ui_set_int(l, T5UI_P_TEXT_STYLE, T5UI_TEXT_BODY2);
    t5ui_set_str(l, T5UI_P_TEXT, text);
    t5ui_set_int(l, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(l, T5UI_P_LONG_MODE, T5UI_LONG_WRAP);
    t5ui_set_int(l, T5UI_P_PAD_LEFT, t5ui_dp(16));
    t5ui_set_int(l, T5UI_P_PAD_RIGHT, t5ui_dp(16));
    t5ui_set_int(l, T5UI_P_PAD_TOP, t5ui_dp(8));
    t5ui_set_int(l, T5UI_P_PAD_BOTTOM, t5ui_dp(8));
    t5ui_set_int(l, T5UI_P_TEXT_OPA, t5ui_theme_emphasis(T5UI_EMPHASIS_MEDIUM));
    return l;
}

t5ui_obj_t *sa_button_row(t5ui_obj_t *parent)
{
    t5ui_obj_t *r = t5ui_create(parent, T5UI_BOX);
    t5ui_set_int(r, T5UI_P_WIDTH, T5UI_PCT(100));
    t5ui_set_int(r, T5UI_P_FLOW, T5UI_ROW_WRAP);
    t5ui_set_int(r, T5UI_P_PLACE_CROSS, T5UI_PLACE_CENTER);
    t5ui_set_int(r, T5UI_P_PLACE_TRACK, T5UI_PLACE_CENTER);
    t5ui_set_int(r, T5UI_P_PAD_LEFT, t5ui_dp(16));
    t5ui_set_int(r, T5UI_P_PAD_RIGHT, t5ui_dp(16));
    t5ui_set_int(r, T5UI_P_PAD_TOP, t5ui_dp(8));
    t5ui_set_int(r, T5UI_P_PAD_BOTTOM, t5ui_dp(8));
    t5ui_set_int(r, T5UI_P_PAD_ROW, t5ui_dp(8));
    t5ui_set_int(r, T5UI_P_PAD_COLUMN, t5ui_dp(8));
    t5ui_set_int(r, T5UI_P_SCROLLABLE, false);
    return r;
}
