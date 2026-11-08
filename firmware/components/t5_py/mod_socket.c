/* SPDX-License-Identifier: Apache-2.0
 * The standard `socket` module (CPython/MicroPython API), so ordinary
 * Python networking code and libraries (umqtt, asyncio streams, aiohttp,
 * microdot...) run unchanged. It is not MicroPython's lwIP socket module:
 * every socket is a t5_sock handle (t5_net/sock.h), so it needs the net
 * permission, counts against the app's handle limit and is closed when the
 * app exits, and nothing here can name a raw address in memory.
 *
 * A socket is a stream (read/readinto/readline/write work, and select.poll
 * and asyncio can wait on it). The BSD sequence (socket, setsockopt, bind,
 * listen/connect) is collected until the t5 handle can be made: connect()
 * for TCP clients, listen() for servers, bind()/sendto()/connect() for UDP.
 *
 * Timeouts follow Python: settimeout(seconds) or None (blocking, the
 * default) or 0 / setblocking(False) (raise EAGAIN, streams return None).
 * Waits are sliced so callbacks, Ctrl-C and Stop get through. connect()
 * always completes before returning, also on non-blocking sockets (asyncio
 * handles that the same way as an immediate connect).
 */
#include <stdlib.h>
#include <string.h>
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "t5_net/sock.h"
#include "t5_pkg/manifest.h"
#include "py_priv.h"

#define SLICE_MS 100
#define CONNECT_MS 10000

/* lwIP's values, which programs ported from C expect. */
enum {
    AF_INET_ = 2, AF_INET6_ = 10, SOCK_STREAM_ = 1, SOCK_DGRAM_ = 2, SOCK_RAW_ = 3,
    IPPROTO_IP_ = 0, IPPROTO_TCP_ = 6, IPPROTO_UDP_ = 17,
    SOL_SOCKET_ = 0xfff, SO_REUSEADDR_ = 0x0004, SO_KEEPALIVE_ = 0x0008, SO_BROADCAST_ = 0x0020,
    TCP_NODELAY_ = 0x01, IP_TTL_ = 2, IP_ADD_MEMBERSHIP_ = 3, IP_DROP_MEMBERSHIP_ = 4, IP_MULTICAST_TTL_ = 5,
    IP_MULTICAST_LOOP_ = 7,
};

enum { ST_NEW, ST_TCP, ST_LISTEN, ST_UDP, ST_CLOSED };

typedef struct {
    mp_obj_base_t base;
    t5_sock_t *h;
    uint32_t id;
    uint8_t family, type, state;
    int32_t timeout;            /* ms; -1 = blocking, 0 = non-blocking */
    uint16_t port;              /* bind() */
    bool reuse, broadcast, nodelay;
    int keepalive;
    uint8_t mcast_ttl;
    bool mcast_loop;
} py_sock_t;

uint32_t py_net_poll_tick;     /* vm.c: wake often while sockets are being polled */

static const mp_obj_type_t py_type_socket;

/* ---- helpers --------------------------------------------------------------------------- */
static int t5_to_errno(int e)
{
    switch (e) {
    case T5_ERR_TIMEOUT: return MP_ETIMEDOUT;
    case T5_ERR_NO_MEM: return MP_ENOMEM;
    case T5_ERR_INVALID_ARG: return MP_EINVAL;
    case T5_ERR_NOT_FOUND: return MP_ENOENT;
    case T5_ERR_BUSY: return MP_EADDRINUSE;
    case T5_ERR_DENIED: return MP_EACCES;
    case T5_ERR_NOT_SUPPORTED: return MP_EOPNOTSUPP;
    case T5_ERR_STATE: return MP_ENOTCONN;
    default: return MP_ECONNRESET;
    }
}

static MP_NORETURN void raise_errno(int err)
{
    mp_event_handle_nowait();          /* a pending Stop or Ctrl-C wins */
    mp_raise_OSError(err);
}

static MP_NORETURN void raise_t5(int e)
{
    if (e == T5_ERR_DENIED) {
        mp_event_handle_nowait();
        py_raise_t5((t5_err_t)e);
    }
    raise_errno(t5_to_errno(e));
}

static bool alive(const py_sock_t *s)
{
    return s->h && s->id && t5_net_handle_id(s->h) == s->id;
}

static py_sock_t *self_live(mp_obj_t o)
{
    py_sock_t *s = MP_OBJ_TO_PTR(o);
    if (s->state == ST_CLOSED || (s->h && !alive(s))) raise_errno(MP_EBADF);
    return s;
}

static void adopt(py_sock_t *s, t5_sock_t *h, uint8_t state)
{
    s->h = h;
    s->id = t5_net_handle_id(h);
    s->state = state;
}

/* (host, port) -> address; names are resolved. */
static void addr_arg(mp_obj_t t, t5_net_addr_t *out, const char **host)
{
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(t, &n, &items);
    if (n < 2) mp_raise_TypeError(MP_ERROR_TEXT("address is (host, port)"));
    const char *h = mp_obj_str_get_str(items[0]);
    mp_int_t port = mp_obj_get_int(items[1]);
    if (port < 0 || port > 65535) mp_raise_ValueError(MP_ERROR_TEXT("port is 0-65535"));
    if (host) *host = h;
    if (!*h) h = "0.0.0.0";
    t5_err_t e = t5_net_resolve(h, out, CONNECT_MS);
    if (e != T5_OK) raise_t5(e);
    out->port = (uint16_t)port;
}

static mp_obj_t addr_tuple(const t5_net_addr_t *a)
{
    char ip[48];
    if (t5_net_addr_str(a, ip, sizeof ip) < 0) ip[0] = 0;
    mp_obj_t t[2] = { mp_obj_new_str_from_cstr(ip), MP_OBJ_NEW_SMALL_INT(a->port) };
    return mp_obj_new_tuple(2, t);
}

/* Call step in slices for the socket's timeout. Returns its result, or
 * T5_ERR_TIMEOUT once the timeout passes (immediately for non-blocking). */
typedef int (*step_t)(py_sock_t *s, void *ctx, uint32_t ms);

static int sliced(py_sock_t *s, step_t step, void *ctx)
{
    uint32_t t0 = mp_hal_ticks_ms();
    for (;;) {
        uint32_t ms = SLICE_MS;
        if (s->timeout >= 0) {
            uint32_t el = mp_hal_ticks_ms() - t0;
            uint32_t left = el >= (uint32_t)s->timeout ? 0 : (uint32_t)s->timeout - el;
            if (left < ms) ms = left;
        }
        int r = step(s, ctx, ms);
        if (r != T5_ERR_TIMEOUT) return r;
        if (s->timeout >= 0 && mp_hal_ticks_ms() - t0 >= (uint32_t)s->timeout) return T5_ERR_TIMEOUT;
        mp_event_handle_nowait();
    }
}

/* ---- opening the t5 handle lazily ---------------------------------------------------------- */
static void apply_opts(py_sock_t *s)
{
    if (s->nodelay) T5_IGNORE(t5_sock_set_option(s->h, T5_SOCKOPT_NODELAY, 1));      /* best effort, like setsockopt */
    if (s->keepalive) T5_IGNORE(t5_sock_set_option(s->h, T5_SOCKOPT_KEEPALIVE, s->keepalive));
}

static void udp_open(py_sock_t *s)
{
    if (s->state == ST_UDP) return;
    uint32_t f = (s->reuse ? T5_UDP_REUSE : 0) | (s->broadcast ? T5_UDP_BROADCAST : 0) |
                 (s->family == AF_INET6_ ? T5_UDP_IPV6 : 0);
    t5_sock_t *h;
    t5_err_t e = t5_udp_open(s->port, f, &h);
    if (e != T5_OK) raise_t5(e);
    adopt(s, h, ST_UDP);
}

/* ---- the stream protocol --------------------------------------------------------------------- */
typedef struct {
    void *buf;
    size_t n;
} io_t;

static int step_read(py_sock_t *s, void *c, uint32_t ms)
{
    io_t *io = c;
    return t5_sock_recv(s->h, io->buf, io->n, ms);
}

static int step_write(py_sock_t *s, void *c, uint32_t ms)
{
    io_t *io = c;
    int r = t5_sock_send(s->h, io->buf, io->n, ms);
    return r == 0 && io->n ? T5_ERR_TIMEOUT : r;
}

static mp_uint_t sock_read(mp_obj_t o, void *buf, mp_uint_t size, int *err)
{
    py_sock_t *s = MP_OBJ_TO_PTR(o);
    if (s->state != ST_TCP && s->state != ST_UDP) {
        *err = MP_ENOTCONN;
        return MP_STREAM_ERROR;
    }
    if (!alive(s)) {
        *err = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    if (!size) return 0;
    io_t io = { buf, size };
    int r = sliced(s, step_read, &io);
    if (r >= 0) return (mp_uint_t)r;
    *err = r == T5_ERR_TIMEOUT ? (s->timeout == 0 ? MP_EAGAIN : MP_ETIMEDOUT) : t5_to_errno(r);
    return MP_STREAM_ERROR;
}

static mp_uint_t sock_write(mp_obj_t o, const void *buf, mp_uint_t size, int *err)
{
    py_sock_t *s = MP_OBJ_TO_PTR(o);
    if (s->state != ST_TCP && s->state != ST_UDP) {
        *err = MP_ENOTCONN;
        return MP_STREAM_ERROR;
    }
    if (!alive(s)) {
        *err = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    io_t io = { (void *)buf, size };
    int r = size ? sliced(s, step_write, &io) : 0;
    if (r >= 0) return (mp_uint_t)r;
    *err = r == T5_ERR_TIMEOUT ? (s->timeout == 0 ? MP_EAGAIN : MP_ETIMEDOUT) : t5_to_errno(r);
    return MP_STREAM_ERROR;
}

static mp_obj_t sock_close(mp_obj_t o);

static mp_uint_t sock_ioctl(mp_obj_t o, mp_uint_t req, uintptr_t arg, int *err)
{
    py_sock_t *s = MP_OBJ_TO_PTR(o);
    if (req == MP_STREAM_CLOSE) {
        sock_close(o);
        return 0;
    }
    if (req != MP_STREAM_POLL) {
        *err = MP_EINVAL;
        return MP_STREAM_ERROR;
    }
    py_net_poll_tick = mp_hal_ticks_ms();
    if (s->state == ST_CLOSED) return MP_STREAM_POLL_NVAL;
    if (s->state == ST_NEW) return arg & MP_STREAM_POLL_WR;          /* nothing to wait for yet */
    if (!alive(s)) return MP_STREAM_POLL_HUP | MP_STREAM_POLL_ERR;
    t5_poll_t p = { .handle = s->h, .events = (uint8_t)(((arg & MP_STREAM_POLL_RD) ? T5_POLL_IN : 0) |
                                                        ((arg & MP_STREAM_POLL_WR) ? T5_POLL_OUT : 0)) };
    int r = t5_net_poll(&p, 1, 0);
    mp_uint_t ret = 0;
    if (r > 0) {
        if (p.revents & T5_POLL_IN) ret |= MP_STREAM_POLL_RD;
        if (p.revents & T5_POLL_OUT) ret |= MP_STREAM_POLL_WR;
        if (p.revents & T5_POLL_ERR) ret |= MP_STREAM_POLL_ERR | MP_STREAM_POLL_HUP;
    }
    return ret & (arg | MP_STREAM_POLL_ERR | MP_STREAM_POLL_HUP);
}

static const mp_stream_p_t sock_stream_p = {
    .read = sock_read,
    .write = sock_write,
    .ioctl = sock_ioctl,
    .is_text = false,
};

/* ---- methods ------------------------------------------------------------------------------- */
static mp_obj_t sock_close(mp_obj_t o)
{
    py_sock_t *s = MP_OBJ_TO_PTR(o);
    if (alive(s)) T5_IGNORE(t5_sock_close(s->h));      /* finaliser too: nothing to report to */
    s->h = NULL;
    s->id = 0;
    s->state = ST_CLOSED;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_close_obj, sock_close);

static mp_obj_t sock_exit(size_t n, const mp_obj_t *args)
{
    (void)n;
    return sock_close(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_exit_obj, 1, 4, sock_exit);

static mp_obj_t sock_connect(mp_obj_t o, mp_obj_t addr)
{
    py_sock_t *s = self_live(o);
    if (s->type == SOCK_DGRAM_) {
        t5_net_addr_t a;
        addr_arg(addr, &a, NULL);
        udp_open(s);
        t5_err_t e = t5_udp_connect(s->h, &a);
        if (e != T5_OK) raise_t5(e);
        return mp_const_none;
    }
    if (s->state != ST_NEW) raise_errno(MP_EISCONN);
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(addr, &n, &items);
    if (n < 2) mp_raise_TypeError(MP_ERROR_TEXT("address is (host, port)"));
    mp_int_t port = mp_obj_get_int(items[1]);
    if (port <= 0 || port > 65535) mp_raise_ValueError(MP_ERROR_TEXT("port is 1-65535"));
    t5_tcp_cfg_t cfg = { .timeout_ms = s->timeout > 0 ? (uint32_t)s->timeout : CONNECT_MS,
                         .flags = s->nodelay ? T5_TCP_NODELAY : 0 };
    t5_sock_t *h;
    t5_err_t e = t5_tcp_connect(mp_obj_str_get_str(items[0]), (uint16_t)port, &cfg, &h);
    if (e != T5_OK) raise_errno(e == T5_ERR_IO ? MP_ECONNREFUSED : e == T5_ERR_NOT_FOUND ? MP_ENOENT : t5_to_errno(e));
    adopt(s, h, ST_TCP);
    apply_opts(s);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_connect_obj, sock_connect);

static mp_obj_t sock_bind(mp_obj_t o, mp_obj_t addr)
{
    py_sock_t *s = self_live(o);
    if (s->state != ST_NEW) raise_errno(MP_EINVAL);
    t5_net_addr_t a;
    addr_arg(addr, &a, NULL);              /* the address part is ignored: every interface */
    s->port = a.port;
    if (s->type == SOCK_DGRAM_) udp_open(s);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_bind_obj, sock_bind);

static mp_obj_t sock_listen(size_t n, const mp_obj_t *args)
{
    py_sock_t *s = self_live(args[0]);
    if (s->type != SOCK_STREAM_ || s->state != ST_NEW) raise_errno(MP_EINVAL);
    mp_int_t backlog = n > 1 ? mp_obj_get_int(args[1]) : 4;
    t5_sock_t *h;
    t5_err_t e = t5_tcp_listen(s->port, (int)backlog, &h);
    if (e != T5_OK) raise_t5(e);
    adopt(s, h, ST_LISTEN);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_listen_obj, 1, 2, sock_listen);

typedef struct {
    t5_sock_t *got;
} accept_t;

static int step_accept(py_sock_t *s, void *c, uint32_t ms)
{
    return t5_tcp_accept(s->h, ms, &((accept_t *)c)->got);
}

static mp_obj_t sock_accept(mp_obj_t o)
{
    py_sock_t *s = self_live(o);
    if (s->state != ST_LISTEN) raise_errno(MP_EINVAL);
    py_sock_t *c = mp_obj_malloc_with_finaliser(py_sock_t, &py_type_socket);  /* first: a Stop can't leak it */
    memset((uint8_t *)c + sizeof c->base, 0, sizeof *c - sizeof c->base);
    c->family = s->family;
    c->type = SOCK_STREAM_;
    c->timeout = -1;
    accept_t a = { NULL };
    int r = sliced(s, step_accept, &a);
    if (r == T5_ERR_TIMEOUT) raise_errno(s->timeout == 0 ? MP_EAGAIN : MP_ETIMEDOUT);
    if (r < 0) raise_t5(r);
    adopt(c, a.got, ST_TCP);
    t5_net_addr_t peer = { 0 };
    T5_IGNORE(t5_sock_peer_addr(a.got, &peer));        /* ("", 0) if it already went away */
    mp_obj_t t[2] = { MP_OBJ_FROM_PTR(c), addr_tuple(&peer) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_accept_obj, sock_accept);

/* send(buf): what could be sent before the timeout (at least one byte). */
static mp_obj_t sock_send(mp_obj_t o, mp_obj_t data)
{
    py_sock_t *s = self_live(o);
    if (s->state != ST_TCP && s->state != ST_UDP) raise_errno(MP_ENOTCONN);
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    int err;
    mp_uint_t r = sock_write(o, b.buf, b.len, &err);
    if (r == MP_STREAM_ERROR) raise_errno(err);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_send_obj, sock_send);

static mp_obj_t sock_sendall(mp_obj_t o, mp_obj_t data)
{
    py_sock_t *s = self_live(o);
    if (s->state != ST_TCP && s->state != ST_UDP) raise_errno(MP_ENOTCONN);
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    size_t done = 0;
    while (done < b.len) {             /* bounded: each pass sends something or raises */
        int err;
        mp_uint_t r = sock_write(o, (uint8_t *)b.buf + done, b.len - done, &err);
        if (r == MP_STREAM_ERROR) raise_errno(err);
        done += r;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_sendall_obj, sock_sendall);

static mp_obj_t sock_recv(mp_obj_t o, mp_obj_t len)
{
    self_live(o);
    mp_int_t n = mp_obj_get_int(len);
    if (n < 0) mp_raise_ValueError(NULL);
    vstr_t v;
    vstr_init_len(&v, (size_t)n);
    int err;
    mp_uint_t r = sock_read(o, v.buf, (mp_uint_t)n, &err);
    if (r == MP_STREAM_ERROR) {
        vstr_clear(&v);
        raise_errno(err);
    }
    v.len = r;
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_recv_obj, sock_recv);

typedef struct {
    const t5_net_addr_t *to;
    void *buf;
    size_t n;
    t5_net_addr_t from;
} dgram_t;

static int step_sendto(py_sock_t *s, void *c, uint32_t ms)
{
    dgram_t *d = c;
    return t5_udp_sendto(s->h, d->to, d->buf, d->n, ms);
}

static int step_recvfrom(py_sock_t *s, void *c, uint32_t ms)
{
    dgram_t *d = c;
    return t5_udp_recvfrom(s->h, d->buf, d->n, &d->from, ms);
}

static mp_obj_t sock_sendto(mp_obj_t o, mp_obj_t data, mp_obj_t addr)
{
    py_sock_t *s = self_live(o);
    if (s->type != SOCK_DGRAM_) raise_errno(MP_EOPNOTSUPP);
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    t5_net_addr_t to;
    addr_arg(addr, &to, NULL);
    udp_open(s);
    dgram_t d = { &to, b.buf, b.len, { 0 } };
    int r = sliced(s, step_sendto, &d);
    if (r == T5_ERR_TIMEOUT) raise_errno(s->timeout == 0 ? MP_EAGAIN : MP_ETIMEDOUT);
    if (r < 0) raise_t5(r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_3(sock_sendto_obj, sock_sendto);

static mp_obj_t sock_recvfrom(mp_obj_t o, mp_obj_t len)
{
    py_sock_t *s = self_live(o);
    if (s->type != SOCK_DGRAM_) raise_errno(MP_EOPNOTSUPP);
    mp_int_t n = mp_obj_get_int(len);
    if (n <= 0 || n > 65507) mp_raise_ValueError(MP_ERROR_TEXT("size is 1-65507"));
    udp_open(s);
    vstr_t v;
    vstr_init_len(&v, (size_t)n);
    dgram_t d = { NULL, v.buf, (size_t)n, { 0 } };
    int r = sliced(s, step_recvfrom, &d);
    if (r < 0) {
        vstr_clear(&v);
        if (r == T5_ERR_TIMEOUT) raise_errno(s->timeout == 0 ? MP_EAGAIN : MP_ETIMEDOUT);
        raise_t5(r);
    }
    v.len = (size_t)r;
    mp_obj_t t[2] = { mp_obj_new_bytes_from_vstr(&v), addr_tuple(&d.from) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_recvfrom_obj, sock_recvfrom);

static mp_obj_t sock_settimeout(mp_obj_t o, mp_obj_t t)
{
    py_sock_t *s = MP_OBJ_TO_PTR(o);
    if (t == mp_const_none) {
        s->timeout = -1;
    } else {
        mp_float_t sec = mp_obj_get_float(t);
        if (sec < 0) mp_raise_ValueError(MP_ERROR_TEXT("timeout must be >= 0"));
        s->timeout = (int32_t)(sec * 1000);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_settimeout_obj, sock_settimeout);

static mp_obj_t sock_setblocking(mp_obj_t o, mp_obj_t flag)
{
    py_sock_t *s = MP_OBJ_TO_PTR(o);
    s->timeout = mp_obj_is_true(flag) ? -1 : 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_setblocking_obj, sock_setblocking);

static mp_obj_t sock_setsockopt(size_t n, const mp_obj_t *args)
{
    (void)n;
    py_sock_t *s = self_live(args[0]);
    mp_int_t level = mp_obj_get_int(args[1]), opt = mp_obj_get_int(args[2]);
    mp_int_t val = 0;
    mp_buffer_info_t b = { 0 };
    if (mp_obj_is_int(args[3]) || mp_obj_is_bool(args[3])) val = mp_obj_get_int(args[3]);
    else mp_get_buffer_raise(args[3], &b, MP_BUFFER_READ);
    t5_err_t e = T5_OK;
    if (level == SOL_SOCKET_ && opt == SO_REUSEADDR_) {
        s->reuse = val;
        if (s->h) e = t5_sock_set_option(s->h, T5_SOCKOPT_REUSEADDR, (int)val);
    } else if (level == SOL_SOCKET_ && opt == SO_BROADCAST_) {
        s->broadcast = val;
        if (s->h) e = t5_sock_set_option(s->h, T5_SOCKOPT_BROADCAST, (int)val);
    } else if (level == SOL_SOCKET_ && opt == SO_KEEPALIVE_) {
        s->keepalive = val ? 60 : 0;
        if (s->state == ST_TCP) e = t5_sock_set_option(s->h, T5_SOCKOPT_KEEPALIVE, s->keepalive);
    } else if (level == IPPROTO_TCP_ && opt == TCP_NODELAY_) {
        s->nodelay = val;
        if (s->state == ST_TCP) e = t5_sock_set_option(s->h, T5_SOCKOPT_NODELAY, (int)val);
    } else if (level == IPPROTO_IP_ && opt == IP_TTL_) {
        if (s->h) e = t5_sock_set_option(s->h, T5_SOCKOPT_TTL, (int)val);
    } else if (level == IPPROTO_IP_ && (opt == IP_ADD_MEMBERSHIP_ || opt == IP_DROP_MEMBERSHIP_)) {
        /* struct ip_mreq: group, interface (4 bytes each). */
        if (b.len < 4) mp_raise_ValueError(MP_ERROR_TEXT("ip_mreq is 8 bytes"));
        udp_open(s);
        const uint8_t *g = b.buf;
        char group[16];
        snprintf(group, sizeof group, "%u.%u.%u.%u", g[0], g[1], g[2], g[3]);
        e = opt == IP_ADD_MEMBERSHIP_ ? t5_udp_join(s->h, group) : t5_udp_leave(s->h, group);
    } else if (level == IPPROTO_IP_ && (opt == IP_MULTICAST_TTL_ || opt == IP_MULTICAST_LOOP_)) {
        if (opt == IP_MULTICAST_TTL_) s->mcast_ttl = (uint8_t)val;
        else s->mcast_loop = val;
        udp_open(s);
        e = t5_udp_multicast_opts(s->h, s->mcast_ttl ? s->mcast_ttl : 1, s->mcast_loop);
    }
    /* Anything else (SO_RCVBUF, IP_TOS...) is accepted and ignored, as code
     * written for other ports often sets options it doesn't depend on. */
    if (e != T5_OK) raise_t5(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_setsockopt_obj, 4, 4, sock_setsockopt);

static mp_obj_t sock_addr(mp_obj_t o, bool peer)
{
    py_sock_t *s = self_live(o);
    if (!s->h) raise_errno(MP_ENOTCONN);
    t5_net_addr_t a;
    t5_err_t e = peer ? t5_sock_peer_addr(s->h, &a) : t5_sock_local_addr(s->h, &a);
    if (e != T5_OK) raise_errno(MP_ENOTCONN);
    return addr_tuple(&a);
}

static mp_obj_t sock_getpeername(mp_obj_t o)
{
    return sock_addr(o, true);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_getpeername_obj, sock_getpeername);

static mp_obj_t sock_getsockname(mp_obj_t o)
{
    return sock_addr(o, false);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_getsockname_obj, sock_getsockname);

static mp_obj_t sock_makefile(size_t n, const mp_obj_t *args)
{
    (void)n;
    return args[0];                    /* the socket is already a binary stream */
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_makefile_obj, 1, 3, sock_makefile);

static mp_obj_t sock_fileno(mp_obj_t o)
{
    py_sock_t *s = MP_OBJ_TO_PTR(o);
    return MP_OBJ_NEW_SMALL_INT(alive(s) ? (mp_int_t)(s->id & 0x3fffffff) : -1);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_fileno_obj, sock_fileno);

/* ssl.py calls this to turn a connected TCP socket into TLS in place. */
static mp_obj_t sock_tls(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_self, A_host, A_verify, A_ca, A_cert, A_key, A_server, A_alpn, A_name };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_server_hostname, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_verify, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_ca, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_cert, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_key, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_server_side, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_alpn, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_check_name, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_sock_t *s = self_live(a[A_self].u_obj);
    if (s->state != ST_TCP) raise_errno(MP_ENOTCONN);
    const char *alpn[5] = { NULL };
    if (a[A_alpn].u_obj != mp_const_none) {
        size_t n;
        mp_obj_t *items;
        mp_obj_get_array(a[A_alpn].u_obj, &n, &items);
        for (size_t i = 0; i < n && i < 4; i++) alpn[i] = mp_obj_str_get_str(items[i]);
    }
    t5_tls_cfg_t cfg = {
        .flags = (a[A_server].u_bool ? T5_TLS_SERVER : 0) | (a[A_verify].u_bool ? 0 : T5_TLS_NO_VERIFY) |
                 (a[A_name].u_bool ? 0 : T5_TLS_NO_NAME_CHECK),
        .timeout_ms = s->timeout > 0 ? (uint32_t)s->timeout : CONNECT_MS,
        .sni = a[A_host].u_obj != mp_const_none ? mp_obj_str_get_str(a[A_host].u_obj) : NULL,
        .ca_pem = a[A_ca].u_obj != mp_const_none ? mp_obj_str_get_str(a[A_ca].u_obj) : NULL,
        .cert_pem = a[A_cert].u_obj != mp_const_none ? mp_obj_str_get_str(a[A_cert].u_obj) : NULL,
        .key_pem = a[A_key].u_obj != mp_const_none ? mp_obj_str_get_str(a[A_key].u_obj) : NULL,
        .alpn = alpn[0] ? alpn : NULL,
    };
    t5_err_t e = t5_sock_start_tls(s->h, &cfg);
    if (e != T5_OK) raise_t5(e);
    return a[A_self].u_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(sock_tls_obj, 1, sock_tls);

static void sock_print(const mp_print_t *print, mp_obj_t o, mp_print_kind_t kind)
{
    (void)kind;
    static const char *const st[] = { "new", "tcp", "listening", "udp", "closed" };
    py_sock_t *s = MP_OBJ_TO_PTR(o);
    mp_printf(print, "<socket %s>", st[s->state]);
}

static const mp_rom_map_elem_t net_sock_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&sock_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind), MP_ROM_PTR(&sock_bind_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&sock_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_accept), MP_ROM_PTR(&sock_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&sock_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendall), MP_ROM_PTR(&sock_sendall_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&sock_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendto), MP_ROM_PTR(&sock_sendto_obj) },
    { MP_ROM_QSTR(MP_QSTR_recvfrom), MP_ROM_PTR(&sock_recvfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_setblocking), MP_ROM_PTR(&sock_setblocking_obj) },
    { MP_ROM_QSTR(MP_QSTR_setsockopt), MP_ROM_PTR(&sock_setsockopt_obj) },
    { MP_ROM_QSTR(MP_QSTR_getpeername), MP_ROM_PTR(&sock_getpeername_obj) },
    { MP_ROM_QSTR(MP_QSTR_getsockname), MP_ROM_PTR(&sock_getsockname_obj) },
    { MP_ROM_QSTR(MP_QSTR_makefile), MP_ROM_PTR(&sock_makefile_obj) },
    { MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&sock_fileno_obj) },
    { MP_ROM_QSTR(MP_QSTR__tls), MP_ROM_PTR(&sock_tls_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&sock_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&sock_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&sock_exit_obj) },
};
static MP_DEFINE_CONST_DICT(net_sock_locals, net_sock_locals_table);

static mp_obj_t sock_make_new(const mp_obj_type_t *type, size_t n, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n, n_kw, 0, 3, false);
    py_need_perm(T5_PERM_NET);
    mp_int_t af = n > 0 ? mp_obj_get_int(args[0]) : AF_INET_;
    mp_int_t st = n > 1 ? mp_obj_get_int(args[1]) : SOCK_STREAM_;
    if (af != AF_INET_ && af != AF_INET6_) raise_errno(MP_EAFNOSUPPORT);
    if (st != SOCK_STREAM_ && st != SOCK_DGRAM_) raise_errno(MP_EOPNOTSUPP);   /* no raw sockets */
    py_sock_t *s = mp_obj_malloc_with_finaliser(py_sock_t, type);
    memset((uint8_t *)s + sizeof s->base, 0, sizeof *s - sizeof s->base);
    s->family = (uint8_t)af;
    s->type = (uint8_t)st;
    s->timeout = -1;
    s->mcast_ttl = 1;
    return MP_OBJ_FROM_PTR(s);
}

static MP_DEFINE_CONST_OBJ_TYPE(py_type_socket, MP_QSTR_socket, MP_TYPE_FLAG_ITER_IS_STREAM, make_new, sock_make_new,
                                print, sock_print, protocol, &sock_stream_p, locals_dict, &net_sock_locals);

/* ---- module functions ------------------------------------------------------------------------ */
/* getaddrinfo(host, port, af=0, type=0, proto=0, flags=0) -> [(af, type, proto, '', (ip, port))] */
static mp_obj_t socket_getaddrinfo(size_t n, const mp_obj_t *args)
{
    py_need_perm(T5_PERM_NET);
    mp_int_t type = n > 3 && mp_obj_get_int(args[3]) ? mp_obj_get_int(args[3]) : SOCK_STREAM_;
    mp_obj_t host = args[0];
    mp_int_t port = 0;
    if (mp_obj_is_str(args[1])) port = strtol(mp_obj_str_get_str(args[1]), NULL, 10);
    else if (args[1] != mp_const_none) port = mp_obj_get_int(args[1]);
    if (port < 0 || port > 65535) mp_raise_ValueError(MP_ERROR_TEXT("port is 0-65535"));
    t5_net_addr_t a;
    t5_err_t e = t5_net_resolve(host == mp_const_none ? "127.0.0.1" : mp_obj_str_get_str(host), &a, CONNECT_MS);
    if (e != T5_OK) raise_errno(e == T5_ERR_NOT_FOUND || e == T5_ERR_INVALID_ARG ? MP_ENOENT : t5_to_errno(e));
    a.port = (uint16_t)port;
    mp_obj_t t[5] = { MP_OBJ_NEW_SMALL_INT(a.family == 6 ? AF_INET6_ : AF_INET_), MP_OBJ_NEW_SMALL_INT(type),
                      MP_OBJ_NEW_SMALL_INT(type == SOCK_DGRAM_ ? IPPROTO_UDP_ : IPPROTO_TCP_), MP_OBJ_NEW_QSTR(MP_QSTR_),
                      addr_tuple(&a) };
    mp_obj_t entry = mp_obj_new_tuple(5, t);
    return mp_obj_new_list(1, &entry);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_getaddrinfo_obj, 2, 6, socket_getaddrinfo);

static mp_obj_t socket_inet_pton(mp_obj_t af, mp_obj_t s)
{
    t5_net_addr_t a;
    if (t5_net_addr_parse(mp_obj_str_get_str(s), &a) != T5_OK ||
        (a.family == 6) != (mp_obj_get_int(af) == AF_INET6_)) {
        raise_errno(MP_EINVAL);
    }
    return mp_obj_new_bytes(a.ip, a.family == 6 ? 16 : 4);
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_inet_pton_obj, socket_inet_pton);

static mp_obj_t socket_inet_ntop(mp_obj_t af, mp_obj_t b)
{
    mp_buffer_info_t bi;
    mp_get_buffer_raise(b, &bi, MP_BUFFER_READ);
    t5_net_addr_t a = { .family = mp_obj_get_int(af) == AF_INET6_ ? 6 : 4 };
    if (bi.len != (a.family == 6 ? 16u : 4u)) raise_errno(MP_EINVAL);
    memcpy(a.ip, bi.buf, bi.len);
    char ip[48];
    if (t5_net_addr_str(&a, ip, sizeof ip) < 0) raise_errno(MP_EINVAL);
    return mp_obj_new_str_from_cstr(ip);
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_inet_ntop_obj, socket_inet_ntop);

static const mp_rom_map_elem_t socket_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_socket) },
    { MP_ROM_QSTR(MP_QSTR_socket), MP_ROM_PTR(&py_type_socket) },
    { MP_ROM_QSTR(MP_QSTR_getaddrinfo), MP_ROM_PTR(&socket_getaddrinfo_obj) },
    { MP_ROM_QSTR(MP_QSTR_inet_pton), MP_ROM_PTR(&socket_inet_pton_obj) },
    { MP_ROM_QSTR(MP_QSTR_inet_ntop), MP_ROM_PTR(&socket_inet_ntop_obj) },
    { MP_ROM_QSTR(MP_QSTR_error), MP_ROM_PTR(&mp_type_OSError) },
    { MP_ROM_QSTR(MP_QSTR_timeout), MP_ROM_PTR(&mp_type_OSError) },
    { MP_ROM_QSTR(MP_QSTR_AF_INET), MP_ROM_INT(AF_INET_) },
    { MP_ROM_QSTR(MP_QSTR_AF_INET6), MP_ROM_INT(AF_INET6_) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_STREAM), MP_ROM_INT(SOCK_STREAM_) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_DGRAM), MP_ROM_INT(SOCK_DGRAM_) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_RAW), MP_ROM_INT(SOCK_RAW_) },
    { MP_ROM_QSTR(MP_QSTR_IPPROTO_IP), MP_ROM_INT(IPPROTO_IP_) },
    { MP_ROM_QSTR(MP_QSTR_IPPROTO_TCP), MP_ROM_INT(IPPROTO_TCP_) },
    { MP_ROM_QSTR(MP_QSTR_IPPROTO_UDP), MP_ROM_INT(IPPROTO_UDP_) },
    { MP_ROM_QSTR(MP_QSTR_SOL_SOCKET), MP_ROM_INT(SOL_SOCKET_) },
    { MP_ROM_QSTR(MP_QSTR_SO_REUSEADDR), MP_ROM_INT(SO_REUSEADDR_) },
    { MP_ROM_QSTR(MP_QSTR_SO_BROADCAST), MP_ROM_INT(SO_BROADCAST_) },
    { MP_ROM_QSTR(MP_QSTR_SO_KEEPALIVE), MP_ROM_INT(SO_KEEPALIVE_) },
    { MP_ROM_QSTR(MP_QSTR_TCP_NODELAY), MP_ROM_INT(TCP_NODELAY_) },
    { MP_ROM_QSTR(MP_QSTR_IP_TTL), MP_ROM_INT(IP_TTL_) },
    { MP_ROM_QSTR(MP_QSTR_IP_ADD_MEMBERSHIP), MP_ROM_INT(IP_ADD_MEMBERSHIP_) },
    { MP_ROM_QSTR(MP_QSTR_IP_DROP_MEMBERSHIP), MP_ROM_INT(IP_DROP_MEMBERSHIP_) },
    { MP_ROM_QSTR(MP_QSTR_IP_MULTICAST_TTL), MP_ROM_INT(IP_MULTICAST_TTL_) },
    { MP_ROM_QSTR(MP_QSTR_IP_MULTICAST_LOOP), MP_ROM_INT(IP_MULTICAST_LOOP_) },
};
static MP_DEFINE_CONST_DICT(socket_globals, socket_globals_table);

const mp_obj_module_t py_mod_socket = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&socket_globals };
MP_REGISTER_MODULE(MP_QSTR_socket, py_mod_socket);
