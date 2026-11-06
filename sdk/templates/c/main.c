/* A minimal Tab5 OS app: an app bar and a button that counts taps. */
#include <stdio.h>
#include <stdlib.h>
#include <t5/app.h>
#include <t5/ui.h>

typedef struct {
    lv_obj_t *label;
    int taps;
} state_t;

static void tapped(lv_event_t *e)
{
    state_t *s = lv_event_get_user_data(e);
    lv_label_set_text_fmt(s->label, "Tapped %d times", ++s->taps);
}

static void back(lv_event_t *e)
{
    t5_app_exit(t5_app_self());
}

static void on_create(t5_app_t *app)
{
    state_t *s = calloc(1, sizeof *s);      /* freed by the OS if we forget */
    t5_app_set_user(app, s);
    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    md_appbar_create(scr, "Hello", MD_ICON_ARROW_BACK, back, NULL);
    s->label = md_label_create(scr, "Hello, Tab5", MD_TYPE_HEADLINE5);
    lv_obj_set_style_pad_all(s->label, T5_DP(16), 0);
    lv_obj_t *b = md_button_create(scr, "Tap me", MD_BUTTON_CONTAINED);
    lv_obj_set_style_margin_left(b, T5_DP(16), 0);
    lv_obj_add_event_cb(b, tapped, LV_EVENT_CLICKED, s);
}

static void on_destroy(t5_app_t *app)
{
    free(t5_app_get_user(app));
}

T5_APP(.on_create = on_create, .on_destroy = on_destroy);
