/* SPDX-License-Identifier: Apache-2.0
 * t5.net (plan 8.3): Wi-Fi status and the network stack of t5_net/sock.h,
 * http.h and ws.h for apps with the net permission. Raw lwIP sockets are not
 * exposed: everything goes through tracked handles that close when the app
 * exits, and through the same per-app handle limit as native apps.
 *
 *   r = t5.net.get("https://api.example.com/v1/items", headers={"Authorization": "Bearer x"})
 *   r.status, r.headers["content-type"], r.json()
 *   r = t5.net.post(url, json={"temp": 21.5})
 *   s = t5.net.tcp("example.com", 443, tls=True); s.send(b"..."); s.recv(1024)
 *   u = t5.net.udp(5000, broadcast=True); u.sendto(b"hi", ("255.255.255.255", 5000))
 *   ws = t5.net.websocket("wss://echo.example.com"); ws.send("hi"); ws.recv(5000)
 *   srv = t5.net.listen(8080); client, addr = srv.accept()
 *   ready = t5.net.poll([ws, u], 1000)
 *
 * Timeouts are milliseconds; None (the default for receives) waits until
 * something happens. A receive that times out returns None; b"" means the
 * peer closed. Waits run in short slices with callbacks, Ctrl-C and Stop
 * handled in between. Connects and lookups aren't sliced, but the VM's stop
 * flag is the task's network cancel flag, so Stop still ends them.
 */
#include <string.h>
#include <strings.h>
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/objtuple.h"
#include "t5_net/http.h"
#include "t5_net/httpd.h"
#include "t5_net/mqtt.h"
#include "t5_net/net.h"
#include "t5_net/sock.h"
#include "t5_net/ws.h"
#include "bind_perm.h"
#include "py_net.h"

#define COMPAT_BODY_MAX (512 * 1024)
#define MAX_HEADERS 32

typedef struct {
    mp_obj_base_t base;
    void *h;                    /* t5_sock_t / t5_http_t / t5_ws_t */
    uint32_t id;                /* t5_net_handle_id at open: pointers get reused */
    int32_t timeout;            /* default for waits, -1 = none */
    mp_obj_t status, headers, url, content;     /* Response */
    bool closed;                                 /* WebSocket: the close arrived */
    int close_code;
    mp_obj_t close_reason;
} py_net_t;

static const mp_obj_type_t py_type_socket, py_type_server, py_type_udp, py_type_response, py_type_websocket;

void py_net_reset(void)
{
    /* Stop ends a connect or DNS lookup the VM task is blocked in. */
    if (py_vm) t5_net_cancel_flag(&py_vm->stop);
}

static py_net_t *new_obj(const mp_obj_type_t *type)
{
    py_net_t *o = mp_obj_malloc_with_finaliser(py_net_t, type);
    o->h = NULL;
    o->id = 0;
    o->timeout = -1;
    o->status = o->headers = o->url = o->content = o->close_reason = mp_const_none;
    o->closed = false;
    o->close_code = 0;
    return o;
}

static void adopt(py_net_t *o, void *h)
{
    o->h = h;
    o->id = t5_net_handle_id(h);
}

static bool alive(const py_net_t *o)
{
    return py_net_alive(o->h, o->id);
}

static py_net_t *live(mp_obj_t self)
{
    py_net_t *o = MP_OBJ_TO_PTR(self);
    if (!alive(o)) mp_raise_ValueError(MP_ERROR_TEXT("closed"));
    return o;
}

static MP_NORETURN void fail(int e)
{
    py_net_fail(e, PY_ERRNO_GENERIC);
}

static int32_t timeout_arg(mp_obj_t a, int32_t dflt)
{
    if (a == mp_const_none || a == MP_OBJ_NULL) return dflt;
    mp_int_t v = mp_obj_get_int(a);
    return v < 0 ? -1 : (int32_t)v;
}

static mp_obj_t close_common(mp_obj_t self, t5_err_t (*fn)(void *))
{
    py_net_t *o = MP_OBJ_TO_PTR(self);
    if (alive(o)) T5_IGNORE(fn(o->h));    /* finaliser too: nothing to report to */
    o->h = NULL;
    o->id = 0;
    return mp_const_none;
}

static t5_err_t close_sock(void *h)
{
    return t5_sock_close(h);
}

static mp_obj_t net_exit(size_t n, const mp_obj_t *args)
{
    (void)n;
    mp_obj_t m[2];
    mp_load_method(args[0], MP_QSTR_close, m);
    return mp_call_method_n_kw(0, 0, m);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(net_exit_obj, 4, 4, net_exit);

/* ---- status, DNS ---------------------------------------------------------------------------- */
static mp_obj_t net_status(void)
{
    t5_net_status_t s;
    t5_net_status(&s);
    mp_obj_t d = mp_obj_new_dict(10);
    char ip[16];
    snprintf(ip, sizeof ip, "%u.%u.%u.%u", s.ip[0], s.ip[1], s.ip[2], s.ip[3]);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_state), mp_obj_new_str_from_cstr(t5_net_state_name(s.state)));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_connected), mp_obj_new_bool(s.state == T5_NET_CONNECTED));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_ssid), mp_obj_new_str_from_cstr(s.ssid));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rssi), MP_OBJ_NEW_SMALL_INT(s.rssi));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_ip), mp_obj_new_str_from_cstr(ip));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_time_synced), mp_obj_new_bool(s.sntp_synced));
    const uint8_t *more[3] = { s.gw, s.mask, s.dns };
    const qstr names[3] = { MP_QSTR_gw, MP_QSTR_mask, MP_QSTR_dns };
    for (int i = 0; i < 3; i++) {
        snprintf(ip, sizeof ip, "%u.%u.%u.%u", more[i][0], more[i][1], more[i][2], more[i][3]);
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(names[i]), mp_obj_new_str_from_cstr(ip));
    }
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_channel), MP_OBJ_NEW_SMALL_INT(s.channel));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(net_status_obj, net_status);

static mp_obj_t net_connected(void)
{
    t5_net_status_t s;
    t5_net_status(&s);
    return mp_obj_new_bool(s.state == T5_NET_CONNECTED);
}
static MP_DEFINE_CONST_FUN_OBJ_0(net_connected_obj, net_connected);

static mp_obj_t net_resolve(size_t n, const mp_obj_t *args)
{
    py_need_perm(T5G_PERM_t5_net_resolve);
    int32_t to = n > 1 ? timeout_arg(args[1], PY_NET_CONNECT_MS) : PY_NET_CONNECT_MS;
    t5_net_addr_t a;
    t5_err_t e = t5_net_resolve(mp_obj_str_get_str(args[0]), &a, to < 0 ? T5_NET_FOREVER : (uint32_t)to);
    if (e != T5_OK) fail(e);
    char ip[48];
    if (t5_net_addr_str(&a, ip, sizeof ip) < 0) fail(T5_ERR_FAIL);
    return mp_obj_new_str_from_cstr(ip);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(net_resolve_obj, 1, 2, net_resolve);

/* ---- TCP sockets -------------------------------------------------------------------------- */
typedef struct {
    const uint8_t *p;
    size_t n, done;
} send_ctx_t;

static int step_send(void *h, void *c, uint32_t ms)
{
    send_ctx_t *s = c;
    int r = t5_sock_send(h, s->p + s->done, s->n - s->done, ms);
    if (r > 0) s->done += (size_t)r;
    if (r >= 0 && s->done < s->n) return T5_ERR_TIMEOUT;   /* more to go */
    return r < 0 ? r : (int)s->done;
}

static mp_obj_t sock_send(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    mp_buffer_info_t b;
    mp_get_buffer_raise(args[1], &b, MP_BUFFER_READ);
    send_ctx_t c = { b.buf, b.len, 0 };
    int r = b.len ? py_net_sliced(o->h, step_send, &c, timeout_arg(n > 2 ? args[2] : MP_OBJ_NULL, o->timeout)) : 0;
    if (r == T5_ERR_TIMEOUT && c.done) r = (int)c.done;
    if (r < 0) fail(r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_send_obj, 2, 3, sock_send);

typedef struct {
    uint8_t *buf;
    size_t n;
} recv_ctx_t;

static int step_recv(void *h, void *c, uint32_t ms)
{
    recv_ctx_t *r = c;
    return t5_sock_recv(h, r->buf, r->n, ms);
}

static mp_obj_t sock_recv(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    mp_int_t want = n > 1 ? mp_obj_get_int(args[1]) : 4096;
    if (want <= 0) mp_raise_ValueError(MP_ERROR_TEXT("size must be positive"));
    vstr_t v;
    vstr_init_len(&v, (size_t)want);
    recv_ctx_t c = { (uint8_t *)v.buf, (size_t)want };
    int r = py_net_sliced(o->h, step_recv, &c, timeout_arg(n > 2 ? args[2] : MP_OBJ_NULL, o->timeout));
    if (r == T5_ERR_TIMEOUT) {
        vstr_clear(&v);
        return mp_const_none;
    }
    if (r < 0) fail(r);
    v.len = (size_t)r;
    return mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sock_recv_obj, 1, 3, sock_recv);

static mp_obj_t sock_pending(mp_obj_t self)
{
    int r = t5_sock_pending(live(self)->h);
    if (r < 0) fail(r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_pending_obj, sock_pending);

static mp_obj_t sock_shutdown(mp_obj_t self)
{
    t5_err_t e = t5_sock_shutdown(live(self)->h);
    if (e != T5_OK) fail(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_shutdown_obj, sock_shutdown);

static mp_obj_t sock_settimeout(mp_obj_t self, mp_obj_t t)
{
    py_net_t *o = MP_OBJ_TO_PTR(self);
    o->timeout = timeout_arg(t, -1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(sock_settimeout_obj, sock_settimeout);

static mp_obj_t sock_addr(mp_obj_t self, bool peer)
{
    t5_net_addr_t a;
    t5_err_t e = peer ? t5_sock_peer_addr(live(self)->h, &a) : t5_sock_local_addr(live(self)->h, &a);
    if (e != T5_OK) fail(e);
    return py_net_addr_tuple(&a);
}

static mp_obj_t sock_peer(mp_obj_t self)
{
    return sock_addr(self, true);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_peer_obj, sock_peer);

static mp_obj_t sock_local(mp_obj_t self)
{
    return sock_addr(self, false);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_local_obj, sock_local);

static mp_obj_t sock_close(mp_obj_t self)
{
    return close_common(self, close_sock);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sock_close_obj, sock_close);

static void net_print(const mp_print_t *print, mp_obj_t self, mp_print_kind_t kind)
{
    (void)kind;
    py_net_t *o = MP_OBJ_TO_PTR(self);
    mp_printf(print, "<%q%s>", (qstr)mp_obj_get_type(self)->name, alive(o) ? "" : " closed");
}

static const mp_rom_map_elem_t net_socket_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&sock_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendall), MP_ROM_PTR(&sock_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&sock_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_pending), MP_ROM_PTR(&sock_pending_obj) },
    { MP_ROM_QSTR(MP_QSTR_shutdown), MP_ROM_PTR(&sock_shutdown_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_peer), MP_ROM_PTR(&sock_peer_obj) },
    { MP_ROM_QSTR(MP_QSTR_local), MP_ROM_PTR(&sock_local_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&sock_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&sock_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&net_exit_obj) },
};
static MP_DEFINE_CONST_DICT(net_socket_locals, net_socket_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_socket, MP_QSTR_Socket, MP_TYPE_FLAG_NONE, print, net_print, locals_dict,
                                &net_socket_locals);

/* tcp(host, port, *, tls=False, verify=True, ca=None, server_hostname=None, timeout=10000, nodelay=True) */
static mp_obj_t net_tcp(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_host, A_port, A_tls, A_verify, A_ca, A_sni, A_timeout, A_nodelay };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_host, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_port, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_tls, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_verify, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_ca, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_server_hostname, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = PY_NET_CONNECT_MS } },
        { MP_QSTR_nodelay, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_need_perm(T5G_PERM_t5_tcp_connect);
    t5_tcp_cfg_t cfg = { .timeout_ms = a[A_timeout].u_int > 0 ? (uint32_t)a[A_timeout].u_int : PY_NET_CONNECT_MS };
    if (a[A_tls].u_bool) cfg.flags |= a[A_verify].u_bool ? T5_TCP_TLS : T5_TCP_TLS_NO_VERIFY;
    if (a[A_nodelay].u_bool) cfg.flags |= T5_TCP_NODELAY;
    if (a[A_ca].u_obj != mp_const_none) cfg.ca_pem = mp_obj_str_get_str(a[A_ca].u_obj);
    if (a[A_sni].u_obj != mp_const_none) cfg.sni = mp_obj_str_get_str(a[A_sni].u_obj);
    py_net_t *o = new_obj(&py_type_socket);
    t5_sock_t *s;
    t5_err_t e = t5_tcp_connect(mp_obj_str_get_str(a[A_host].u_obj), py_net_port_arg(MP_OBJ_NEW_SMALL_INT(a[A_port].u_int)),
                                &cfg, &s);
    if (e != T5_OK) fail(e);
    adopt(o, s);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(net_tcp_obj, 2, net_tcp);

/* ---- TCP servers ---------------------------------------------------------------------------- */
typedef struct {
    t5_sock_t *got;
} accept_ctx_t;

static int step_accept(void *h, void *c, uint32_t ms)
{
    return t5_tcp_accept(h, ms, &((accept_ctx_t *)c)->got);
}

static mp_obj_t server_accept(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    py_net_t *client = new_obj(&py_type_socket);     /* first, so a Stop can't leak the socket */
    accept_ctx_t c = { NULL };
    int r = py_net_sliced(o->h, step_accept, &c, timeout_arg(n > 1 ? args[1] : MP_OBJ_NULL, o->timeout));
    if (r == T5_ERR_TIMEOUT) return mp_const_none;
    if (r < 0) fail(r);
    adopt(client, c.got);
    t5_net_addr_t peer;
    mp_obj_t addr = t5_sock_peer_addr(c.got, &peer) == T5_OK ? py_net_addr_tuple(&peer) : mp_const_none;
    mp_obj_t t[2] = { MP_OBJ_FROM_PTR(client), addr };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(server_accept_obj, 1, 2, server_accept);

static const mp_rom_map_elem_t net_server_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_accept), MP_ROM_PTR(&server_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_local), MP_ROM_PTR(&sock_local_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&sock_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&sock_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&net_exit_obj) },
};
static MP_DEFINE_CONST_DICT(net_server_locals, net_server_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_server, MP_QSTR_Server, MP_TYPE_FLAG_NONE, print, net_print, locals_dict,
                                &net_server_locals);

static mp_obj_t net_listen(size_t n, const mp_obj_t *args)
{
    py_need_perm(T5G_PERM_t5_tcp_listen);
    uint16_t port = n > 0 ? py_net_port_arg(args[0]) : 0;
    int backlog = n > 1 ? (int)mp_obj_get_int(args[1]) : 4;
    py_net_t *o = new_obj(&py_type_server);
    t5_sock_t *s;
    t5_err_t e = t5_tcp_listen(port, backlog, &s);
    if (e != T5_OK) fail(e);
    adopt(o, s);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(net_listen_obj, 0, 2, net_listen);

/* ---- UDP ------------------------------------------------------------------------------------ */
typedef struct {
    const t5_net_addr_t *to;
    const void *p;
    size_t n;
} sendto_ctx_t;

static int step_sendto(void *h, void *c, uint32_t ms)
{
    sendto_ctx_t *s = c;
    return t5_udp_sendto(h, s->to, s->p, s->n, ms);
}

static mp_obj_t udp_sendto(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    mp_buffer_info_t b;
    mp_get_buffer_raise(args[1], &b, MP_BUFFER_READ);
    t5_net_addr_t to;
    py_net_addr_arg(args[2], &to, PY_ERRNO_GENERIC);
    sendto_ctx_t c = { &to, b.buf, b.len };
    int r = py_net_sliced(o->h, step_sendto, &c, timeout_arg(n > 3 ? args[3] : MP_OBJ_NULL, o->timeout));
    if (r < 0) fail(r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(udp_sendto_obj, 3, 4, udp_sendto);

typedef struct {
    uint8_t *buf;
    size_t n;
    t5_net_addr_t from;
} recvfrom_ctx_t;

static int step_recvfrom(void *h, void *c, uint32_t ms)
{
    recvfrom_ctx_t *r = c;
    return t5_udp_recvfrom(h, r->buf, r->n, &r->from, ms);
}

static mp_obj_t udp_recvfrom(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    mp_int_t want = n > 1 ? mp_obj_get_int(args[1]) : 1500;
    if (want <= 0 || want > 65507) mp_raise_ValueError(MP_ERROR_TEXT("size is 1-65507"));
    vstr_t v;
    vstr_init_len(&v, (size_t)want);
    recvfrom_ctx_t c = { (uint8_t *)v.buf, (size_t)want, { 0 } };
    int r = py_net_sliced(o->h, step_recvfrom, &c, timeout_arg(n > 2 ? args[2] : MP_OBJ_NULL, o->timeout));
    if (r == T5_ERR_TIMEOUT) {
        vstr_clear(&v);
        return mp_const_none;
    }
    if (r < 0) fail(r);
    v.len = (size_t)r;
    mp_obj_t t[2] = { mp_obj_new_bytes_from_vstr(&v), py_net_addr_tuple(&c.from) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(udp_recvfrom_obj, 1, 3, udp_recvfrom);

static mp_obj_t udp_connect(mp_obj_t self, mp_obj_t addr)
{
    py_net_t *o = live(self);
    t5_net_addr_t to;
    py_net_addr_arg(addr, &to, PY_ERRNO_GENERIC);
    t5_err_t e = t5_udp_connect(o->h, &to);
    if (e != T5_OK) fail(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(udp_connect_obj, udp_connect);

static mp_obj_t udp_join(mp_obj_t self, mp_obj_t group)
{
    t5_err_t e = t5_udp_join(live(self)->h, mp_obj_str_get_str(group));
    if (e != T5_OK) fail(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(udp_join_obj, udp_join);

static mp_obj_t udp_leave(mp_obj_t self, mp_obj_t group)
{
    t5_err_t e = t5_udp_leave(live(self)->h, mp_obj_str_get_str(group));
    if (e != T5_OK) fail(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(udp_leave_obj, udp_leave);

static mp_obj_t udp_multicast(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    mp_int_t ttl = n > 1 ? mp_obj_get_int(args[1]) : 1;
    if (ttl < 1 || ttl > 255) mp_raise_ValueError(MP_ERROR_TEXT("ttl is 1-255"));
    t5_err_t e = t5_udp_multicast_opts(o->h, (uint8_t)ttl, n > 2 && mp_obj_is_true(args[2]));
    if (e != T5_OK) fail(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(udp_multicast_obj, 1, 3, udp_multicast);

static const mp_rom_map_elem_t net_udp_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_sendto), MP_ROM_PTR(&udp_sendto_obj) },
    { MP_ROM_QSTR(MP_QSTR_recvfrom), MP_ROM_PTR(&udp_recvfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&udp_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&sock_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&sock_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_join), MP_ROM_PTR(&udp_join_obj) },
    { MP_ROM_QSTR(MP_QSTR_leave), MP_ROM_PTR(&udp_leave_obj) },
    { MP_ROM_QSTR(MP_QSTR_multicast), MP_ROM_PTR(&udp_multicast_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_local), MP_ROM_PTR(&sock_local_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&sock_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&sock_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&net_exit_obj) },
};
static MP_DEFINE_CONST_DICT(net_udp_locals, net_udp_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_udp, MP_QSTR_UDP, MP_TYPE_FLAG_NONE, print, net_print, locals_dict,
                                &net_udp_locals);

/* udp(port=0, *, broadcast=False, reuse=False, ipv6=False) */
static mp_obj_t net_udp(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_port, A_broadcast, A_reuse, A_ipv6 };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_port, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_broadcast, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_reuse, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_ipv6, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_need_perm(T5G_PERM_t5_udp_open);
    uint32_t flags = (a[A_broadcast].u_bool ? T5_UDP_BROADCAST : 0) | (a[A_reuse].u_bool ? T5_UDP_REUSE : 0) |
                     (a[A_ipv6].u_bool ? T5_UDP_IPV6 : 0);
    py_net_t *o = new_obj(&py_type_udp);
    t5_sock_t *s;
    t5_err_t e = t5_udp_open(py_net_port_arg(MP_OBJ_NEW_SMALL_INT(a[A_port].u_int)), flags, &s);
    if (e != T5_OK) fail(e);
    adopt(o, s);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(net_udp_obj, 0, net_udp);

/* ---- HTTP ------------------------------------------------------------------------------------ */
static mp_obj_t json_call(qstr fn, mp_obj_t arg)
{
    mp_obj_t mod = mp_import_name(MP_QSTR_json, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    return mp_call_function_1(mp_load_attr(mod, fn), arg);
}

static t5_err_t close_http(void *h)
{
    return t5_http_close(h);
}

static mp_obj_t resp_close(mp_obj_t self)
{
    return close_common(self, close_http);
}
static MP_DEFINE_CONST_FUN_OBJ_1(resp_close_obj, resp_close);

typedef struct {
    vstr_t *v;
    size_t max;
} body_ctx_t;

static int step_body(void *h, void *c, uint32_t ms)
{
    body_ctx_t *b = c;
    if (b->v->len >= b->max) return T5_ERR_NO_MEM;
    size_t room = b->max - b->v->len < 4096 ? b->max - b->v->len : 4096;
    char *dst = vstr_add_len(b->v, room);
    int r = t5_http_read(h, dst, room, ms);
    b->v->len -= room - (r > 0 ? (size_t)r : 0);
    if (r > 0) return T5_ERR_TIMEOUT;           /* got some: keep going */
    return r;                                    /* 0 = end of body */
}

/* Read up to max more bytes of the body (all of it for SIZE_MAX). */
static mp_obj_t read_body(py_net_t *o, size_t max, int32_t timeout_ms, bool all)
{
    vstr_t v;
    vstr_init(&v, 1024);
    body_ctx_t c = { &v, max };
    for (;;) {
        /* Each slice of data restarts the wait: the timeout is per pause, not the total. */
        size_t before = v.len;
        int r = py_net_sliced(o->h, step_body, &c, timeout_ms);
        if (r == 0 || (r == T5_ERR_NO_MEM && !all) || (!all && v.len)) break;
        if (r == T5_ERR_TIMEOUT && v.len > before) continue;
        if (r == T5_ERR_NO_MEM) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("response too big"));
        if (r == T5_ERR_TIMEOUT && !v.len && !all) {
            vstr_clear(&v);
            return mp_const_none;
        }
        fail(r);
    }
    return mp_obj_new_bytes_from_vstr(&v);
}

static mp_obj_t resp_content(py_net_t *o)
{
    if (o->content == mp_const_none && alive(o)) {
        o->content = read_body(o, (size_t)-1 >> 1, o->timeout, true);
        resp_close(MP_OBJ_FROM_PTR(o));
    }
    return o->content == mp_const_none ? mp_const_empty_bytes : o->content;
}

/* read(n=-1, timeout=None): up to n more body bytes (-1: the rest); b"" at the end. */
static mp_obj_t resp_read(size_t n, const mp_obj_t *args)
{
    py_net_t *o = MP_OBJ_TO_PTR(args[0]);
    mp_int_t want = n > 1 ? mp_obj_get_int(args[1]) : -1;
    if (want < 0) return resp_content(o);
    if (o->content != mp_const_none || !alive(o) || want == 0) return mp_const_empty_bytes;
    mp_obj_t r = read_body(o, (size_t)want, timeout_arg(n > 2 ? args[2] : MP_OBJ_NULL, o->timeout), false);
    return r;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(resp_read_obj, 1, 3, resp_read);

static mp_obj_t resp_text(mp_obj_t self)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(resp_content(MP_OBJ_TO_PTR(self)), &b, MP_BUFFER_READ);
    return mp_obj_new_str(b.buf, b.len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(resp_text_obj, resp_text);

static mp_obj_t resp_json(mp_obj_t self)
{
    return json_call(MP_QSTR_loads, resp_content(MP_OBJ_TO_PTR(self)));
}
static MP_DEFINE_CONST_FUN_OBJ_1(resp_json_obj, resp_json);

static void resp_attr(mp_obj_t self, qstr attr, mp_obj_t *dest)
{
    py_net_t *o = MP_OBJ_TO_PTR(self);
    if (dest[0] != MP_OBJ_NULL) return;          /* read-only */
    switch (attr) {
    case MP_QSTR_status: dest[0] = o->status; break;
    case MP_QSTR_headers: dest[0] = o->headers; break;
    case MP_QSTR_url: dest[0] = o->url; break;
    case MP_QSTR_ok: {
        mp_int_t st = mp_obj_get_int(o->status);
        dest[0] = mp_obj_new_bool(st >= 200 && st < 300);
        break;
    }
    case MP_QSTR_content: dest[0] = resp_content(o); break;
    default: dest[1] = MP_OBJ_SENTINEL; break;   /* methods */
    }
}

static void resp_print(const mp_print_t *print, mp_obj_t self, mp_print_kind_t kind)
{
    (void)kind;
    py_net_t *o = MP_OBJ_TO_PTR(self);
    mp_printf(print, "<Response %d>", (int)mp_obj_get_int(o->status));
}

static const mp_rom_map_elem_t net_response_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&resp_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&resp_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_json), MP_ROM_PTR(&resp_json_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&resp_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&resp_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&net_exit_obj) },
};
static MP_DEFINE_CONST_DICT(net_response_locals, net_response_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_response, MP_QSTR_Response, MP_TYPE_FLAG_NONE, print, resp_print, attr,
                                resp_attr, locals_dict, &net_response_locals);

/* headers=dict or [(name, value)] -> hdr[] (pointers into the objects). */
static size_t headers_arg(mp_obj_t h, t5_http_header_t *out, size_t max)
{
    if (h == mp_const_none) return 0;
    size_t n = 0;
    if (mp_obj_is_type(h, &mp_type_dict)) {
        mp_map_t *m = mp_obj_dict_get_map(h);
        for (size_t i = 0; i < m->alloc; i++) {
            if (!mp_map_slot_is_filled(m, i)) continue;
            if (n == max) mp_raise_ValueError(MP_ERROR_TEXT("too many headers"));
            out[n].name = mp_obj_str_get_str(m->table[i].key);
            out[n].value = mp_obj_str_get_str(m->table[i].value);
            n++;
        }
        return n;
    }
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(h, &len, &items);
    for (size_t i = 0; i < len; i++) {
        size_t pl;
        mp_obj_t *pair;
        mp_obj_get_array(items[i], &pl, &pair);
        if (pl != 2) mp_raise_TypeError(MP_ERROR_TEXT("headers are (name, value) pairs"));
        if (n == max) mp_raise_ValueError(MP_ERROR_TEXT("too many headers"));
        out[n].name = mp_obj_str_get_str(pair[0]);
        out[n].value = mp_obj_str_get_str(pair[1]);
        n++;
    }
    return n;
}

static bool has_header(const t5_http_header_t *h, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++) {
        if (!strcasecmp(h[i].name, name)) return true;
    }
    return false;
}

/* The response headers as a dict with lower-case names; repeats joined by ", ". */
static mp_obj_t headers_dict(t5_http_t *h)
{
    mp_obj_t d = mp_obj_new_dict(8);
    const char *name, *value;
    for (int i = 0; t5_http_header_at(h, i, &name, &value) == T5_OK; i++) {
        vstr_t k;
        vstr_init(&k, strlen(name) + 1);
        for (const char *p = name; *p; p++) vstr_add_byte(&k, (byte)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p));
        mp_obj_t key = mp_obj_new_str_from_vstr(&k);
        mp_obj_t val = mp_obj_new_str_from_cstr(value);
        mp_map_elem_t *e = mp_map_lookup(mp_obj_dict_get_map(d), key, MP_MAP_LOOKUP);
        if (e) {
            vstr_t j;
            vstr_init(&j, 64);
            vstr_add_str(&j, mp_obj_str_get_str(e->value));
            vstr_add_str(&j, ", ");
            vstr_add_str(&j, value);
            val = mp_obj_new_str_from_vstr(&j);
        }
        mp_obj_dict_store(d, key, val);
    }
    return d;
}

enum { R_url, R_data, R_json, R_headers, R_timeout, R_redirects, R_verify, R_ca, R_stream };
static const mp_arg_t request_args[] = {
    { MP_QSTR_url, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    { MP_QSTR_data, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    { MP_QSTR_json, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    { MP_QSTR_headers, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = PY_NET_CONNECT_MS } },
    { MP_QSTR_redirects, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    { MP_QSTR_verify, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    { MP_QSTR_ca, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    { MP_QSTR_stream, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
};

static mp_obj_t do_request(const char *method, mp_arg_val_t *a, size_t body_max)
{
    py_need_perm(T5G_PERM_t5_http_open);
    t5_http_header_t hdr[MAX_HEADERS + 1];
    size_t nh = headers_arg(a[R_headers].u_obj, hdr, MAX_HEADERS);
    t5_http_req_t req = { .method = method, .url = mp_obj_str_get_str(a[R_url].u_obj), .headers = hdr };
    mp_obj_t body = a[R_data].u_obj;
    if (a[R_json].u_obj != mp_const_none) {
        body = json_call(MP_QSTR_dumps, a[R_json].u_obj);
        if (!has_header(hdr, nh, "Content-Type")) hdr[nh++] = (t5_http_header_t){ "Content-Type", "application/json" };
    }
    if (body != mp_const_none) {
        mp_buffer_info_t b;
        mp_get_buffer_raise(body, &b, MP_BUFFER_READ);
        req.body = b.len ? b.buf : "";
        req.body_len = b.len;
    }
    req.n_headers = nh;
    req.timeout_ms = a[R_timeout].u_int > 0 ? (uint32_t)a[R_timeout].u_int : PY_NET_CONNECT_MS;
    req.flags = (a[R_redirects].u_bool ? 0 : T5_HTTP_NO_REDIRECT) | (a[R_verify].u_bool ? 0 : T5_HTTP_TLS_NO_VERIFY);
    if (a[R_ca].u_obj != mp_const_none) req.ca_pem = mp_obj_str_get_str(a[R_ca].u_obj);
    py_net_t *o = new_obj(&py_type_response);
    o->timeout = (int32_t)req.timeout_ms;
    t5_http_t *h;
    t5_err_t e = t5_http_open(&req, &h);
    if (e != T5_OK) fail(e);
    adopt(o, h);
    o->status = MP_OBJ_NEW_SMALL_INT(t5_http_status(h));
    o->url = mp_obj_new_str_from_cstr(t5_http_url(h));
    o->headers = headers_dict(h);
    if (!a[R_stream].u_bool) {
        o->content = read_body(o, body_max, o->timeout, true);
        resp_close(MP_OBJ_FROM_PTR(o));
    }
    return MP_OBJ_FROM_PTR(o);
}

/* request(method, url, data=None, *, json=None, headers=None, timeout=10000,
 *         redirects=True, verify=True, ca=None, stream=False) */
static mp_obj_t net_request(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    if (n_args < 1) mp_raise_TypeError(MP_ERROR_TEXT("request(method, url, ...)"));
    mp_arg_val_t a[MP_ARRAY_SIZE(request_args)];
    mp_arg_parse_all(n_args - 1, pos + 1, kw, MP_ARRAY_SIZE(request_args), request_args, a);
    vstr_t m;
    vstr_init(&m, 8);
    for (const char *p = mp_obj_str_get_str(pos[0]); *p; p++) vstr_add_byte(&m, (byte)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p));
    return do_request(vstr_null_terminated_str(&m), a, (size_t)-1 >> 1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(net_request_obj, 2, net_request);

#define METHOD_FN(fname, METHOD)                                                                   \
    static mp_obj_t net_##fname(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)                  \
    {                                                                                              \
        mp_arg_val_t a[MP_ARRAY_SIZE(request_args)];                                               \
        mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(request_args), request_args, a);           \
        return do_request(METHOD, a, (size_t)-1 >> 1);                                             \
    }                                                                                              \
    static MP_DEFINE_CONST_FUN_OBJ_KW(net_##fname##_obj, 1, net_##fname)

METHOD_FN(get, "GET");
METHOD_FN(post, "POST");
METHOD_FN(put, "PUT");
METHOD_FN(patch, "PATCH");
METHOD_FN(delete, "DELETE");
METHOD_FN(head, "HEAD");

/* The original calls: (status, body) with a 512 KB limit. */
static mp_obj_t compat(const char *method, mp_obj_t url, mp_obj_t body, mp_obj_t ctype, mp_obj_t timeout)
{
    mp_arg_val_t a[MP_ARRAY_SIZE(request_args)];
    memset(a, 0, sizeof a);
    a[R_url].u_obj = url;
    a[R_data].u_obj = body;
    a[R_json].u_obj = mp_const_none;
    a[R_ca].u_obj = mp_const_none;
    a[R_redirects].u_bool = true;
    a[R_verify].u_bool = true;
    mp_int_t to = timeout == MP_OBJ_NULL ? PY_NET_CONNECT_MS : mp_obj_get_int(timeout);
    a[R_timeout].u_int = to <= 0 || to > 60000 ? PY_NET_CONNECT_MS : to;
    if (ctype != MP_OBJ_NULL) {
        mp_obj_t pair[2] = { mp_obj_new_str_from_cstr("Content-Type"), ctype };
        mp_obj_t list[1] = { mp_obj_new_tuple(2, pair) };
        a[R_headers].u_obj = mp_obj_new_list(1, list);
    } else {
        a[R_headers].u_obj = mp_const_none;
    }
    py_net_t *o = MP_OBJ_TO_PTR(do_request(method, a, COMPAT_BODY_MAX));
    mp_obj_t t[2] = { o->status, o->content };
    return mp_obj_new_tuple(2, t);
}

static mp_obj_t net_http_get(size_t n, const mp_obj_t *args)
{
    return compat("GET", args[0], mp_const_none, MP_OBJ_NULL, n > 1 ? args[1] : MP_OBJ_NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(net_http_get_obj, 1, 2, net_http_get);

static mp_obj_t net_http_post(size_t n, const mp_obj_t *args)
{
    mp_obj_t ctype = n > 2 ? args[2] : mp_obj_new_str_from_cstr("application/octet-stream");
    return compat("POST", args[0], args[1], ctype, n > 3 ? args[3] : MP_OBJ_NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(net_http_post_obj, 2, 4, net_http_post);

/* ---- WebSocket ------------------------------------------------------------------------------- */
static t5_err_t close_ws_quick(void *h)
{
    return t5_ws_close(h, 1001);
}

static mp_obj_t ws_del(mp_obj_t self)
{
    return close_common(self, close_ws_quick);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ws_del_obj, ws_del);

static mp_obj_t ws_close(size_t n, const mp_obj_t *args)
{
    py_net_t *o = MP_OBJ_TO_PTR(args[0]);
    mp_int_t code = n > 1 ? mp_obj_get_int(args[1]) : 1000;
    if (code < 0 || code > 4999) mp_raise_ValueError(MP_ERROR_TEXT("close code is 0-4999"));
    if (alive(o)) T5_IGNORE(t5_ws_close(o->h, (uint16_t)code));   /* closing either way */
    o->h = NULL;
    o->id = 0;
    o->closed = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ws_close_obj, 1, 2, ws_close);

/* Not sliced: a frame can't be resumed once part of it went out (the
 * connection would fail), so it gets the whole timeout in one call. Stop
 * still ends it through the VM's cancel flag (t5_net_cancel_flag). */
static mp_obj_t ws_send_op(py_net_t *o, t5_ws_op_t op, mp_obj_t data, mp_obj_t timeout)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    int32_t ms = timeout_arg(timeout, o->timeout);
    int r = t5_ws_send(o->h, op, b.buf, b.len, ms < 0 ? T5_NET_FOREVER : (uint32_t)ms);
    if (r < 0) fail(r);
    return MP_OBJ_NEW_SMALL_INT(r);
}

/* send(data, timeout=None): str goes as text, bytes as binary. */
static mp_obj_t ws_send(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    t5_ws_op_t op = mp_obj_is_str(args[1]) ? T5_WS_TEXT : T5_WS_BINARY;
    return ws_send_op(o, op, args[1], n > 2 ? args[2] : MP_OBJ_NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ws_send_obj, 2, 3, ws_send);

static mp_obj_t ws_ping(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    return ws_send_op(o, T5_WS_PING, n > 1 ? args[1] : mp_const_empty_bytes, MP_OBJ_NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ws_ping_obj, 1, 2, ws_ping);

typedef struct {
    t5_ws_op_t op;
} ws_wait_ctx_t;

/* Wait for a message without taking it: a zero-size buffer leaves it queued
 * (T5_ERR_NO_MEM), except an empty message, which is taken (0). */
static int step_ws_wait(void *h, void *c, uint32_t ms)
{
    return t5_ws_recv(h, NULL, 0, &((ws_wait_ctx_t *)c)->op, ms);
}

/* recv(timeout=None): str (text) or bytes (binary); None on timeout or once
 * the server closed (then .closed, .close_code and .close_reason are set). */
static mp_obj_t ws_recv(size_t n, const mp_obj_t *args)
{
    py_net_t *o = MP_OBJ_TO_PTR(args[0]);
    if (o->closed) return mp_const_none;
    live(args[0]);
    ws_wait_ctx_t c = { T5_WS_BINARY };
    int r = py_net_sliced(o->h, step_ws_wait, &c, timeout_arg(n > 1 ? args[1] : MP_OBJ_NULL, o->timeout));
    if (r == T5_ERR_TIMEOUT) return mp_const_none;
    vstr_t v;
    if (r == T5_ERR_NO_MEM) {
        int len = t5_ws_next_len(o->h);
        if (len < 0) fail(T5_ERR_IO);
        vstr_init_len(&v, (size_t)len);
        r = t5_ws_recv(o->h, v.buf, (size_t)len, &c.op, 0);
        if (r < 0) fail(r);
        v.len = (size_t)r;
    } else if (r == 0) {
        vstr_init(&v, 1);
    } else {
        fail(r);
    }
    if (c.op == T5_WS_CLOSE) {
        o->closed = true;
        o->close_code = v.len >= 2 ? ((uint8_t)v.buf[0] << 8) | (uint8_t)v.buf[1] : 1005;
        o->close_reason = mp_obj_new_str(v.len > 2 ? v.buf + 2 : "", v.len > 2 ? v.len - 2 : 0);
        vstr_clear(&v);
        return mp_const_none;
    }
    return c.op == T5_WS_TEXT ? mp_obj_new_str_from_vstr(&v) : mp_obj_new_bytes_from_vstr(&v);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ws_recv_obj, 1, 2, ws_recv);

static void ws_attr(mp_obj_t self, qstr attr, mp_obj_t *dest)
{
    py_net_t *o = MP_OBJ_TO_PTR(self);
    if (dest[0] != MP_OBJ_NULL) return;
    switch (attr) {
    case MP_QSTR_closed: dest[0] = mp_obj_new_bool(o->closed || !alive(o)); break;
    case MP_QSTR_close_code: dest[0] = o->close_code ? MP_OBJ_NEW_SMALL_INT(o->close_code) : mp_const_none; break;
    case MP_QSTR_close_reason: dest[0] = o->close_reason; break;
    case MP_QSTR_subprotocol: dest[0] = mp_obj_new_str_from_cstr(alive(o) ? t5_ws_subprotocol(o->h) : ""); break;
    default: dest[1] = MP_OBJ_SENTINEL; break;
    }
}

static const mp_rom_map_elem_t net_websocket_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&ws_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&ws_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_ping), MP_ROM_PTR(&ws_ping_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&ws_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&ws_del_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&net_exit_obj) },
};
static MP_DEFINE_CONST_DICT(net_websocket_locals, net_websocket_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_websocket, MP_QSTR_WebSocket, MP_TYPE_FLAG_NONE, print, net_print, attr,
                                ws_attr, locals_dict, &net_websocket_locals);

/* websocket(url, *, headers=None, protocols=None, timeout=10000, verify=True, ca=None, max_msg=65536) */
static mp_obj_t net_websocket(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_url, A_headers, A_protocols, A_timeout, A_verify, A_ca, A_max };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_url, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_headers, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_protocols, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = PY_NET_CONNECT_MS } },
        { MP_QSTR_verify, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_ca, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_max_msg, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 64 * 1024 } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_need_perm(T5G_PERM_t5_ws_connect);
    t5_http_header_t hdr[MAX_HEADERS];
    t5_ws_cfg_t cfg = { .headers = hdr, .n_headers = headers_arg(a[A_headers].u_obj, hdr, MAX_HEADERS) };
    mp_obj_t protos = a[A_protocols].u_obj;
    if (protos != mp_const_none && !mp_obj_is_str(protos)) {
        /* A list: join with ", ". */
        mp_obj_t sep = mp_obj_new_str_from_cstr(", ");
        protos = mp_call_function_1(mp_load_attr(sep, MP_QSTR_join), protos);
    }
    if (protos != mp_const_none) cfg.subprotocols = mp_obj_str_get_str(protos);
    cfg.timeout_ms = a[A_timeout].u_int > 0 ? (uint32_t)a[A_timeout].u_int : PY_NET_CONNECT_MS;
    cfg.flags = a[A_verify].u_bool ? 0 : T5_HTTP_TLS_NO_VERIFY;
    if (a[A_ca].u_obj != mp_const_none) cfg.ca_pem = mp_obj_str_get_str(a[A_ca].u_obj);
    cfg.max_msg = a[A_max].u_int > 0 ? (size_t)a[A_max].u_int : 0;
    py_net_t *o = new_obj(&py_type_websocket);
    t5_ws_t *ws;
    t5_err_t e = t5_ws_connect(mp_obj_str_get_str(a[A_url].u_obj), &cfg, &ws);
    if (e != T5_OK) fail(e);
    adopt(o, ws);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(net_websocket_obj, 1, net_websocket);


/* ---- MQTT -------------------------------------------------------------------------------------- */
static const mp_obj_type_t py_type_mqtt, py_type_httpd, py_type_request;

static t5_err_t close_mqtt(void *h)
{
    return t5_mqtt_close(h);
}

static mp_obj_t mqtt_close(mp_obj_t self)
{
    return close_common(self, close_mqtt);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mqtt_close_obj, mqtt_close);

/* publish(topic, msg, qos=0, retain=False) -> message id (0 for QoS 0) */
static mp_obj_t mqtt_publish(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_self, A_topic, A_msg, A_qos, A_retain };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_topic, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_msg, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_qos, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_retain, MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_net_t *o = live(a[A_self].u_obj);
    mp_buffer_info_t b;
    mp_get_buffer_raise(a[A_msg].u_obj, &b, MP_BUFFER_READ);
    int r = t5_mqtt_publish(o->h, mp_obj_str_get_str(a[A_topic].u_obj), b.buf, b.len, (int)a[A_qos].u_int,
                            a[A_retain].u_bool);
    if (r < 0) fail(r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mqtt_publish_obj, 3, mqtt_publish);

static mp_obj_t mqtt_subscribe_kw(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_self, A_topic, A_qos };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_topic, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_qos, MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    int r = t5_mqtt_subscribe(live(a[A_self].u_obj)->h, mp_obj_str_get_str(a[A_topic].u_obj), (int)a[A_qos].u_int);
    if (r < 0) fail(r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mqtt_subscribe_obj, 2, mqtt_subscribe_kw);

static mp_obj_t mqtt_unsubscribe(mp_obj_t self, mp_obj_t topic)
{
    int r = t5_mqtt_unsubscribe(live(self)->h, mp_obj_str_get_str(topic));
    if (r < 0) fail(r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_2(mqtt_unsubscribe_obj, mqtt_unsubscribe);

typedef struct {
    t5_mqtt_event_t ev;
} mqtt_wait_t;

static int step_mqtt(void *h, void *c, uint32_t ms)
{
    int need = t5_mqtt_next_size(h);
    if (need >= 0) return T5_OK;               /* one is waiting: take it outside */
    t5_err_t e = t5_mqtt_next(h, &((mqtt_wait_t *)c)->ev, NULL, 0, ms);
    return e == T5_ERR_NO_MEM ? T5_OK : e;
}

/* recv(timeout=None) -> event (.type, .topic, .payload, .qos, .retain, .msg_id), or None on timeout.
 * type: "message", "connected", "disconnected", "subscribed", "unsubscribed", "published", "error". */
static mp_obj_t mqtt_recv(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    mqtt_wait_t w;
    int r = py_net_sliced(o->h, step_mqtt, &w, timeout_arg(n > 1 ? args[1] : MP_OBJ_NULL, o->timeout));
    if (r == T5_ERR_TIMEOUT) return mp_const_none;
    if (r < 0) fail(r);
    int need = t5_mqtt_next_size(o->h);
    if (need < 0) return mp_const_none;
    vstr_t v;
    vstr_init_len(&v, (size_t)need);
    t5_mqtt_event_t ev;
    t5_err_t e = t5_mqtt_next(o->h, &ev, v.buf, (size_t)need, 0);
    if (e != T5_OK) {
        vstr_clear(&v);
        if (e == T5_ERR_TIMEOUT) return mp_const_none;
        fail(e);
    }
    static const qstr kinds[] = { MP_QSTR_, MP_QSTR_message, MP_QSTR_connected, MP_QSTR_disconnected,
                                  MP_QSTR_subscribed, MP_QSTR_unsubscribed, MP_QSTR_published, MP_QSTR_error };
    static const qstr fields[] = { MP_QSTR_type, MP_QSTR_topic, MP_QSTR_payload, MP_QSTR_qos, MP_QSTR_retain,
                                   MP_QSTR_msg_id };
    mp_obj_t items[6] = {
        MP_OBJ_NEW_QSTR(ev.type < MP_ARRAY_SIZE(kinds) ? kinds[ev.type] : MP_QSTR_),
        ev.type == T5_MQTT_EV_MESSAGE ? mp_obj_new_str_from_cstr(ev.topic) : mp_const_none,
        ev.type == T5_MQTT_EV_MESSAGE ? mp_obj_new_bytes(ev.data, ev.len) : mp_const_none,
        MP_OBJ_NEW_SMALL_INT(ev.qos), mp_obj_new_bool(ev.retain), MP_OBJ_NEW_SMALL_INT(ev.msg_id),
    };
    vstr_clear(&v);
    return mp_obj_new_attrtuple(fields, 6, items);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mqtt_recv_obj, 1, 2, mqtt_recv);

static void mqtt_attr(mp_obj_t self, qstr attr, mp_obj_t *dest)
{
    py_net_t *o = MP_OBJ_TO_PTR(self);
    if (dest[0] != MP_OBJ_NULL) return;
    if (attr == MP_QSTR_connected) dest[0] = mp_obj_new_bool(alive(o) && t5_mqtt_connected(o->h));
    else if (attr == MP_QSTR_dropped) dest[0] = mp_obj_new_int_from_uint(alive(o) ? t5_mqtt_dropped(o->h) : 0);
    else dest[1] = MP_OBJ_SENTINEL;
}

static const mp_rom_map_elem_t net_mqtt_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_publish), MP_ROM_PTR(&mqtt_publish_obj) },
    { MP_ROM_QSTR(MP_QSTR_subscribe), MP_ROM_PTR(&mqtt_subscribe_obj) },
    { MP_ROM_QSTR(MP_QSTR_unsubscribe), MP_ROM_PTR(&mqtt_unsubscribe_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&mqtt_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mqtt_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mqtt_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&net_exit_obj) },
};
static MP_DEFINE_CONST_DICT(net_mqtt_locals, net_mqtt_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_mqtt, MP_QSTR_MQTT, MP_TYPE_FLAG_NONE, print, net_print, attr, mqtt_attr,
                                locals_dict, &net_mqtt_locals);

static const char *opt_str(mp_obj_t o)
{
    return o == mp_const_none ? NULL : mp_obj_str_get_str(o);
}

/* mqtt(uri, *, client_id=None, user=None, password=None, keepalive=60, will=None, clean=True,
 *      mqtt5=False, verify=True, ca=None, cert=None, key=None, max_msg=65536, timeout=10000) */
static mp_obj_t net_mqtt(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_uri, A_id, A_user, A_pw, A_ka, A_will, A_clean, A_v5, A_verify, A_ca, A_cert, A_key, A_max, A_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_uri, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_client_id, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_user, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_password, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_keepalive, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 60 } },
        { MP_QSTR_will, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_clean, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_mqtt5, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_verify, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_ca, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_cert, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_key, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_max_msg, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 64 * 1024 } },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = PY_NET_CONNECT_MS } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_need_perm(T5G_PERM_t5_mqtt_connect);
    t5_mqtt_cfg_t cfg = {
        .uri = mp_obj_str_get_str(a[A_uri].u_obj), .client_id = opt_str(a[A_id].u_obj),
        .username = opt_str(a[A_user].u_obj), .password = opt_str(a[A_pw].u_obj),
        .keepalive_s = (uint16_t)(a[A_ka].u_int > 0 && a[A_ka].u_int < 65536 ? a[A_ka].u_int : 60),
        .flags = (a[A_clean].u_bool ? 0 : T5_MQTT_PERSISTENT) | (a[A_v5].u_bool ? T5_MQTT_V5 : 0) |
                 (a[A_verify].u_bool ? 0 : T5_MQTT_TLS_NO_VERIFY),
        .ca_pem = opt_str(a[A_ca].u_obj), .cert_pem = opt_str(a[A_cert].u_obj), .key_pem = opt_str(a[A_key].u_obj),
        .max_msg = a[A_max].u_int > 0 ? (size_t)a[A_max].u_int : 0,
        .timeout_ms = a[A_timeout].u_int > 0 ? (uint32_t)a[A_timeout].u_int : PY_NET_CONNECT_MS,
    };
    if (a[A_will].u_obj != mp_const_none) {
        /* (topic, msg[, qos[, retain]]) */
        size_t wn;
        mp_obj_t *w;
        mp_obj_get_array(a[A_will].u_obj, &wn, &w);
        if (wn < 2) mp_raise_TypeError(MP_ERROR_TEXT("will is (topic, msg, qos, retain)"));
        mp_buffer_info_t b;
        mp_get_buffer_raise(w[1], &b, MP_BUFFER_READ);
        cfg.will_topic = mp_obj_str_get_str(w[0]);
        cfg.will_msg = b.buf;
        cfg.will_len = b.len;
        cfg.will_qos = wn > 2 ? (uint8_t)mp_obj_get_int(w[2]) : 0;
        cfg.will_retain = wn > 3 && mp_obj_is_true(w[3]);
    }
    py_net_t *o = new_obj(&py_type_mqtt);
    t5_mqtt_t *m;
    t5_err_t e = t5_mqtt_connect(&cfg, &m);
    if (e != T5_OK) fail(e);
    adopt(o, m);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(net_mqtt_obj, 1, net_mqtt);

/* ---- HTTP server ------------------------------------------------------------------------------- */
static t5_err_t close_httpd(void *h)
{
    return t5_httpd_stop(h);
}

static mp_obj_t httpd_close(mp_obj_t self)
{
    return close_common(self, close_httpd);
}
static MP_DEFINE_CONST_FUN_OBJ_1(httpd_close_obj, httpd_close);

/* A request nobody answered is closed with a 500 when collected. */
static t5_err_t drop_req(void *h)
{
    return t5_httpd_respond(h, 500, NULL, NULL, 0, NULL, 0);
}

static mp_obj_t req_del(mp_obj_t self)
{
    return close_common(self, drop_req);
}
static MP_DEFINE_CONST_FUN_OBJ_1(req_del_obj, req_del);

static mp_obj_t lower_dict_httpd(t5_httpd_req_t *r)
{
    mp_obj_t d = mp_obj_new_dict(8);
    const char *name, *value;
    for (int i = 0; t5_httpd_header_at(r, i, &name, &value) == T5_OK; i++) {
        vstr_t k;
        vstr_init(&k, strlen(name) + 1);
        for (const char *p = name; *p; p++) vstr_add_byte(&k, (byte)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p));
        mp_obj_dict_store(d, mp_obj_new_str_from_vstr(&k), mp_obj_new_str_from_cstr(value));
    }
    return d;
}

typedef struct {
    t5_httpd_req_t *got;
} httpd_wait_t;

static int step_httpd(void *h, void *c, uint32_t ms)
{
    return t5_httpd_next(h, ms, &((httpd_wait_t *)c)->got);
}

/* next(timeout=None) -> Request, or None on timeout. */
static mp_obj_t httpd_next(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    py_net_t *r = new_obj(&py_type_request);         /* first: a Stop can't leak the request */
    httpd_wait_t w = { NULL };
    int e = py_net_sliced(o->h, step_httpd, &w, timeout_arg(n > 1 ? args[1] : MP_OBJ_NULL, o->timeout));
    if (e == T5_ERR_TIMEOUT) return mp_const_none;
    if (e < 0) fail(e);
    adopt(r, w.got);
    mp_obj_t info = mp_obj_new_dict(6);
    mp_obj_dict_store(info, MP_OBJ_NEW_QSTR(MP_QSTR_method), mp_obj_new_str_from_cstr(t5_httpd_method(w.got)));
    mp_obj_dict_store(info, MP_OBJ_NEW_QSTR(MP_QSTR_path), mp_obj_new_str_from_cstr(t5_httpd_path(w.got)));
    mp_obj_dict_store(info, MP_OBJ_NEW_QSTR(MP_QSTR_query), mp_obj_new_str_from_cstr(t5_httpd_query(w.got)));
    size_t bl = 0;
    const void *body = t5_httpd_body(w.got, &bl);
    r->content = mp_obj_new_bytes(body, bl);
    r->headers = lower_dict_httpd(w.got);
    t5_net_addr_t peer;
    r->url = t5_httpd_peer(w.got, &peer) == T5_OK ? py_net_addr_tuple(&peer) : mp_const_none;
    r->status = info;
    return MP_OBJ_FROM_PTR(r);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(httpd_next_obj, 1, 2, httpd_next);

static void httpd_attr(mp_obj_t self, qstr attr, mp_obj_t *dest)
{
    py_net_t *o = MP_OBJ_TO_PTR(self);
    if (dest[0] != MP_OBJ_NULL) return;
    if (attr == MP_QSTR_port) dest[0] = MP_OBJ_NEW_SMALL_INT(alive(o) ? t5_httpd_port(o->h) : 0);
    else dest[1] = MP_OBJ_SENTINEL;
}

static const mp_rom_map_elem_t net_httpd_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_next), MP_ROM_PTR(&httpd_next_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout), MP_ROM_PTR(&sock_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&httpd_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&httpd_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&net_exit_obj) },
};
static MP_DEFINE_CONST_DICT(net_httpd_locals, net_httpd_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_httpd, MP_QSTR_HTTPServer, MP_TYPE_FLAG_NONE, print, net_print, attr,
                                httpd_attr, locals_dict, &net_httpd_locals);

/* Request: .method .path .query .headers .body .peer */
static void req_attr(mp_obj_t self, qstr attr, mp_obj_t *dest)
{
    py_net_t *o = MP_OBJ_TO_PTR(self);
    if (dest[0] != MP_OBJ_NULL) return;
    if (attr == MP_QSTR_method || attr == MP_QSTR_path || attr == MP_QSTR_query) {
        dest[0] = mp_obj_dict_get(o->status, MP_OBJ_NEW_QSTR(attr));
    } else if (attr == MP_QSTR_headers) {
        dest[0] = o->headers;
    } else if (attr == MP_QSTR_body) {
        dest[0] = o->content;
    } else if (attr == MP_QSTR_peer) {
        dest[0] = o->url;
    } else {
        dest[1] = MP_OBJ_SENTINEL;
    }
}

static void body_arg(mp_obj_t b, mp_buffer_info_t *out, const char **ctype)
{
    out->buf = NULL;
    out->len = 0;
    if (b == mp_const_none) return;
    if (mp_obj_is_str(b) && !*ctype) *ctype = "text/plain; charset=utf-8";
    mp_get_buffer_raise(b, out, MP_BUFFER_READ);
}

static void req_done(py_net_t *o)
{
    o->h = NULL;
    o->id = 0;
}

/* respond(status=200, body=b"", content_type=None, headers=None) */
static mp_obj_t req_respond(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_self, A_status, A_body, A_ctype, A_headers };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_status, MP_ARG_INT, { .u_int = 200 } },
        { MP_QSTR_body, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_content_type, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_headers, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_net_t *o = live(a[A_self].u_obj);
    const char *ctype = opt_str(a[A_ctype].u_obj);
    mp_buffer_info_t b;
    body_arg(a[A_body].u_obj, &b, &ctype);
    t5_http_header_t hdr[MAX_HEADERS];
    size_t nh = headers_arg(a[A_headers].u_obj, hdr, MAX_HEADERS);
    t5_err_t e = t5_httpd_respond(o->h, (int)a[A_status].u_int, ctype, b.buf, b.len, hdr, nh);
    if (e == T5_ERR_INVALID_ARG) fail(e);       /* still ours: the app may try again */
    req_done(o);
    if (e != T5_OK) fail(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(req_respond_obj, 1, req_respond);

/* json(obj, status=200) */
static mp_obj_t req_json(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    mp_obj_t text = json_call(MP_QSTR_dumps, args[1]);
    mp_buffer_info_t b;
    mp_get_buffer_raise(text, &b, MP_BUFFER_READ);
    t5_err_t e = t5_httpd_respond(o->h, n > 2 ? (int)mp_obj_get_int(args[2]) : 200, "application/json", b.buf, b.len,
                                  NULL, 0);
    req_done(o);
    if (e != T5_OK) fail(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(req_json_obj, 2, 3, req_json);

/* begin(status=200, content_type=None, headers=None): a streamed (chunked) answer. */
static mp_obj_t req_begin(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    t5_http_header_t hdr[MAX_HEADERS];
    size_t nh = n > 3 ? headers_arg(args[3], hdr, MAX_HEADERS) : 0;
    t5_err_t e = t5_httpd_begin(o->h, n > 1 ? (int)mp_obj_get_int(args[1]) : 200, n > 2 ? opt_str(args[2]) : NULL,
                                hdr, nh);
    if (e != T5_OK) fail(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(req_begin_obj, 1, 4, req_begin);

static mp_obj_t req_write(mp_obj_t self, mp_obj_t data)
{
    py_net_t *o = live(self);
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    int r = t5_httpd_write(o->h, b.buf, b.len);
    if (r < 0) fail(r);
    return MP_OBJ_NEW_SMALL_INT(r);
}
static MP_DEFINE_CONST_FUN_OBJ_2(req_write_obj, req_write);

static mp_obj_t req_end(mp_obj_t self)
{
    py_net_t *o = live(self);
    t5_err_t e = t5_httpd_end(o->h);
    req_done(o);
    if (e != T5_OK) fail(e);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(req_end_obj, req_end);

/* websocket(subprotocol=None, max_msg=65536) -> WebSocket (the server side) */
static mp_obj_t req_websocket(size_t n, const mp_obj_t *args)
{
    py_net_t *o = live(args[0]);
    py_net_t *w = new_obj(&py_type_websocket);
    t5_ws_t *ws;
    t5_err_t e = t5_httpd_websocket(o->h, n > 1 ? opt_str(args[1]) : NULL,
                                    n > 2 ? (size_t)mp_obj_get_int(args[2]) : 0, &ws);
    if (e == T5_ERR_INVALID_ARG) mp_raise_ValueError(MP_ERROR_TEXT("not a WebSocket upgrade request"));
    req_done(o);
    if (e != T5_OK) fail(e);
    adopt(w, ws);
    return MP_OBJ_FROM_PTR(w);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(req_websocket_obj, 1, 3, req_websocket);

static void req_print(const mp_print_t *print, mp_obj_t self, mp_print_kind_t kind)
{
    (void)kind;
    py_net_t *o = MP_OBJ_TO_PTR(self);
    mp_printf(print, "<Request %s %s>", mp_obj_str_get_str(mp_obj_dict_get(o->status, MP_OBJ_NEW_QSTR(MP_QSTR_method))),
              mp_obj_str_get_str(mp_obj_dict_get(o->status, MP_OBJ_NEW_QSTR(MP_QSTR_path))));
}

static const mp_rom_map_elem_t net_request_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_respond), MP_ROM_PTR(&req_respond_obj) },
    { MP_ROM_QSTR(MP_QSTR_json), MP_ROM_PTR(&req_json_obj) },
    { MP_ROM_QSTR(MP_QSTR_begin), MP_ROM_PTR(&req_begin_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&req_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_end), MP_ROM_PTR(&req_end_obj) },
    { MP_ROM_QSTR(MP_QSTR_websocket), MP_ROM_PTR(&req_websocket_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&req_del_obj) },
};
static MP_DEFINE_CONST_DICT(net_request_locals, net_request_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(py_type_request, MP_QSTR_Request, MP_TYPE_FLAG_NONE, print, req_print, attr, req_attr,
                                locals_dict, &net_request_locals);

/* serve(port=0, *, max_clients=4, max_body=65536, timeout=10000, cert=None, key=None) */
static mp_obj_t net_serve(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
    enum { A_port, A_clients, A_body, A_timeout, A_cert, A_key };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_port, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_max_clients, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 4 } },
        { MP_QSTR_max_body, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 64 * 1024 } },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = PY_NET_CONNECT_MS } },
        { MP_QSTR_cert, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_key, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    py_need_perm(T5G_PERM_t5_httpd_start);
    t5_httpd_cfg_t cfg = { .max_clients = (int)a[A_clients].u_int,
                           .max_body = a[A_body].u_int > 0 ? (size_t)a[A_body].u_int : 0,
                           .timeout_ms = a[A_timeout].u_int > 0 ? (uint32_t)a[A_timeout].u_int : 0,
                           .cert_pem = opt_str(a[A_cert].u_obj), .key_pem = opt_str(a[A_key].u_obj) };
    py_net_t *o = new_obj(&py_type_httpd);
    t5_httpd_t *srv;
    t5_err_t e = t5_httpd_start(py_net_port_arg(MP_OBJ_NEW_SMALL_INT(a[A_port].u_int)), &cfg, &srv);
    if (e != T5_OK) fail(e);
    adopt(o, srv);
    return MP_OBJ_FROM_PTR(o);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(net_serve_obj, 0, net_serve);

/* ---- poll ------------------------------------------------------------------------------------ */
typedef struct {
    t5_poll_t *items;
    int n;
} poll_ctx_t;

static int step_poll(void *h, void *c, uint32_t ms)
{
    (void)h;
    poll_ctx_t *p = c;
    int r = t5_net_poll(p->items, p->n, ms);
    return r == 0 ? T5_ERR_TIMEOUT : r;
}

/* poll(objs, timeout=None) -> the objects that can be read (or failed) now. */
static mp_obj_t net_poll(size_t n, const mp_obj_t *args)
{
    size_t len;
    mp_obj_t *objs;
    mp_obj_get_array(args[0], &len, &objs);
    if (len > 16) mp_raise_ValueError(MP_ERROR_TEXT("at most 16 objects"));
    t5_poll_t items[16];
    for (size_t i = 0; i < len; i++) {
        const mp_obj_type_t *t = mp_obj_get_type(objs[i]);
        if (t != &py_type_socket && t != &py_type_server && t != &py_type_udp && t != &py_type_response &&
            t != &py_type_websocket && t != &py_type_mqtt && t != &py_type_httpd) {
            mp_raise_TypeError(MP_ERROR_TEXT("poll takes t5.net objects"));
        }
        py_net_t *o = MP_OBJ_TO_PTR(objs[i]);
        items[i] = (t5_poll_t){ .handle = alive(o) ? o->h : NULL, .events = T5_POLL_IN };
    }
    poll_ctx_t c = { items, (int)len };
    int r = py_net_sliced(NULL, step_poll, &c, timeout_arg(n > 1 ? args[1] : MP_OBJ_NULL, -1));
    mp_obj_t out = mp_obj_new_list(0, NULL);
    if (r == T5_ERR_TIMEOUT) return out;
    if (r < 0) fail(r);
    for (size_t i = 0; i < len; i++) {
        if (items[i].revents) mp_obj_list_append(out, objs[i]);
    }
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(net_poll_obj, 1, 2, net_poll);

static const mp_rom_map_elem_t net_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_net) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&net_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_connected), MP_ROM_PTR(&net_connected_obj) },
    { MP_ROM_QSTR(MP_QSTR_resolve), MP_ROM_PTR(&net_resolve_obj) },
    { MP_ROM_QSTR(MP_QSTR_request), MP_ROM_PTR(&net_request_obj) },
    { MP_ROM_QSTR(MP_QSTR_get), MP_ROM_PTR(&net_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_post), MP_ROM_PTR(&net_post_obj) },
    { MP_ROM_QSTR(MP_QSTR_put), MP_ROM_PTR(&net_put_obj) },
    { MP_ROM_QSTR(MP_QSTR_patch), MP_ROM_PTR(&net_patch_obj) },
    { MP_ROM_QSTR(MP_QSTR_delete), MP_ROM_PTR(&net_delete_obj) },
    { MP_ROM_QSTR(MP_QSTR_head), MP_ROM_PTR(&net_head_obj) },
    { MP_ROM_QSTR(MP_QSTR_tcp), MP_ROM_PTR(&net_tcp_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&net_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_udp), MP_ROM_PTR(&net_udp_obj) },
    { MP_ROM_QSTR(MP_QSTR_websocket), MP_ROM_PTR(&net_websocket_obj) },
    { MP_ROM_QSTR(MP_QSTR_poll), MP_ROM_PTR(&net_poll_obj) },
    { MP_ROM_QSTR(MP_QSTR_mqtt), MP_ROM_PTR(&net_mqtt_obj) },
    { MP_ROM_QSTR(MP_QSTR_serve), MP_ROM_PTR(&net_serve_obj) },
    { MP_ROM_QSTR(MP_QSTR_http_get), MP_ROM_PTR(&net_http_get_obj) },
    { MP_ROM_QSTR(MP_QSTR_http_post), MP_ROM_PTR(&net_http_post_obj) },
};
static MP_DEFINE_CONST_DICT(net_globals, net_globals_table);
const mp_obj_module_t py_mod_net = { .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&net_globals };
