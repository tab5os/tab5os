/* SPDX-License-Identifier: Apache-2.0
 * Posting, the subscriber table and dispatch. Shared by the device loop and
 * host tests.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "t5_core/event.h"
#include "t5_core/event_test.h"
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
    return t5_mutex_once(&s_lock) ? T5_OK : T5_ERR_NO_MEM;
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

t5_err_t t5_event_post(t5_event_id_t id, const void *data, size_t len)
{
    if (len > T5_EVENT_MAX_DATA) {
        return T5_ERR_INVALID_ARG;
    }
    if (!data) {
        return t5_event_loop_post(id, NULL);
    }
    t5_event_msg_t m = { .len = (uint32_t)len };
    memcpy(m.data, data, len);
    return t5_event_loop_post(id, &m);
}

void t5_event_deliver(t5_event_id_t id, const t5_event_msg_t *msg)
{
    const void *data = msg ? msg->data : NULL;
    size_t len = msg ? msg->len : 0;
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
    /* Without the lock, so handlers may (un)subscribe or post. */
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
