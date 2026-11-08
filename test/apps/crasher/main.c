/* Misbehaving-app test (plan 5.11, isolate.h). Misbehaves half a second
 * after starting, in the way the launch argument picks
 * (`app run com.t5.test.crasher <mode>`):
 *
 *   (none)  store through NULL in a UI timer: app code on the UI task
 *   call    strlen(NULL) from a UI timer: the fault is in the OS, called by the app
 *   lvgl    lv_label_set_text on a bad object: the fault is deep in LVGL
 *   task    store through NULL in a task the app created
 *   hog     a task that spins forever without sleeping
 *   mem     malloc until refused, then LVGL buffers until PSRAM runs out
 *   draw    store through NULL in a draw callback, mid-frame
 *   drawbad a NULL store in a draw callback, a few frames in, with shadows
 *   stack   unbounded recursion in a task the app created
 *   uistack unbounded recursion on the UI task
 *   dfree   double free, then keeps running (no crash expected)
 *   badfree free() of a wild pointer that faults with the OS's locks held
 *   tasks   starts tasks until refused, then keeps running
 *   objs    creates queues and mutexes until refused, then keeps running
 *   post    floods the UI task with calls, then keeps running
 *   lvleak  lv_malloc forever from a timer (must end with the app)
 *   evhang  an event handler that never returns
 *   evcrash an event handler that faults
 *   wcode   writes over the OS's code
 *   wrodata writes over the OS's read-only data
 *   uilock  a task takes the UI lock and never gives it back
 *   late    leaves calls queued for the UI task as it exits (must not run)
 *   hog2    tasks spinning on both cores
 *   socks   opens sockets until refused, then keeps running
 *   syswrite tries to write and delete the OS's files, then keeps running
 *
 * The OS must stay up in every case: the app is closed (or, for hog, its
 * task moved to the lowest priority), "app crashes" shows the record, and
 * the launcher is back. After two crashes in a row the app is no longer
 * auto-launched. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <t5/app.h>
#include <t5/sys.h>
#include <t5/ui.h>

static const char *s_mode = "";
static lv_obj_t *s_label;

static void store_null(void)
{
    volatile int *p = (volatile int *)0;
    *p = 42;
}

static void worker(void *arg)
{
    t5_delay_ms(500);
    store_null();
}

static void spinner(void *arg)
{
    for (volatile unsigned i = 0;; i++) {
    }
}

static int __attribute__((noinline)) recurse(int n)
{
    volatile char pad[256];
    pad[0] = (char)n;
    return recurse(n + 1) + pad[0];
}

static void stack_task(void *arg)
{
    t5_delay_ms(500);
    recurse(0);
}

static void hold_ui_lock(void *arg)
{
    t5_delay_ms(500);
    t5_ui_lock();
    for (;;) t5_delay_ms(1000);
}

static void late_call(void *arg)
{
    printf("crasher: LATE CALL RAN\n");
}

extern int lwip_socket(int domain, int type, int protocol);

static void idle_task(void *arg)
{
    for (;;) t5_delay_ms(1000);
}

static void draw_cb(lv_event_t *e)
{
    static unsigned frames;
    if (s_mode[4] && ++frames < 5) {
        lv_obj_invalidate(lv_event_get_target(e));
        return;
    }
    store_null();
}

static void ev_bad(t5_event_id_t id, const void *d, size_t len, void *arg)
{
    if (!strcmp(s_mode, "evcrash")) store_null();
    for (volatile unsigned i = 0;; i++) {
    }
}

static void nop_call(void *arg)
{
}

static void leak_small(lv_timer_t *t)
{
    static unsigned kb;
    for (int i = 0; i < 16; i++) {
        if (lv_malloc(4096)) kb += 4;
    }
    lv_label_set_text_fmt(s_label, "lv_malloc leaked: %u KB", kb);
    if (kb >= 4096) {
        lv_timer_pause(t);
        printf("crasher: lv_malloc leaked %u KB\n", kb);
    }
}

static void leak_lvgl(lv_timer_t *t)
{
    static unsigned n;
    if (lv_draw_buf_create(256, 512, LV_COLOR_FORMAT_ARGB8888, 0)) n++;
    lv_label_set_text_fmt(s_label, "LVGL buffers leaked: %u KB", n * 512);
}

static t5_app_t *s_app;
static t5_app_t *app_of_label(void)
{
    return s_app;
}

static void boom(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (!strcmp(s_mode, "call")) {
        const char *volatile s = NULL;      /* hidden from the compiler: a real call */
        lv_label_set_text_fmt(s_label, "%u", (unsigned)strlen(s));
    } else if (!strcmp(s_mode, "lvgl")) {
        lv_label_set_text((lv_obj_t *)0x10, "boom");
    } else if (!strncmp(s_mode, "draw", 4)) {
        lv_obj_t *o = lv_obj_create(t5_app_screen(app_of_label()));
        if (s_mode[4]) lv_obj_set_style_shadow_width(o, 40, 0);
        lv_obj_set_size(o, 300, 300);
        lv_obj_center(o);
        lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    } else if (!strcmp(s_mode, "late")) {
        /* nothing yet: on_destroy does it */
    } else if (!strcmp(s_mode, "wcode")) {
        *(volatile uint32_t *)(void *)lv_label_set_text = 0;
    } else if (!strcmp(s_mode, "wrodata")) {
        *(volatile char *)t5_err_str(T5_ERR_BUSY) = 'x';
    } else if (!strcmp(s_mode, "socks")) {
        unsigned n = 0;
        while (n < 32 && lwip_socket(2, 2, 0) >= 0) n++;
        printf("crasher: sockets: %u\n", n);
    } else if (!strcmp(s_mode, "syswrite")) {
        FILE *f = fopen("/data/sys/_crasher", "w");
        int r = remove("/data/sys/crashes.bin");
        printf("crasher: write %s, remove %s\n", f ? "ALLOWED" : "refused", r ? "refused" : "ALLOWED");
        if (f) fclose(f);
    } else if (!strcmp(s_mode, "uistack")) {
        recurse(0);
    } else if (!strcmp(s_mode, "dfree")) {
        char *p = malloc(100);
        free(p);
        free(p);
        char *q = malloc(100);
        memset(q, 1, 100);
        lv_label_set_text_fmt(s_label, "double free survived (%p)", q);
        printf("crasher: double free survived\n");
    } else if (!strcmp(s_mode, "badfree")) {
        free((void *)0x20);
    } else if (!strcmp(s_mode, "tasks")) {
        unsigned n = 0;
        while (n < 100 && xTaskCreatePinnedToCore(idle_task, "crasher.i", 2048, NULL, 2, NULL, tskNO_AFFINITY) == pdPASS) n++;
        lv_label_set_text_fmt(s_label, "tasks started: %u", n);
        printf("crasher: tasks started: %u\n", n);
    } else if (!strcmp(s_mode, "objs")) {
        unsigned n = 0;
        while (n < 2000 && (n & 1 ? xSemaphoreCreateMutex() : xQueueCreate(4, 16))) n++;
        lv_label_set_text_fmt(s_label, "kernel objects: %u", n);
        printf("crasher: kernel objects: %u\n", n);
    } else if (!strcmp(s_mode, "post")) {
        unsigned ok = 0, no = 0;
        for (unsigned i = 0; i < 5000; i++) {
            if (t5_ui_async(nop_call, NULL) == T5_OK) ok++;
            else no++;
        }
        lv_label_set_text_fmt(s_label, "posted %u, refused %u", ok, no);
        printf("crasher: posted %u, refused %u\n", ok, no);
    } else if (!strcmp(s_mode, "lvleak")) {
        lv_timer_create(leak_small, 50, NULL);
    } else if (!strncmp(s_mode, "ev", 2)) {
        t5_event_subscribe(T5_EV_ANY, ev_bad, NULL);
    } else if (!strcmp(s_mode, "mem")) {
        size_t total = 0;
        for (;;) {
            void *p = malloc(256 * 1024);
            if (!p) break;
            memset(p, 0xa5, 4096);
            total += 256 * 1024;
        }
        lv_label_set_text_fmt(s_label, "malloc refused after %u KB", (unsigned)(total / 1024));
        printf("crasher: malloc refused after %u KB\n", (unsigned)(total / 1024));
        /* Then leak LVGL memory, which isn't the app's heap, 512 KB every
         * 50 ms: the OS has to step in before PSRAM is gone. */
        lv_timer_create(leak_lvgl, 50, NULL);
    } else {
        store_null();
    }
}

static void on_create(t5_app_t *app)
{
    s_app = app;
    s_mode = t5_app_launch_arg(app);
    char text[64];
    snprintf(text, sizeof text, "Misbehaving in 0.5 s (%s)", s_mode[0] ? s_mode : "null store");
    s_label = md_label_create(t5_app_screen(app), text, MD_TYPE_HEADLINE5);
    lv_obj_center(s_label);
    if (!strcmp(s_mode, "task")) {
        xTaskCreatePinnedToCore(worker, "crasher.w", 3072, NULL, 3, NULL, tskNO_AFFINITY);
    } else if (!strcmp(s_mode, "stack")) {
        xTaskCreatePinnedToCore(stack_task, "crasher.s", 3072, NULL, 3, NULL, tskNO_AFFINITY);
    } else if (!strcmp(s_mode, "uilock")) {
        xTaskCreatePinnedToCore(hold_ui_lock, "crasher.l", 3072, NULL, 2, NULL, tskNO_AFFINITY);
    } else if (!strcmp(s_mode, "hog2")) {
        xTaskCreatePinnedToCore(spinner, "crasher.h0", 2048, NULL, 4, NULL, 0);
        xTaskCreatePinnedToCore(spinner, "crasher.h1", 2048, NULL, 4, NULL, 1);
    } else if (!strcmp(s_mode, "hog")) {
        xTaskCreatePinnedToCore(spinner, "crasher.hog", 2048, NULL, 4, NULL, 0);
    } else {
        lv_timer_create(boom, 500, NULL);
    }
}

static void on_destroy(t5_app_t *app)
{
    if (!strcmp(s_mode, "late")) {
        t5_ui_async(late_call, NULL);
        lv_async_call(late_call, NULL);
    }
}

T5_APP(.on_create = on_create, .on_destroy = on_destroy);
