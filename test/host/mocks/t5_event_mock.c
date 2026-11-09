/* SPDX-License-Identifier: Apache-2.0
 * Host event loop: every post is delivered at once, on the posting task,
 * through the real post and dispatch code (t5_event.c). Posts are logged.
 */
#include "t5_event_priv.h"
#include "t5_mocks.h"

static t5_event_id_t s_log[MOCK_EV_MAX];
static int s_n;

t5_err_t t5_event_init(void) { return t5_event_table_init(); }

t5_err_t t5_event_loop_post(t5_event_id_t id, const t5_event_msg_t *msg)
{
    if (s_n < MOCK_EV_MAX) s_log[s_n++] = id;
    t5_event_deliver(id, msg);
    return T5_OK;
}

int mock_events_count(t5_event_id_t id)
{
    int n = 0;
    for (int i = 0; i < s_n; i++) n += s_log[i] == id;
    return n;
}

void mock_events_reset(void) { s_n = 0; }
