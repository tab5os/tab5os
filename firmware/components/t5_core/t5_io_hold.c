/* SPDX-License-Identifier: Apache-2.0
 * Holds of ports and radios (t5_core/io_hold.h).
 */
#include <stdio.h>
#include <string.h>
#include "t5_core/err.h"
#include "t5_core/io_hold.h"
#include "t5_core/os.h"

#define WHO_LEN 32

static struct {
    bool held;
    char who[WHO_LEN];
    t5_io_stop_fn_t stop;
} s_hold[T5_IO_COUNT];
static t5_mutex_t s_mx;

static t5_mutex_t mx(void)
{
    return t5_mutex_once(&s_mx);
}

void t5_io_hold_set(t5_io_kind_t kind, const char *who, t5_io_stop_fn_t stop)
{
    if ((unsigned)kind >= T5_IO_COUNT) return;
    t5_mutex_lock(mx());
    s_hold[kind].held = who != NULL;
    snprintf(s_hold[kind].who, WHO_LEN, "%s", who ? who : "");
    s_hold[kind].stop = who ? stop : NULL;
    t5_mutex_unlock(mx());
}

bool t5_io_hold_get(t5_io_kind_t kind, char *who, size_t n)
{
    if ((unsigned)kind >= T5_IO_COUNT) return false;
    t5_mutex_lock(mx());
    bool held = s_hold[kind].held;
    if (who && n) snprintf(who, n, "%s", s_hold[kind].who);
    t5_mutex_unlock(mx());
    return held;
}

static void stop_task(void *arg)
{
    t5_io_stop_fn_t fn = (t5_io_stop_fn_t)arg;
    fn();
}

void t5_io_hold_stop(t5_io_kind_t kind)
{
    if ((unsigned)kind >= T5_IO_COUNT) return;
    t5_mutex_lock(mx());
    t5_io_stop_fn_t fn = s_hold[kind].held ? s_hold[kind].stop : NULL;
    t5_mutex_unlock(mx());
    void *saved = t5_app_ctx_get();
    t5_app_ctx_set(NULL);                     /* the system's task, not the caller's app's */
    if (fn) T5_IGNORE(t5_task_create("io.stop", stop_task, (void *)fn, 4096, 5, T5_CORE_ANY, NULL));
    t5_app_ctx_set(saved);
}

const char *t5_io_kind_text(t5_io_kind_t kind)
{
    static const char *const t[T5_IO_COUNT] = {
        "Using USB-C as a USB device", "Acting as a Bluetooth keyboard or mouse", "Watching Wi-Fi traffic",
        "Full control of USB-C", "Full control of USB-A", "Full control of Wi-Fi", "Full control of Bluetooth",
    };
    return (unsigned)kind < T5_IO_COUNT ? t[kind] : "";
}
