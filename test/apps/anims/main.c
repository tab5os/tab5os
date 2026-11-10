/* Finished animations must not stay counted against the app's tracked
 * handles. Starts 900 short animations in the ways apps do (same object and
 * exec_cb over and over, a custom exec_cb with no var, var pointing at the
 * lv_anim_t itself) next to one that never ends, then checks the long one
 * still runs and files still open. app_checks.py reads `app usage` after
 * "anims: done" and wants the anims count back down to 1. */
#include <stdio.h>
#include <t5/app.h>
#include <t5/ui.h>

#define ROUNDS 300

static t5_app_t *s_app;
static lv_obj_t *s_label;
static int s_round;
static int s_custom_runs;

static void fade(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void move(void *obj, int32_t v)
{
    lv_obj_set_style_translate_x(obj, v, 0);
}

static void custom(lv_anim_t *a, int32_t v)
{
    (void)a;
    (void)v;
    s_custom_runs++;
}

static void start_short(void *var, lv_anim_exec_xcb_t exec, lv_anim_custom_exec_cb_t cexec, bool self)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, self ? &a : var);
    if (exec) lv_anim_set_exec_cb(&a, exec);
    if (cexec) lv_anim_set_custom_exec_cb(&a, cexec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 5);
    lv_anim_start(&a);
}

static void check(lv_timer_t *t)
{
    lv_timer_delete(t);
    bool long_alive = lv_anim_get(s_label, move) != NULL;
    printf("anims: long-running animation kept -> %s\n", long_alive ? "PASS" : "FAIL");

    /* Before the fix the table was full of finished animations by now, and
     * tracked handles like files could not be added. */
    int opened = 0;
    char path[200];
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof path, "%s/f%d.txt", t5_app_data_dir(s_app), i);
        FILE *f = fopen(path, "w");
        if (!f) break;
        opened++;
        fclose(f);
    }
    printf("anims: files opened after %d animations %d/8 -> %s\n", ROUNDS * 3, opened, opened == 8 ? "PASS" : "FAIL");
    printf("anims: custom exec ran %d times -> %s\n", s_custom_runs, s_custom_runs > 0 ? "PASS" : "FAIL");
    printf("anims: done\n");
}

static void round_cb(lv_timer_t *t)
{
    start_short(s_label, fade, NULL, false);   /* same var and exec every time */
    start_short(NULL, NULL, custom, false);    /* custom exec, no var */
    start_short(NULL, NULL, custom, true);     /* var is the descriptor itself */
    if (++s_round < ROUNDS) return;
    lv_timer_delete(t);
    lv_timer_create(check, 300, NULL);         /* let the last ones finish */
}

static void on_create(t5_app_t *app)
{
    s_app = app;
    s_label = md_label_create(t5_app_screen(app), "anims", MD_TYPE_HEADLINE5);
    lv_obj_center(s_label);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_label);
    lv_anim_set_exec_cb(&a, move);
    lv_anim_set_values(&a, -20, 20);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    lv_timer_create(round_cb, 10, NULL);
}

T5_APP(.on_create = on_create);
