/* SPDX-License-Identifier: Apache-2.0
 * Subscriber table and dispatch. Shared by the device loop and host tests.
 */
#include <stdbool.h>
#include <stddef.h>
#include "t5_core/event.h"
#include "t5_core/os.h"
#include "t5_event_priv.h"

#define MAX_SUBS 48

typedef struct {
    t5_event_id_t id;
    t5_event_handler_t fn;
    void *arg;
} sub_t;

static sub_t s_subs[MAX_SUBS];
static t5_mutex_t s_lock;

t5_err_t t5_event_table_init(void)
{
    if (!s_lock) {
        s_lock = t5_mutex_create();
    }
    return s_lock ? T5_OK : T5_ERR_NO_MEM;
}

t5_err_t t5_event_subscribe(t5_event_id_t id, t5_event_handler_t fn, void *arg)
{
    if (!fn) {
        return T5_ERR_INVALID_ARG;
    }
    T5_RETURN_ON_ERR(t5_event_table_init());
    t5_err_t err = T5_ERR_NO_MEM;
    t5_mutex_lock(s_lock);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!s_subs[i].fn) {
            s_subs[i] = (sub_t){ id, fn, arg };
            err = T5_OK;
            break;
        }
    }
    t5_mutex_unlock(s_lock);
    return err;
}

t5_err_t t5_event_unsubscribe(t5_event_id_t id, t5_event_handler_t fn, void *arg)
{
    T5_RETURN_ON_ERR(t5_event_table_init());
    t5_err_t err = T5_ERR_NOT_FOUND;
    t5_mutex_lock(s_lock);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_subs[i].fn == fn && s_subs[i].arg == arg && s_subs[i].id == id) {
            s_subs[i].fn = NULL;
            err = T5_OK;
        }
    }
    t5_mutex_unlock(s_lock);
    return err;
}

void t5_event_dispatch(t5_event_id_t id, const void *data, size_t len)
{
    sub_t hits[MAX_SUBS];
    int n = 0;
    if (t5_event_table_init() != T5_OK) {
        return;
    }
    t5_mutex_lock(s_lock);
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_subs[i].fn && (s_subs[i].id == id || s_subs[i].id == T5_EV_ANY)) {
            hits[n++] = s_subs[i];
        }
    }
    t5_mutex_unlock(s_lock);
    /* Handlers run without the lock so they may (un)subscribe or post. */
    for (int i = 0; i < n; i++) {
        hits[i].fn(id, data, len, hits[i].arg);
    }
}

void t5_event_reset_for_test(void)
{
    if (s_lock) {
        t5_mutex_lock(s_lock);
    }
    for (int i = 0; i < MAX_SUBS; i++) {
        s_subs[i].fn = NULL;
    }
    if (s_lock) {
        t5_mutex_unlock(s_lock);
    }
}
