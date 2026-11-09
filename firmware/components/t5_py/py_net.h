/* SPDX-License-Identifier: Apache-2.0
 * What t5.net (mod_net.c) and the socket module (mod_socket.c) share: both
 * wrap t5_net handles (t5_net/sock.h) and wait on them the same way.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "t5_net/sock.h"
#include "py_priv.h"

/* A wait is cut into slices this long, with callbacks, Ctrl-C and Stop
 * handled in between. */
#define PY_NET_SLICE_MS 100
/* Connects, TLS handshakes and lookups when the caller gives no timeout. */
#define PY_NET_CONNECT_MS 10000

/* Handle pointers are reused, so an object keeps the handle's id from when
 * it got it: the handle is still its own while the id matches. */
static inline bool py_net_alive(void *h, uint32_t id)
{
    return h && id && t5_net_handle_id(h) == id;
}

/* One slice of a wait on handle h: a result, or T5_ERR_TIMEOUT to go on. */
typedef int (*py_net_step_t)(void *h, void *ctx, uint32_t ms);
/* Call step in slices until it returns something other than T5_ERR_TIMEOUT
 * or timeout_ms passes (-1: no limit, 0: one try). */
int py_net_sliced(void *h, py_net_step_t step, void *ctx, int32_t timeout_ms);

/* Raise for a failed call; a pending Stop or Ctrl-C wins over the error.
 * PY_ERRNO_GENERIC raises OSError(errno, text), PY_ERRNO_SOCKET the bare
 * OSError(errno) socket code expects (a refused permission keeps its text). */
MP_NORETURN void py_net_fail(int e, py_errno_ctx_t ctx);

uint16_t py_net_port_arg(mp_obj_t p);
/* (host, port) -> address, resolving names. The socket module also takes
 * longer tuples (IPv6 flow info, scope) and "" for every interface. */
void py_net_addr_arg(mp_obj_t t, t5_net_addr_t *out, py_errno_ctx_t ctx);
mp_obj_t py_net_addr_tuple(const t5_net_addr_t *a);
