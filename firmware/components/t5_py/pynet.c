/* SPDX-License-Identifier: Apache-2.0 */
#include "py_net.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

uint32_t py_net_poll_tick;

int py_net_sliced(void *h, py_net_step_t step, void *ctx, int32_t timeout_ms)
{
    uint32_t t0 = mp_hal_ticks_ms();
    for (;;) {
        uint32_t ms = PY_NET_SLICE_MS;
        if (timeout_ms >= 0) {
            uint32_t el = mp_hal_ticks_ms() - t0;
            uint32_t left = el >= (uint32_t)timeout_ms ? 0 : (uint32_t)timeout_ms - el;
            if (left < ms) ms = left;
        }
        int r = step(h, ctx, ms);
        if (r != T5_ERR_TIMEOUT) return r;
        if (timeout_ms >= 0 && mp_hal_ticks_ms() - t0 >= (uint32_t)timeout_ms) return T5_ERR_TIMEOUT;
        mp_event_handle_nowait();
    }
}

MP_NORETURN void py_net_fail(int e, py_errno_ctx_t ctx)
{
    mp_event_handle_nowait();
    if (ctx == PY_ERRNO_GENERIC || e == T5_ERR_DENIED) py_raise_t5((t5_err_t)e);
    mp_raise_OSError(py_errno((t5_err_t)e, ctx));
}

uint16_t py_net_port_arg(mp_obj_t p)
{
    mp_int_t v = mp_obj_get_int(p);
    if (v < 0 || v > 65535) mp_raise_ValueError(MP_ERROR_TEXT("port is 0-65535"));
    return (uint16_t)v;
}

void py_net_addr_arg(mp_obj_t t, t5_net_addr_t *out, py_errno_ctx_t ctx)
{
    bool sock = ctx == PY_ERRNO_SOCKET;
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(t, &n, &items);
    if (sock ? n < 2 : n != 2) mp_raise_TypeError(MP_ERROR_TEXT("address is (host, port)"));
    const char *host = mp_obj_str_get_str(items[0]);
    uint16_t port = py_net_port_arg(items[1]);
    if (sock && !*host) host = "0.0.0.0";
    t5_err_t e = t5_net_resolve(host, out, PY_NET_CONNECT_MS);
    if (e != T5_OK) py_net_fail(e, ctx);
    out->port = port;
}

mp_obj_t py_net_addr_tuple(const t5_net_addr_t *a)
{
    char ip[48];
    if (t5_net_addr_str(a, ip, sizeof ip) < 0) ip[0] = 0;
    mp_obj_t t[2] = { mp_obj_new_str_from_cstr(ip), MP_OBJ_NEW_SMALL_INT(a->port) };
    return mp_obj_new_tuple(2, t);
}
