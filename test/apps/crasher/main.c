/* Phase 5 exit test: crashes on purpose half a second after starting
 * (a store through NULL), so the device panics and reboots. The crash guard
 * must record it, return to the launcher, and stop auto-launching the app
 * after the second crash in a row. */
#include <t5/app.h>
#include <t5/ui.h>

static void boom(lv_timer_t *t)
{
    volatile int *p = (volatile int *)0;
    *p = 42;
}

static void on_create(t5_app_t *app)
{
    lv_obj_t *l = md_label_create(t5_app_screen(app), "Crashing in 0.5 s", MD_TYPE_HEADLINE5);
    lv_obj_center(l);
    lv_timer_create(boom, 500, NULL);
}

T5_APP(.on_create = on_create);
