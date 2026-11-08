/* SPDX-License-Identifier: Apache-2.0
 * Sockets (t5_net/sock.h): the handle registry, DNS, TCP and TLS clients,
 * TCP servers, UDP and poll.
 *
 * Every socket is non-blocking underneath. Waits are select() slices of at
 * most NET_SLICE_MS, and between slices a wait checks whether its handle is
 * closing, the owner's stop flag (HTTP/WebSocket) or the calling task's
 * cancel flag. DNS runs on a short-lived worker task so a lookup can be
 * abandoned; the worker finishes on its own and drops its result.
 *
 * TLS: esp-tls on the socket we connected ourselves (so connect is
 * cancellable too), stepped with esp_tls_conn_new_async until the handshake
 * is done. mbedTLS contexts aren't safe for a read and a write at the same
 * time, so each TLS call holds the socket's io mutex (only for the call,
 * never across a wait).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/ssl.h"
#include "netio_priv.h"
#include "netproto.h"
#include "sdkconfig.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/res.h"
#include "t5_power/power.h"

static const char *TAG = "net.sock";

/* Thread-local slot for the cancel flag: t5_core keeps the app context in
 * the last slot, ESP-IDF's pthread layer uses slot 0. */
#define NET_TLS_SLOT (CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS - 2)
_Static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS >= 3, "need a TLS slot for the net cancel flag");

#define POLL_MAX 16
#define RESOLVE_WORKERS 4
#define IO_AGAIN (-1)
#define IO_ERR (-2)

enum { SK_TCP, SK_LISTEN, SK_UDP };

struct t5_sock {
    t5_nethdr_t hdr;
    int fd;
    uint8_t kind;
    uint8_t family;
    volatile bool *stop;        /* the owner's closing flag (sockets inside HTTP/WS) */
    esp_tls_t *tls;
    bool tls_server;            /* accepted side: freed with esp_tls_server_session_delete */
    SemaphoreHandle_t io;       /* around esp-tls calls */
};

/* ---- registry ------------------------------------------------------------------------- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static t5_nethdr_t *s_list;
static uint32_t s_next_id = 1;

static int app_count_locked(void *app)
{
    int mine = 0;
    for (t5_nethdr_t *p = s_list; p; p = p->next) mine += p->app == app;
    return mine;
}

/* Would one more handle fit in the calling app's quota? */
static bool quota_ok(void)
{
    void *app = t5_app_ctx_get();
    if (!app) return true;
    taskENTER_CRITICAL(&s_mux);
    bool ok = app_count_locked(app) < T5_NET_APP_MAX;
    taskEXIT_CRITICAL(&s_mux);
    if (!ok) T5_LOGW(TAG, "%s: more than %d network handles open", t5_app_ctx_id(app), T5_NET_APP_MAX);
    return ok;
}

/* Open handles keep Wi-Fi up: while any socket, request, WebSocket, MQTT
 * client or server is open, the stack holds a NET wake lock, so Sleep keeps
 * the C6 connected (in light power save) instead of powering it down, and a
 * download or a long-lived connection survives the screen going off. */
static t5_mutex_t s_wake_mtx;
static t5_wakelock_t *s_wake;
static int s_live;

static void wake_update(void)
{
    if (!s_wake_mtx) {
        t5_mutex_t m = t5_mutex_create();
        taskENTER_CRITICAL(&s_mux);
        if (!s_wake_mtx) {
            s_wake_mtx = m;
            m = NULL;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (m) t5_mutex_delete(m);
        if (!s_wake_mtx) return;
    }
    t5_mutex_lock(s_wake_mtx);
    taskENTER_CRITICAL(&s_mux);
    bool want = s_live > 0;
    taskEXIT_CRITICAL(&s_mux);
    if (want && !s_wake) s_wake = t5_wakelock_acquire(T5_LOCK_NET, "net.io");
    else if (!want && s_wake) {
        t5_wakelock_release(s_wake);
        s_wake = NULL;
    }
    t5_mutex_unlock(s_wake_mtx);
}

int t5_net_handles_open(void)
{
    taskENTER_CRITICAL(&s_mux);
    int n = s_live;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

t5_err_t nh_register(t5_nethdr_t *h, uint32_t magic, void (*release)(void *))
{
    void *app = t5_app_ctx_get();
    h->magic = magic;
    h->app = app;
    h->closing = false;
    h->refs = 0;
    bool full;
    taskENTER_CRITICAL(&s_mux);
    full = app && app_count_locked(app) >= T5_NET_APP_MAX;
    if (!full) {
        h->id = s_next_id++;
        if (!s_next_id) s_next_id = 1;
        h->next = s_list;
        s_list = h;
        s_live++;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (full) {
        T5_LOGW(TAG, "%s: more than %d network handles open", t5_app_ctx_id(app), T5_NET_APP_MAX);
        return T5_ERR_BUSY;
    }
    t5_res_add(T5_RES_OTHER, h, release);
    wake_update();
    return T5_OK;
}

static bool acquire(void *h, uint32_t magic, uint32_t *got)
{
    bool ok = false;
    if (!h) return false;
    taskENTER_CRITICAL(&s_mux);
    for (t5_nethdr_t *p = s_list; p; p = p->next) {
        if (p == h && !p->closing && (!magic || p->magic == magic)) {
            p->refs++;
            if (got) *got = p->magic;
            ok = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

bool nh_acquire(void *h, uint32_t magic)
{
    return acquire(h, magic, NULL);
}

bool nh_acquire_any(void *h, uint32_t *magic)
{
    return acquire(h, 0, magic);
}

void nh_release(void *h)
{
    t5_nethdr_t *n = h;
    taskENTER_CRITICAL(&s_mux);
    n->refs--;
    taskEXIT_CRITICAL(&s_mux);
}

t5_err_t nh_unregister(void *h, uint32_t magic)
{
    t5_nethdr_t *n = h;
    bool found = false;
    if (!h) return T5_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_mux);
    for (t5_nethdr_t **pp = &s_list; *pp; pp = &(*pp)->next) {
        if (*pp == n && n->magic == magic && !n->closing) {
            *pp = n->next;
            n->closing = true;
            found = true;
            s_live--;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    if (!found) return T5_ERR_INVALID_ARG;
    t5_res_remove_app(n->app, n);
    wake_update();
    /* Waits end within a slice once closing is set; give them 5 s at most. */
    for (int i = 0; i < 1000; i++) {
        taskENTER_CRITICAL(&s_mux);
        int refs = n->refs;
        taskEXIT_CRITICAL(&s_mux);
        if (refs <= 0) return T5_OK;
        t5_delay_ms(5);
    }
    T5_LOGE(TAG, "%s still in use after close", n->what);
    return T5_OK;
}

uint32_t t5_net_handle_id(void *h)
{
    uint32_t id = 0;
    if (!h) return 0;
    taskENTER_CRITICAL(&s_mux);
    for (t5_nethdr_t *p = s_list; p; p = p->next) {
        if (p == h && !p->closing) {
            id = p->id;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    return id;
}

int t5_net_handles_dump(char *buf, size_t n)
{
    int count = 0;
    size_t off = 0;
    if (buf && n) buf[0] = 0;
    taskENTER_CRITICAL(&s_mux);
    for (t5_nethdr_t *p = s_list; p; p = p->next) {
        if (buf && off < n) {
            int w = snprintf(buf + off, n - off, "%-24s %s\n", t5_app_ctx_id(p->app), p->what);
            if (w > 0) off += (size_t)w;
        }
        count++;
    }
    taskEXIT_CRITICAL(&s_mux);
    return count;
}

/* ---- time and cancellation ------------------------------------------------------------ */
uint32_t net_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t net_deadline(uint32_t timeout_ms)
{
    return timeout_ms == T5_NET_FOREVER ? T5_NET_FOREVER : net_now_ms() + timeout_ms;
}

uint32_t net_left(uint32_t deadline, uint32_t timeout_ms)
{
    if (timeout_ms == T5_NET_FOREVER) return T5_NET_FOREVER;
    int32_t d = (int32_t)(deadline - net_now_ms());
    return d > 0 ? (uint32_t)d : 0;
}

void t5_net_cancel_flag(volatile bool *flag)
{
    vTaskSetThreadLocalStoragePointer(NULL, NET_TLS_SLOT, (void *)flag);
}

bool net_cancelled(void)
{
    volatile bool *f = pvTaskGetThreadLocalStoragePointer(NULL, NET_TLS_SLOT);
    return f && *f;
}

static bool stopped(volatile bool *stop)
{
    return (stop && *stop) || net_cancelled();
}

static bool sock_aborted(const t5_sock_t *s)
{
    return s->hdr.closing || stopped(s->stop);
}

/* select() on one fd for at most a slice. 1 ready, 0 not yet, -1 error. */
static int wait_fd(int fd, bool rd, bool wr, uint32_t left)
{
    fd_set r, w, e;
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_ZERO(&e);
    if (rd) FD_SET(fd, &r);
    if (wr) FD_SET(fd, &w);
    FD_SET(fd, &e);
    uint32_t ms = left < NET_SLICE_MS ? left : NET_SLICE_MS;
    struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)ms * 1000 };
    int n = select(fd + 1, &r, &w, &e, &tv);
    if (n < 0) return errno == EINTR ? 0 : -1;
    return n > 0 ? 1 : 0;
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
}

/* ---- addresses -------------------------------------------------------------------------- */
static socklen_t to_sockaddr(const t5_net_addr_t *a, struct sockaddr_storage *ss)
{
    memset(ss, 0, sizeof *ss);
    if (a->family == 6) {
        struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)ss;
        s6->sin6_family = AF_INET6;
        s6->sin6_port = htons(a->port);
        memcpy(&s6->sin6_addr, a->ip, 16);
        return sizeof *s6;
    }
    struct sockaddr_in *s4 = (struct sockaddr_in *)ss;
    s4->sin_family = AF_INET;
    s4->sin_port = htons(a->port);
    memcpy(&s4->sin_addr, a->ip, 4);
    return sizeof *s4;
}

static void from_sockaddr(const struct sockaddr *sa, t5_net_addr_t *a)
{
    memset(a, 0, sizeof *a);
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
        a->family = 6;
        memcpy(a->ip, &s6->sin6_addr, 16);
        a->port = ntohs(s6->sin6_port);
    } else {
        const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
        a->family = 4;
        memcpy(a->ip, &s4->sin_addr, 4);
        a->port = ntohs(s4->sin_port);
    }
}

t5_err_t t5_net_addr_parse(const char *s, t5_net_addr_t *out)
{
    if (!s || !out) return T5_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    if (inet_pton(AF_INET, s, out->ip) == 1) {
        out->family = 4;
        return T5_OK;
    }
    if (inet_pton(AF_INET6, s, out->ip) == 1) {
        out->family = 6;
        return T5_OK;
    }
    return T5_ERR_INVALID_ARG;
}

int t5_net_addr_str(const t5_net_addr_t *a, char *buf, size_t n)
{
    if (!a || !buf || !n) return T5_ERR_INVALID_ARG;
    const char *r = inet_ntop(a->family == 6 ? AF_INET6 : AF_INET, a->ip, buf, (socklen_t)n);
    return r ? (int)strlen(buf) : T5_ERR_INVALID_ARG;
}

/* ---- DNS ------------------------------------------------------------------------------- */
typedef struct {
    int refs;                   /* the waiter and the worker */
    volatile bool done;
    t5_err_t err;
    t5_net_addr_t addr;
    char host[T5NP_HOST_MAX + 1];
} rjob_t;

static SemaphoreHandle_t s_rsem;    /* RESOLVE_WORKERS lookups at a time */

static void rjob_put(rjob_t *j)
{
    taskENTER_CRITICAL(&s_mux);
    bool last = --j->refs == 0;
    taskEXIT_CRITICAL(&s_mux);
    if (last) free(j);
}

static void resolve_task(void *arg)
{
    rjob_t *j = arg;
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int r = getaddrinfo(j->host, NULL, &hints, &res);
    j->err = T5_ERR_NOT_FOUND;
    const struct addrinfo *pick = NULL;
    for (const struct addrinfo *p = res; r == 0 && p; p = p->ai_next) {
        if (p->ai_family == AF_INET && (!pick || pick->ai_family != AF_INET)) pick = p;
        if (p->ai_family == AF_INET6 && !pick) pick = p;
    }
    if (pick) {
        from_sockaddr(pick->ai_addr, &j->addr);
        j->addr.port = 0;
        j->err = T5_OK;
    }
    if (res) freeaddrinfo(res);
    taskENTER_CRITICAL(&s_mux);
    j->done = true;
    taskEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_rsem);
    rjob_put(j);
    vTaskDelete(NULL);
}

static t5_err_t resolve(const char *host, t5_net_addr_t *out, uint32_t timeout_ms, volatile bool *stop)
{
    if (!host || !out || !*host || strlen(host) > T5NP_HOST_MAX) return T5_ERR_INVALID_ARG;
    if (t5_net_addr_parse(host, out) == T5_OK) return T5_OK;
    if (!strcasecmp(host, "localhost")) return t5_net_addr_parse("127.0.0.1", out);
    if (!s_rsem) {
        SemaphoreHandle_t s = xSemaphoreCreateCounting(RESOLVE_WORKERS, RESOLVE_WORKERS);
        if (!s) return T5_ERR_NO_MEM;
        taskENTER_CRITICAL(&s_mux);
        bool mine = !s_rsem;
        if (mine) s_rsem = s;
        taskEXIT_CRITICAL(&s_mux);
        if (!mine) vSemaphoreDelete(s);
    }
    uint32_t dl = net_deadline(timeout_ms);
    while (xSemaphoreTake(s_rsem, pdMS_TO_TICKS(NET_SLICE_MS)) != pdTRUE) {
        if (stopped(stop)) return T5_ERR_STATE;
        if (!net_left(dl, timeout_ms)) return T5_ERR_TIMEOUT;
    }
    rjob_t *j = calloc(1, sizeof *j);
    if (!j) {
        xSemaphoreGive(s_rsem);
        return T5_ERR_NO_MEM;
    }
    j->refs = 2;
    snprintf(j->host, sizeof j->host, "%s", host);
    /* A plain FreeRTOS task: no app context, so the app tracker doesn't own it. */
    if (xTaskCreatePinnedToCore(resolve_task, "t5_dns", 4096, j, 5, NULL, tskNO_AFFINITY) != pdPASS) {
        xSemaphoreGive(s_rsem);
        free(j);
        return T5_ERR_NO_MEM;
    }
    t5_err_t e = T5_ERR_TIMEOUT;
    for (;;) {
        if (j->done) {
            e = j->err;
            if (e == T5_OK) *out = j->addr;
            break;
        }
        if (stopped(stop)) {
            e = T5_ERR_STATE;
            break;
        }
        if (!net_left(dl, timeout_ms)) break;
        t5_delay_ms(10);
    }
    rjob_put(j);
    return e;
}

t5_err_t t5_net_resolve(const char *host, t5_net_addr_t *out, uint32_t timeout_ms)
{
    return resolve(host, out, timeout_ms ? timeout_ms : NET_DEFAULT_TIMEOUT_MS, NULL);
}

/* ---- raw I/O ----------------------------------------------------------------------------- */
static t5_sock_t *sock_new(uint8_t kind, uint8_t family)
{
    t5_sock_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->fd = -1;
    s->kind = kind;
    s->family = family;
    s->io = xSemaphoreCreateMutex();
    if (!s->io) {
        free(s);
        return NULL;
    }
    return s;
}

void ns_destroy(t5_sock_t *s)
{
    if (!s) return;
    if (s->tls && s->tls_server) {
        esp_tls_server_session_delete(s->tls);
        if (s->fd >= 0) close(s->fd);
    } else if (s->tls) {
        esp_tls_conn_destroy(s->tls);          /* closes the fd too */
    } else if (s->fd >= 0) {
        close(s->fd);
    }
    vSemaphoreDelete(s->io);
    free(s);
}

static int raw_read(t5_sock_t *s, void *buf, size_t n)
{
    if (s->tls) {
        xSemaphoreTake(s->io, portMAX_DELAY);
        ssize_t r = esp_tls_conn_read(s->tls, buf, n);
        xSemaphoreGive(s->io);
        if (r >= 0) return (int)r;
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) return IO_AGAIN;
        if (r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
        return IO_ERR;
    }
    ssize_t r = recv(s->fd, buf, n, MSG_DONTWAIT);
    if (r >= 0) return (int)r;
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? IO_AGAIN : IO_ERR;
}

static int raw_write(t5_sock_t *s, const void *buf, size_t n)
{
    if (s->tls) {
        xSemaphoreTake(s->io, portMAX_DELAY);
        ssize_t r = esp_tls_conn_write(s->tls, buf, n);
        xSemaphoreGive(s->io);
        if (r > 0) return (int)r;
        if (r == 0 || r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) return IO_AGAIN;
        return IO_ERR;
    }
    ssize_t r = send(s->fd, buf, n, MSG_DONTWAIT);
    if (r >= 0) return (int)r;
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ENOMEM) ? IO_AGAIN : IO_ERR;
}

int ns_send(t5_sock_t *s, const void *data, size_t n, uint32_t timeout_ms)
{
    if (!data && n) return T5_ERR_INVALID_ARG;
    const uint8_t *p = data;
    size_t done = 0;
    uint32_t dl = net_deadline(timeout_ms);
    t5_err_t why = T5_ERR_TIMEOUT;
    while (done < n || (n == 0 && s->kind == SK_UDP && !done)) {
        if (sock_aborted(s)) {
            why = T5_ERR_STATE;
            break;
        }
        int r = raw_write(s, p + done, n - done);
        if (r >= 0) {
            done += (size_t)r;
            if (s->kind == SK_UDP) break;      /* one datagram */
            continue;
        }
        if (r == IO_ERR) {
            why = T5_ERR_IO;
            break;
        }
        uint32_t left = net_left(dl, timeout_ms);
        if (!left) break;
        if (wait_fd(s->fd, false, true, left) < 0) {
            why = T5_ERR_IO;
            break;
        }
    }
    if (done || (n == 0 && why == T5_ERR_TIMEOUT)) return (int)done;
    return why;
}

int ns_recv(t5_sock_t *s, void *buf, size_t n, uint32_t timeout_ms)
{
    if (!buf || !n) return T5_ERR_INVALID_ARG;
    uint32_t dl = net_deadline(timeout_ms);
    for (;;) {
        if (sock_aborted(s)) return T5_ERR_STATE;
        int r = raw_read(s, buf, n);
        if (r >= 0) return r;
        if (r == IO_ERR) return T5_ERR_IO;
        uint32_t left = net_left(dl, timeout_ms);
        if (!left) return T5_ERR_TIMEOUT;
        if (wait_fd(s->fd, true, false, left) < 0) return T5_ERR_IO;
    }
}

int ns_pending(t5_sock_t *s)
{
    if (s->tls) {
        xSemaphoreTake(s->io, portMAX_DELAY);
        ssize_t n = esp_tls_get_bytes_avail(s->tls);
        xSemaphoreGive(s->io);
        return n > 0 ? (int)n : 0;
    }
    int n = 0;
    if (s->kind == SK_LISTEN || ioctl(s->fd, FIONREAD, &n) < 0) return 0;
    return n;
}

int ns_fd(t5_sock_t *s)
{
    return s->fd;
}

/* ---- connect ----------------------------------------------------------------------------- */
/* crt_bundle_attach stand-in for T5_TCP_TLS_NO_VERIFY: esp-tls sets
 * VERIFY_REQUIRED just before calling it. */
esp_err_t net_attach_no_verify(void *conf)
{
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE);
    return ESP_OK;
}

/* Step a handshake (client or server) until done, waiting in slices. */
static t5_err_t tls_wait(t5_sock_t *s, const char *what, int (*step)(t5_sock_t *s, void *ctx), void *ctx, uint32_t dl,
                         uint32_t timeout_ms)
{
    for (;;) {
        int r = step(s, ctx);
        if (r == 1) return T5_OK;
        if (r < 0) {
            T5_LOGW(TAG, "TLS handshake with %s failed", what);
            return T5_ERR_IO;
        }
        if (sock_aborted(s)) return T5_ERR_STATE;
        uint32_t left = net_left(dl, timeout_ms);
        if (!left) return T5_ERR_TIMEOUT;
        if (wait_fd(s->fd, true, false, left) < 0) return T5_ERR_IO;
    }
}

typedef struct {
    const char *name;
    uint16_t port;
    esp_tls_cfg_t *tc;
} client_step_t;

static int client_step(t5_sock_t *s, void *ctx)
{
    client_step_t *c = ctx;
    return esp_tls_conn_new_async(c->name, (int)strlen(c->name), c->port, c->tc, s->tls);
}

static int server_step(t5_sock_t *s, void *ctx)
{
    (void)ctx;
    int r = esp_tls_server_session_continue_async(s->tls);
    if (r == 0) return 1;
    return (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) ? 0 : -1;
}

static void pem(const char *p, const unsigned char **buf, unsigned int *len)
{
    if (!p) return;
    *buf = (const unsigned char *)p;
    *len = (unsigned int)strlen(p) + 1;          /* mbedTLS wants the NUL for PEM */
}

static t5_err_t tls_client(t5_sock_t *s, const char *name, uint16_t port, const t5_tls_cfg_t *cfg, uint32_t dl,
                           uint32_t timeout_ms)
{
    esp_tls_cfg_t tc = { 0 };
    /* The socket is already connected and non-blocking; non_block = false
     * keeps esp-tls from select()ing on fd sets it only fills in for its
     * own connect. Handshake steps still return on WANT_READ/WRITE. */
    tc.non_block = false;
    tc.timeout_ms = NET_SLICE_MS;
    if (cfg->flags & T5_TLS_NO_VERIFY) tc.crt_bundle_attach = net_attach_no_verify;
    else if (cfg->ca_pem) pem(cfg->ca_pem, &tc.cacert_buf, &tc.cacert_bytes);
    else tc.crt_bundle_attach = esp_crt_bundle_attach;
    tc.skip_common_name = cfg->flags & T5_TLS_NO_NAME_CHECK;
    pem(cfg->cert_pem, &tc.clientcert_buf, &tc.clientcert_bytes);
    pem(cfg->key_pem, &tc.clientkey_buf, &tc.clientkey_bytes);
    tc.alpn_protos = (const char **)cfg->alpn;
    s->tls = esp_tls_init();
    if (!s->tls) return T5_ERR_NO_MEM;
    if (esp_tls_set_conn_sockfd(s->tls, s->fd) != ESP_OK ||
        esp_tls_set_conn_state(s->tls, ESP_TLS_CONNECTING) != ESP_OK) {
        return T5_ERR_FAIL;
    }
    client_step_t c = { name, port, &tc };
    return tls_wait(s, name, client_step, &c, dl, timeout_ms);
}

static t5_err_t tls_server(t5_sock_t *s, const t5_tls_cfg_t *cfg, uint32_t dl, uint32_t timeout_ms)
{
    if (!cfg->cert_pem || !cfg->key_pem) return T5_ERR_INVALID_ARG;
    esp_tls_cfg_server_t sc = { 0 };
    pem(cfg->cert_pem, &sc.servercert_buf, &sc.servercert_bytes);
    pem(cfg->key_pem, &sc.serverkey_buf, &sc.serverkey_bytes);
    pem(cfg->ca_pem, &sc.cacert_buf, &sc.cacert_bytes);      /* clients must present a cert signed by it */
    sc.alpn_protos = (const char **)cfg->alpn;
    s->tls = esp_tls_init();
    if (!s->tls) return T5_ERR_NO_MEM;
    s->tls_server = true;
    if (esp_tls_server_session_init(&sc, s->fd, s->tls) != ESP_OK) return T5_ERR_IO;
    T5_RETURN_ON_ERR(tls_wait(s, "a client", server_step, NULL, dl, timeout_ms));
    esp_tls_set_conn_state(s->tls, ESP_TLS_DONE);
    return T5_OK;
}

static t5_err_t tcp_connect_addr(t5_sock_t *s, const t5_net_addr_t *a, uint32_t dl, uint32_t timeout_ms)
{
    s->fd = socket(a->family == 6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->fd < 0) return T5_ERR_NO_MEM;           /* out of sockets */
    set_nonblock(s->fd);
    struct sockaddr_storage ss;
    socklen_t sl = to_sockaddr(a, &ss);
    if (connect(s->fd, (struct sockaddr *)&ss, sl) < 0 && errno != EINPROGRESS) return T5_ERR_IO;
    for (;;) {
        if (sock_aborted(s)) return T5_ERR_STATE;
        uint32_t left = net_left(dl, timeout_ms);
        if (!left) return T5_ERR_TIMEOUT;
        int w = wait_fd(s->fd, false, true, left);
        if (w < 0) return T5_ERR_IO;
        if (w > 0) break;
    }
    int err = 0;
    socklen_t el = sizeof err;
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) return T5_ERR_IO;
    return T5_OK;
}

t5_err_t ns_connect(const char *host, uint16_t port, const t5_tcp_cfg_t *cfg, volatile bool *stop, t5_sock_t **out)
{
    static const t5_tcp_cfg_t defaults = { 0 };
    if (!cfg) cfg = &defaults;
    if (!host || !out || !port) return T5_ERR_INVALID_ARG;
    *out = NULL;
    uint32_t timeout = cfg->timeout_ms ? cfg->timeout_ms : NET_DEFAULT_TIMEOUT_MS;
    uint32_t dl = net_deadline(timeout);
    t5_net_addr_t a;
    T5_RETURN_ON_ERR(resolve(host, &a, timeout, stop));
    a.port = port;
    t5_sock_t *s = sock_new(SK_TCP, a.family);
    if (!s) return T5_ERR_NO_MEM;
    s->stop = stop;
    t5_err_t e = tcp_connect_addr(s, &a, dl, timeout);
    if (e == T5_OK && (cfg->flags & T5_TCP_NODELAY)) {
        int one = 1;
        setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    if (e == T5_OK && (cfg->flags & (T5_TCP_TLS | T5_TCP_TLS_NO_VERIFY))) {
        t5_tls_cfg_t t = { .flags = (cfg->flags & T5_TCP_TLS_NO_VERIFY) ? T5_TLS_NO_VERIFY : 0, .ca_pem = cfg->ca_pem,
                           .alpn = cfg->alpn, .cert_pem = cfg->cert_pem, .key_pem = cfg->key_pem };
        e = tls_client(s, cfg->sni ? cfg->sni : host, port, &t, dl, timeout);
    }
    if (e != T5_OK) {
        ns_destroy(s);
        return e;
    }
    *out = s;
    return T5_OK;
}

/* ---- public sockets ------------------------------------------------------------------------ */
static void release_sock(void *h)
{
    T5_IGNORE(t5_sock_close(h));      /* app exit: nothing to report to */
}

static t5_err_t publish(t5_sock_t *s, t5_sock_t **out)
{
    t5_err_t e = nh_register(&s->hdr, NH_SOCK, release_sock);
    if (e != T5_OK) {
        ns_destroy(s);
        return e;
    }
    *out = s;
    return T5_OK;
}

t5_err_t t5_tcp_connect(const char *host, uint16_t port, const t5_tcp_cfg_t *cfg, t5_sock_t **out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!quota_ok()) return T5_ERR_BUSY;
    t5_sock_t *s;
    T5_RETURN_ON_ERR(ns_connect(host, port, cfg, NULL, &s));
    bool tls = s->tls != NULL;
    snprintf(s->hdr.what, sizeof s->hdr.what, "%s %.24s:%u", tls ? "tls" : "tcp", host, port);
    return publish(s, out);
}

int t5_sock_send(t5_sock_t *s, const void *data, size_t n, uint32_t timeout_ms)
{
    if (!nh_acquire(s, NH_SOCK)) return T5_ERR_INVALID_ARG;
    int r = s->kind == SK_LISTEN ? T5_ERR_STATE : ns_send(s, data, n, timeout_ms);
    nh_release(s);
    return r;
}

int t5_sock_recv(t5_sock_t *s, void *buf, size_t n, uint32_t timeout_ms)
{
    if (!nh_acquire(s, NH_SOCK)) return T5_ERR_INVALID_ARG;
    int r = s->kind == SK_LISTEN ? T5_ERR_STATE : ns_recv(s, buf, n, timeout_ms);
    nh_release(s);
    return r;
}

int t5_sock_pending(t5_sock_t *s)
{
    if (!nh_acquire(s, NH_SOCK)) return T5_ERR_INVALID_ARG;
    int r = ns_pending(s);
    nh_release(s);
    return r;
}

t5_err_t t5_sock_shutdown(t5_sock_t *s)
{
    if (!nh_acquire(s, NH_SOCK)) return T5_ERR_INVALID_ARG;
    t5_err_t e = T5_OK;
    if (s->kind != SK_TCP || s->tls) e = T5_ERR_NOT_SUPPORTED;
    else if (shutdown(s->fd, SHUT_WR) < 0) e = T5_ERR_IO;
    nh_release(s);
    return e;
}

static t5_err_t sock_addr(t5_sock_t *s, t5_net_addr_t *out, bool peer)
{
    if (!out) return T5_ERR_INVALID_ARG;
    if (!nh_acquire(s, NH_SOCK)) return T5_ERR_INVALID_ARG;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int r = peer ? getpeername(s->fd, (struct sockaddr *)&ss, &sl) : getsockname(s->fd, (struct sockaddr *)&ss, &sl);
    nh_release(s);
    if (r < 0) return T5_ERR_STATE;
    from_sockaddr((struct sockaddr *)&ss, out);
    return T5_OK;
}

t5_err_t t5_sock_local_addr(t5_sock_t *s, t5_net_addr_t *out)
{
    return sock_addr(s, out, false);
}

t5_err_t t5_sock_peer_addr(t5_sock_t *s, t5_net_addr_t *out)
{
    return sock_addr(s, out, true);
}

t5_err_t t5_sock_close(t5_sock_t *s)
{
    T5_RETURN_ON_ERR(nh_unregister(s, NH_SOCK));
    ns_destroy(s);
    return T5_OK;
}

t5_err_t ns_start_tls(t5_sock_t *s, const t5_tls_cfg_t *cfg)
{
    static const t5_tls_cfg_t defaults = { 0 };
    if (!cfg) cfg = &defaults;
    uint32_t timeout = cfg->timeout_ms ? cfg->timeout_ms : NET_DEFAULT_TIMEOUT_MS;
    uint32_t dl = net_deadline(timeout);
    if (s->kind != SK_TCP || s->tls) return T5_ERR_STATE;
    if (cfg->flags & T5_TLS_SERVER) return tls_server(s, cfg, dl, timeout);
    char ip[48] = "";
    const char *name = cfg->sni;
    if (!name) {
        /* No name: the certificate can only match the address. */
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        t5_net_addr_t a;
        if (getpeername(s->fd, (struct sockaddr *)&ss, &sl) == 0) {
            from_sockaddr((struct sockaddr *)&ss, &a);
            if (t5_net_addr_str(&a, ip, sizeof ip) < 0) ip[0] = 0;
        }
        name = ip;
    }
    return tls_client(s, name, 443, cfg, dl, timeout);
}

t5_err_t t5_sock_start_tls(t5_sock_t *s, const t5_tls_cfg_t *cfg)
{
    if (!nh_acquire(s, NH_SOCK)) return T5_ERR_INVALID_ARG;
    t5_err_t e = ns_start_tls(s, cfg);
    if (e == T5_OK && !strncmp(s->hdr.what, "tcp", 3)) memcpy(s->hdr.what, "tls", 3);
    nh_release(s);
    return e;
}

t5_err_t t5_sock_set_option(t5_sock_t *s, t5_sockopt_t opt, int value)
{
    if (!nh_acquire(s, NH_SOCK)) return T5_ERR_INVALID_ARG;
    int r = 0, one = value ? 1 : 0;
    switch (opt) {
    case T5_SOCKOPT_NODELAY: r = setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one); break;
    case T5_SOCKOPT_KEEPALIVE:
        r = setsockopt(s->fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
        if (!r && value > 0) {
            int idle = value, intvl = value > 3 ? value / 3 : 1, cnt = 3;
            r = setsockopt(s->fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle) ||
                setsockopt(s->fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl) ||
                setsockopt(s->fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
        }
        break;
    case T5_SOCKOPT_BROADCAST: r = setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one); break;
    case T5_SOCKOPT_REUSEADDR: r = setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); break;
    case T5_SOCKOPT_RCVBUF: r = setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof value); break;
    case T5_SOCKOPT_TTL: r = setsockopt(s->fd, IPPROTO_IP, IP_TTL, &value, sizeof value); break;
    case T5_SOCKOPT_TOS: r = setsockopt(s->fd, IPPROTO_IP, IP_TOS, &value, sizeof value); break;
    default: r = -2; break;
    }
    nh_release(s);
    return r == -2 ? T5_ERR_INVALID_ARG : r ? T5_ERR_NOT_SUPPORTED : T5_OK;
}

/* ---- servers ------------------------------------------------------------------------------- */
t5_err_t ns_listen(uint16_t port, int backlog, t5_sock_t **out)
{
    t5_sock_t *s = sock_new(SK_LISTEN, 4);
    if (!s) return T5_ERR_NO_MEM;
    s->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    t5_err_t e = s->fd < 0 ? T5_ERR_NO_MEM : T5_OK;
    if (e == T5_OK) {
        int one = 1;
        setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        set_nonblock(s->fd);
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(s->fd, (struct sockaddr *)&sa, sizeof sa) < 0) e = errno == EADDRINUSE ? T5_ERR_BUSY : T5_ERR_IO;
        else if (listen(s->fd, backlog > 0 && backlog <= 8 ? backlog : 4) < 0) e = T5_ERR_IO;
    }
    if (e != T5_OK) {
        ns_destroy(s);
        return e;
    }
    *out = s;
    return T5_OK;
}

t5_err_t t5_tcp_listen(uint16_t port, int backlog, t5_sock_t **out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!quota_ok()) return T5_ERR_BUSY;
    t5_sock_t *s;
    T5_RETURN_ON_ERR(ns_listen(port, backlog, &s));
    snprintf(s->hdr.what, sizeof s->hdr.what, "listen :%u", port);
    return publish(s, out);
}

/* Accept with a timeout; *out is an owned (unregistered) socket. */
t5_err_t ns_accept(t5_sock_t *srv, uint32_t timeout_ms, t5_sock_t **out)
{
    if (srv->kind != SK_LISTEN) return T5_ERR_STATE;
    int fd = -1;
    struct sockaddr_storage ss;
    uint32_t dl = net_deadline(timeout_ms);
    for (;;) {
        if (sock_aborted(srv)) return T5_ERR_STATE;
        socklen_t sl = sizeof ss;
        fd = accept(srv->fd, (struct sockaddr *)&ss, &sl);
        if (fd >= 0) break;
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return T5_ERR_IO;
        uint32_t left = net_left(dl, timeout_ms);
        if (!left) return T5_ERR_TIMEOUT;
        if (wait_fd(srv->fd, true, false, left) < 0) return T5_ERR_IO;
    }
    t5_sock_t *s = sock_new(SK_TCP, ss.ss_family == AF_INET6 ? 6 : 4);
    if (!s) {
        close(fd);
        return T5_ERR_NO_MEM;
    }
    s->fd = fd;
    set_nonblock(fd);
    *out = s;
    return T5_OK;
}

t5_err_t t5_tcp_accept(t5_sock_t *srv, uint32_t timeout_ms, t5_sock_t **out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!nh_acquire(srv, NH_SOCK)) return T5_ERR_INVALID_ARG;
    t5_sock_t *s = NULL;
    t5_err_t e = ns_accept(srv, timeout_ms, &s);
    nh_release(srv);
    if (e != T5_OK) return e;
    if (!quota_ok()) {
        ns_destroy(s);
        return T5_ERR_BUSY;
    }
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    t5_net_addr_t peer = { 0 };
    if (getpeername(s->fd, (struct sockaddr *)&ss, &sl) == 0) from_sockaddr((struct sockaddr *)&ss, &peer);
    char ip[48];
    if (t5_net_addr_str(&peer, ip, sizeof ip) < 0) snprintf(ip, sizeof ip, "?");
    snprintf(s->hdr.what, sizeof s->hdr.what, "tcp from %.24s:%u", ip, peer.port);
    return publish(s, out);
}

/* For owned sockets. */
t5_err_t ns_addr(t5_sock_t *s, t5_net_addr_t *out, bool peer)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    int r = peer ? getpeername(s->fd, (struct sockaddr *)&ss, &sl) : getsockname(s->fd, (struct sockaddr *)&ss, &sl);
    if (r < 0) return T5_ERR_STATE;
    from_sockaddr((struct sockaddr *)&ss, out);
    return T5_OK;
}

void ns_set_stop(t5_sock_t *s, volatile bool *stop)
{
    s->stop = stop;
}

/* ---- UDP ----------------------------------------------------------------------------------- */
t5_err_t t5_udp_open(uint16_t local_port, uint32_t flags, t5_sock_t **out)
{
    if (!out) return T5_ERR_INVALID_ARG;
    *out = NULL;
    if (!quota_ok()) return T5_ERR_BUSY;
    bool v6 = flags & T5_UDP_IPV6;
    t5_sock_t *s = sock_new(SK_UDP, v6 ? 6 : 4);
    if (!s) return T5_ERR_NO_MEM;
    s->fd = socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    t5_err_t e = s->fd < 0 ? T5_ERR_NO_MEM : T5_OK;
    if (e == T5_OK) {
        int one = 1;
        set_nonblock(s->fd);
        if (flags & T5_UDP_REUSE) setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (flags & T5_UDP_BROADCAST) setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        t5_net_addr_t any = { .family = v6 ? 6 : 4, .port = local_port };
        struct sockaddr_storage ss;
        socklen_t sl = to_sockaddr(&any, &ss);
        if (bind(s->fd, (struct sockaddr *)&ss, sl) < 0) e = errno == EADDRINUSE ? T5_ERR_BUSY : T5_ERR_IO;
    }
    if (e != T5_OK) {
        ns_destroy(s);
        return e;
    }
    snprintf(s->hdr.what, sizeof s->hdr.what, "udp :%u", local_port);
    return publish(s, out);
}

static t5_sock_t *udp_acquire(t5_sock_t *s)
{
    if (!nh_acquire(s, NH_SOCK)) return NULL;
    if (s->kind != SK_UDP) {
        nh_release(s);
        return NULL;
    }
    return s;
}

t5_err_t t5_udp_connect(t5_sock_t *s, const t5_net_addr_t *to)
{
    if (!to || !udp_acquire(s)) return T5_ERR_INVALID_ARG;
    struct sockaddr_storage ss;
    socklen_t sl = to_sockaddr(to, &ss);
    int r = connect(s->fd, (struct sockaddr *)&ss, sl);
    nh_release(s);
    return r < 0 ? T5_ERR_IO : T5_OK;
}

int t5_udp_sendto(t5_sock_t *s, const t5_net_addr_t *to, const void *data, size_t n, uint32_t timeout_ms)
{
    if (!to || (!data && n) || n > 65507) return T5_ERR_INVALID_ARG;
    if (!udp_acquire(s)) return T5_ERR_INVALID_ARG;
    struct sockaddr_storage ss;
    socklen_t sl = to_sockaddr(to, &ss);
    uint32_t dl = net_deadline(timeout_ms);
    int r;
    for (;;) {
        if (sock_aborted(s)) {
            r = T5_ERR_STATE;
            break;
        }
        ssize_t w = sendto(s->fd, data, n, MSG_DONTWAIT, (struct sockaddr *)&ss, sl);
        if (w >= 0) {
            r = (int)w;
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOMEM && errno != EINTR) {
            r = T5_ERR_IO;
            break;
        }
        uint32_t left = net_left(dl, timeout_ms);
        if (!left) {
            r = T5_ERR_TIMEOUT;
            break;
        }
        if (wait_fd(s->fd, false, true, left) < 0) {
            r = T5_ERR_IO;
            break;
        }
    }
    nh_release(s);
    return r;
}

int t5_udp_recvfrom(t5_sock_t *s, void *buf, size_t n, t5_net_addr_t *from, uint32_t timeout_ms)
{
    if (!buf || !n) return T5_ERR_INVALID_ARG;
    if (!udp_acquire(s)) return T5_ERR_INVALID_ARG;
    uint32_t dl = net_deadline(timeout_ms);
    int r;
    for (;;) {
        if (sock_aborted(s)) {
            r = T5_ERR_STATE;
            break;
        }
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        ssize_t got = recvfrom(s->fd, buf, n, MSG_DONTWAIT, (struct sockaddr *)&ss, &sl);
        if (got >= 0) {
            if (from) from_sockaddr((struct sockaddr *)&ss, from);
            r = (int)got;
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            r = T5_ERR_IO;
            break;
        }
        uint32_t left = net_left(dl, timeout_ms);
        if (!left) {
            r = T5_ERR_TIMEOUT;
            break;
        }
        if (wait_fd(s->fd, true, false, left) < 0) {
            r = T5_ERR_IO;
            break;
        }
    }
    nh_release(s);
    return r;
}

static t5_err_t membership(t5_sock_t *s, const char *group, bool join)
{
    t5_net_addr_t g;
    if (!group || t5_net_addr_parse(group, &g) != T5_OK || g.family != 4 || (g.ip[0] & 0xf0) != 0xe0) {
        return T5_ERR_INVALID_ARG;
    }
    if (!udp_acquire(s)) return T5_ERR_INVALID_ARG;
    t5_err_t e = T5_OK;
    if (s->family != 4) {
        e = T5_ERR_NOT_SUPPORTED;
    } else {
        struct ip_mreq m = { 0 };
        memcpy(&m.imr_multiaddr.s_addr, g.ip, 4);
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(s->fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m, sizeof m) < 0) {
            e = T5_ERR_IO;
        }
    }
    nh_release(s);
    return e;
}

t5_err_t t5_udp_join(t5_sock_t *s, const char *group)
{
    return membership(s, group, true);
}

t5_err_t t5_udp_leave(t5_sock_t *s, const char *group)
{
    return membership(s, group, false);
}

t5_err_t t5_udp_multicast_opts(t5_sock_t *s, uint8_t ttl, bool loop)
{
    if (!udp_acquire(s)) return T5_ERR_INVALID_ARG;
    uint8_t t = ttl ? ttl : 1, l = loop;
    t5_err_t e = T5_OK;
    if (s->family != 4) e = T5_ERR_NOT_SUPPORTED;
    else if (setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_TTL, &t, sizeof t) < 0 ||
             setsockopt(s->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &l, sizeof l) < 0) {
        e = T5_ERR_IO;
    }
    nh_release(s);
    return e;
}

/* ---- poll ---------------------------------------------------------------------------------- */
/* Where to wait for one item: its fd (or -1), and whether it is ready
 * already from buffered data. */
static int poll_source(void *h, uint32_t magic, bool *buffered)
{
    int fd = -1;
    *buffered = false;
    if (magic == NH_SOCK) {
        t5_sock_t *s = h;
        fd = s->fd;
        *buffered = s->tls && ns_pending(s) > 0;
    } else if (magic == NH_HTTP) {
        *buffered = http_poll_buffered(h, &fd);
    } else if (magic == NH_WS) {
        *buffered = ws_poll_buffered(h, &fd);
    } else if (magic == NH_MQTT) {
        *buffered = mqtt_poll_buffered(h);
        fd = -2;                                     /* no fd: events come from esp-mqtt's task */
    } else if (magic == NH_HTTPD) {
        *buffered = httpd_poll_buffered(h, &fd);
    }
    return fd;
}

static int poll_once(t5_poll_t *items, int n, const uint32_t *magic, uint32_t left)
{
    fd_set r, w, e;
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_ZERO(&e);
    int maxfd = -1, ready = 0;
    int fds[POLL_MAX];
    for (int i = 0; i < n; i++) {
        fds[i] = -1;
        if (!magic[i]) continue;                     /* already marked ERR */
        bool buffered;
        fds[i] = poll_source(items[i].handle, magic[i], &buffered);
        if (((t5_nethdr_t *)items[i].handle)->closing) items[i].revents |= T5_POLL_ERR;
        if (buffered && (items[i].events & T5_POLL_IN)) items[i].revents |= T5_POLL_IN;
        if (fds[i] == -2) continue;                    /* checked again next slice */
        if (fds[i] < 0) {
            items[i].revents |= items[i].events & T5_POLL_IN;   /* nothing left to wait for: EOF */
            continue;
        }
        if (items[i].events & T5_POLL_IN) FD_SET(fds[i], &r);
        if (items[i].events & T5_POLL_OUT) FD_SET(fds[i], &w);
        FD_SET(fds[i], &e);
        if (fds[i] > maxfd) maxfd = fds[i];
    }
    for (int i = 0; i < n; i++) ready += items[i].revents != 0;
    if (ready) return ready;
    uint32_t ms = left < NET_SLICE_MS ? left : NET_SLICE_MS;
    if (maxfd < 0) {
        /* Only fd-less handles (MQTT): look again shortly. */
        t5_delay_ms(ms < 20 ? ms : 20);
        return 0;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)ms * 1000 };
    if (select(maxfd + 1, &r, &w, &e, &tv) < 0) return errno == EINTR ? 0 : T5_ERR_IO;
    for (int i = 0; i < n; i++) {
        if (fds[i] < 0) continue;
        if (FD_ISSET(fds[i], &r)) items[i].revents |= T5_POLL_IN;
        if (FD_ISSET(fds[i], &w)) items[i].revents |= T5_POLL_OUT;
        if (FD_ISSET(fds[i], &e)) items[i].revents |= T5_POLL_ERR;
        ready += items[i].revents != 0;
    }
    return ready;
}

int t5_net_poll(t5_poll_t *items, int n, uint32_t timeout_ms)
{
    if (!items || n < 0 || n > POLL_MAX) return T5_ERR_INVALID_ARG;
    uint32_t magic[POLL_MAX];
    for (int i = 0; i < n; i++) {
        items[i].revents = 0;
        magic[i] = 0;
        if (!nh_acquire_any(items[i].handle, &magic[i])) {
            magic[i] = 0;
            items[i].revents = T5_POLL_ERR;
        }
    }
    uint32_t dl = net_deadline(timeout_ms);
    int r;
    for (;;) {
        r = poll_once(items, n, magic, net_left(dl, timeout_ms));
        if (r != 0) break;
        if (net_cancelled()) {
            r = T5_ERR_STATE;
            break;
        }
        if (!net_left(dl, timeout_ms)) break;
        if (n == 0) t5_delay_ms(NET_SLICE_MS);
    }
    for (int i = 0; i < n; i++) {
        if (magic[i]) nh_release(items[i].handle);
    }
    return r;
}
