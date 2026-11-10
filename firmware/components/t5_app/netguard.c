/* SPDX-License-Identifier: Apache-2.0
 * App tasks inside lwIP (isolate.h, "network calls").
 *
 * The linker wraps lwIP's socket calls (CMakeLists.txt), so every one is
 * seen: an app's own (bound through the ABI table), and those OS code makes
 * on an app task (t5_net's select slices, esp-tls, VFS read/write/select).
 * On a task with an app context the call is counted in a TLS word; OS tasks
 * pay one TLS read. The exit (lifecycle.c, apps_hw.c) reads the count of a
 * stopped task to decide whether it may park or delete it.
 */
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "t5_app/isolate.h"
#include "t5_app/tracker.h"
#include "t5_core/log.h"
#include "t5_core/os.h"
#include "t5_core/task_tls.h"
#include "app_priv.h"
#include "apps_hw.h"

static const char *TAG = "t5.app";

/* The word changes on its own task (enter, leave) and on the exiting UI
 * task (orphan): each read-modify-write is in here, so a task stopped by
 * vTaskSuspend never stops halfway through one. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static t5_iso_net_t word_get(TaskHandle_t t)
{
    return (t5_iso_net_t)(uintptr_t)pvTaskGetThreadLocalStoragePointer(t, T5_TASK_TLS_NET_GUARD);
}

static void word_set(TaskHandle_t t, t5_iso_net_t w)
{
    vTaskSetThreadLocalStoragePointer(t, T5_TASK_TLS_NET_GUARD, (void *)(uintptr_t)w);
}

/* The calling task is one of its exiting app's own (not the UI task or an
 * event dispatcher running the app's callbacks). As file_enter in wrappers.c. */
static bool exiting_own(t5_app_t *a)
{
    if (a->hdr.magic != T5_APP_CTX_MAGIC || !a->exiting) return false;
    apps_lock();
    bool mine = t5_tracker_has(&a->trk, t5_task_self());
    apps_unlock();
    return mine;
}

/* 1: counted, 0: not an app's task, -1: refused (errno set). */
static int enter(bool waits)
{
    t5_app_t *a = t5_app_ctx_get();
    if (!a) return 0;
    bool exiting = waits && exiting_own(a);
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    taskENTER_CRITICAL(&s_mux);
    t5_iso_net_t w = word_get(self);
    bool ok = t5_iso_net_enter(&w, waits, exiting);
    word_set(self, w);
    taskEXIT_CRITICAL(&s_mux);
    if (ok) return 1;
    errno = ECANCELED;
    return -1;
}

static void leave(int g)
{
    if (g <= 0) return;
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    taskENTER_CRITICAL(&s_mux);
    t5_iso_net_t w = word_get(self);
    bool orphaned = t5_iso_net_leave(&w);
    word_set(self, w);
    taskEXIT_CRITICAL(&s_mux);
    if (!orphaned) return;
    /* Its app is gone (and maybe its code): never return to the caller. */
    T5_LOGW(TAG, "%s: left its network call after its app exited; ending it", pcTaskGetName(NULL));
    word_set(self, 0);
    t5_task_delete(NULL);
}

bool apps_hw_net_inside(void *task)
{
    taskENTER_CRITICAL(&s_mux);
    bool in = t5_iso_net_inside(word_get((TaskHandle_t)task));
    taskEXIT_CRITICAL(&s_mux);
    return in;
}

bool apps_hw_net_orphan(void *task)
{
    taskENTER_CRITICAL(&s_mux);
    t5_iso_net_t w = word_get((TaskHandle_t)task);
    bool del = t5_iso_net_delete(&w);
    word_set((TaskHandle_t)task, w);
    taskEXIT_CRITICAL(&s_mux);
    return !del;
}

/* ---- the wraps ------------------------------------------------------------------------ */
/* WAITS: may wait on the network, refused once the app is exiting. QUICK:
 * always allowed (closing must still work), but counted: without core
 * locking each one still waits for the tcpip thread on the task's
 * semaphore. errno is the call's: leave() doesn't touch it unless the task
 * ends there. */
#define WAITS true
#define QUICK false
#define WRAP(ret, fail, name, waits, params, args)            \
    ret __real_##name params;                                 \
    ret __wrap_##name params;                                 \
    ret __wrap_##name params                                  \
    {                                                         \
        int guard_ = enter(waits);                            \
        if (guard_ < 0) return fail;                          \
        ret ret_ = __real_##name args;                        \
        int errno_ = errno;                                   \
        leave(guard_);                                        \
        errno = errno_;                                       \
        return ret_;                                          \
    }

WRAP(int, -1, lwip_socket, WAITS, (int d, int t, int p), (d, t, p))
WRAP(int, -1, lwip_accept, WAITS, (int s, struct sockaddr *a, socklen_t *l), (s, a, l))
WRAP(int, -1, lwip_bind, WAITS, (int s, const struct sockaddr *a, socklen_t l), (s, a, l))
WRAP(int, -1, lwip_listen, WAITS, (int s, int b), (s, b))
WRAP(int, -1, lwip_connect, WAITS, (int s, const struct sockaddr *a, socklen_t l), (s, a, l))
WRAP(ssize_t, -1, lwip_recv, WAITS, (int s, void *m, size_t n, int f), (s, m, n, f))
WRAP(ssize_t, -1, lwip_recvfrom, WAITS, (int s, void *m, size_t n, int f, struct sockaddr *a, socklen_t *l),
     (s, m, n, f, a, l))
WRAP(ssize_t, -1, lwip_recvmsg, WAITS, (int s, struct msghdr *m, int f), (s, m, f))
WRAP(ssize_t, -1, lwip_read, WAITS, (int s, void *m, size_t n), (s, m, n))
WRAP(ssize_t, -1, lwip_readv, WAITS, (int s, const struct iovec *v, int c), (s, v, c))
WRAP(ssize_t, -1, lwip_send, WAITS, (int s, const void *m, size_t n, int f), (s, m, n, f))
WRAP(ssize_t, -1, lwip_sendto, WAITS, (int s, const void *m, size_t n, int f, const struct sockaddr *a, socklen_t l),
     (s, m, n, f, a, l))
WRAP(ssize_t, -1, lwip_sendmsg, WAITS, (int s, const struct msghdr *m, int f), (s, m, f))
WRAP(ssize_t, -1, lwip_write, WAITS, (int s, const void *m, size_t n), (s, m, n))
WRAP(ssize_t, -1, lwip_writev, WAITS, (int s, const struct iovec *v, int c), (s, v, c))
WRAP(int, -1, lwip_select, WAITS, (int n, fd_set *r, fd_set *w, fd_set *x, struct timeval *t), (n, r, w, x, t))
WRAP(int, -1, lwip_poll, WAITS, (struct pollfd *f, nfds_t n, int t), (f, n, t))
WRAP(int, EAI_FAIL, lwip_getaddrinfo, WAITS, (const char *n, const char *s, const struct addrinfo *h,
                                             struct addrinfo **r), (n, s, h, r))
WRAP(struct hostent *, NULL, lwip_gethostbyname, WAITS, (const char *n), (n))
WRAP(int, -1, lwip_gethostbyname_r, WAITS, (const char *n, struct hostent *h, char *b, size_t l,
                                           struct hostent **r, int *e), (n, h, b, l, r, e))
WRAP(int, -1, lwip_close, QUICK, (int s), (s))
WRAP(int, -1, lwip_shutdown, QUICK, (int s, int h), (s, h))
WRAP(int, -1, lwip_getsockopt, QUICK, (int s, int lv, int o, void *v, socklen_t *l), (s, lv, o, v, l))
WRAP(int, -1, lwip_setsockopt, QUICK, (int s, int lv, int o, const void *v, socklen_t l), (s, lv, o, v, l))
WRAP(int, -1, lwip_getpeername, QUICK, (int s, struct sockaddr *a, socklen_t *l), (s, a, l))
WRAP(int, -1, lwip_getsockname, QUICK, (int s, struct sockaddr *a, socklen_t *l), (s, a, l))
WRAP(int, -1, lwip_ioctl, QUICK, (int s, long c, void *a), (s, c, a))
WRAP(int, -1, lwip_fcntl, QUICK, (int s, int c, int v), (s, c, v))
