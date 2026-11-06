/* Plan 5.12 test: blocks the UI task forever half a second after starting.
 * The UI watchdog must show "App not responding" after 2 s; Wait hides it
 * for 5 s, Close app restarts to the launcher and records the hang. */
#include <t5/app.h>
#include <t5/ui.h>

static void stuck(lv_timer_t *t)
{
    for (volatile unsigned i = 0;; i++) {
    }
}

static void on_create(t5_app_t *app)
{
    lv_obj_t *l = md_label_create(t5_app_screen(app), "Hanging in 0.5 s", MD_TYPE_HEADLINE5);
    lv_obj_center(l);
    lv_timer_create(stuck, 500, NULL);
}

T5_APP(.on_create = on_create);
