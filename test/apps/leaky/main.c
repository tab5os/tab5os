/* Phase 5 exit test: an app that leaks on purpose. It allocates memory,
 * claims grove, holds a wake lock and a rail, starts tasks, LVGL timers and
 * animations, subscribes to events, opens a file and creates a mutex, and
 * frees none of it. Opening and closing it 500 times (`app stress
 * com.t5.test.leaky 500`) must leave heap, claims, locks and rails where
 * they started. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/port.h>
#include <t5/power.h>
#include <t5/sys.h>
#include <t5/ui.h>

static void spin(void *arg)
{
    t5_app_t *app = arg;
    /* A well-behaved loop: stops when the app exits. */
    while (t5_app_running(app)) {
        void *p = malloc(64);       /* and leaks a little each time */
        (void)p;
        t5_delay_ms(20);
    }
}

static void rogue(void *arg)
{
    /* A badly behaved one: never checks, has to be deleted. */
    for (;;) vTaskDelay(10);
}

static void tick(lv_timer_t *t)
{
    lv_obj_t *label = lv_timer_get_user_data(t);
    static int n;
    lv_label_set_text_fmt(label, "tick %d", ++n);
}

static void fade(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void on_battery(t5_event_id_t id, const void *data, size_t len, void *arg)
{
}

static void on_create(t5_app_t *app)
{
    for (int i = 0; i < 32; i++) {
        char *p = malloc(1024 + i * 100);
        if (p) memset(p, i, 1024);
    }
    static void *volatile keep;
    keep = calloc(10, 1000);
    keep = strdup("leaked string");

    t5_port_t *grove = NULL;
    t5_port_i2c_cfg_t cfg = { .hz = 100000 };
    t5_err_t e = t5_port_open("grove", T5_MODE_I2C, &cfg, &grove);
    (void)t5_wakelock_acquire(T5_LOCK_CPU, "leaky");
    t5_rail_hold_t *hold = NULL;
    (void)t5_rail_acquire(T5_RAIL_EXT5V, "leaky", 0, &hold);

    t5_task_create("leaky_spin", spin, app, 3072, 3, T5_CORE_ANY, NULL);
    xTaskCreatePinnedToCore(rogue, "leaky_rogue", 2048, NULL, 2, NULL, tskNO_AFFINITY);
    (void)t5_mutex_create();
    (void)xSemaphoreCreateBinary();
    t5_event_subscribe(T5_EV_BATTERY_UPDATE, on_battery, NULL);
    char path[200];
    snprintf(path, sizeof path, "%s/log.txt", t5_app_data_dir(app));
    FILE *f = fopen(path, "a");
    if (f) fputs("opened, never closed\n", f);

    lv_obj_t *scr = t5_app_screen(app);
    lv_obj_t *label = md_label_create(scr, e == T5_OK ? "grove claimed" : t5_err_str(e), MD_TYPE_HEADLINE5);
    lv_obj_center(label);
    lv_timer_create(tick, 50, label);
    lv_timer_t *once = lv_timer_create(tick, 30, label);
    lv_timer_set_repeat_count(once, 1);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, label);
    lv_anim_set_exec_cb(&a, fade);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    /* A dialog on the top layer, outside the app's screen. */
    md_dialog_show("Leaky", "This dialog is never closed by the app.", "OK", NULL, NULL, NULL);
}

T5_APP(.on_create = on_create);
