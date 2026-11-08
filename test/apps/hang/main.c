/* Plan 5.12 test: blocks the UI task forever half a second after starting.
 * The UI watchdog must show "App not responding" after 2 s; Wait hides it
 * for 5 s; Close app (or `app anr close` on the console) closes the app
 * without restarting the device and records the hang.
 *
 *   (none)  a busy loop in app code
 *   sleep   a wait that never ends, inside the OS (vTaskDelay)
 */
#include <string.h>
#include <t5/app.h>
#include <t5/sys.h>
#include <t5/ui.h>

static const char *s_mode = "";

static void stuck(lv_timer_t *t)
{
    if (!strcmp(s_mode, "sleep")) {
        for (;;) vTaskDelay(100000);
    }
    for (volatile unsigned i = 0;; i++) {
    }
}

static void on_create(t5_app_t *app)
{
    s_mode = t5_app_launch_arg(app);
    lv_obj_t *l = md_label_create(t5_app_screen(app), "Hanging in 0.5 s", MD_TYPE_HEADLINE5);
    lv_obj_center(l);
    lv_timer_create(stuck, 500, NULL);
}

T5_APP(.on_create = on_create);
