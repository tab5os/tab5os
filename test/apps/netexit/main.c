/* Exit while app tasks are blocked inside network calls. Four workers, each
 * stuck in a different place in lwIP, all over loopback to a listener of
 * the app's that never answers:
 *
 *   nx.http  t5_http_fetch, 15 s timeout: OS code (t5_net) waiting in
 *            select() slices on the app's task, as Weather's worker was
 *   nx.sel   select() on a raw socket with no timeout
 *   nx.recv  blocking recv() on a raw socket
 *   nx.acc   blocking accept() on a listener nobody connects to
 *
 * It prints "netexit: armed" once all four are inside their calls; then
 * `app exit` on the console. Before the fix the device rebooted (the tcpip
 * task signalled a freed select semaphore); now the app closes and
 * `net handles` lists nothing of it. test/hil/net_exit.py runs it in a loop. */
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <t5/app.h>
#include <t5/net.h>
#include <t5/ui.h>

static volatile int s_armed;
static int s_quiet = -1, s_lonely = -1;   /* listeners: one never accepts, one has no clients */
static uint16_t s_quiet_port;

static int listener(uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(0x7f000001) };
    socklen_t l = sizeof a;
    if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 4) ||
        getsockname(fd, (struct sockaddr *)&a, &l)) {
        printf("netexit: listener failed\n");
        return -1;
    }
    if (port) *port = ntohs(a.sin_port);
    return fd;
}

static int dial(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(s_quiet_port),
                             .sin_addr.s_addr = htonl(0x7f000001) };
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) {
        printf("netexit: connect failed\n");
        return -1;
    }
    return fd;
}

static void http_worker(void *arg)
{
    (void)arg;
    char url[48], buf[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/", s_quiet_port);
    t5_http_req_t req = { .url = url, .timeout_ms = 15000 };
    int status = 0;
    __atomic_add_fetch(&s_armed, 1, __ATOMIC_SEQ_CST);
    int n = t5_http_fetch(&req, buf, sizeof buf, &status);
    printf("netexit: http returned %d (status %d)\n", n, status);
}

static void select_worker(void *arg)
{
    (void)arg;
    int fd = dial();
    fd_set r;
    FD_ZERO(&r);
    if (fd >= 0) FD_SET(fd, &r);
    __atomic_add_fetch(&s_armed, 1, __ATOMIC_SEQ_CST);
    int n = select(fd + 1, &r, NULL, NULL, NULL);
    printf("netexit: select returned %d\n", n);
}

static void recv_worker(void *arg)
{
    (void)arg;
    int fd = dial();
    char b[8];
    __atomic_add_fetch(&s_armed, 1, __ATOMIC_SEQ_CST);
    int n = (int)recv(fd, b, sizeof b, 0);
    printf("netexit: recv returned %d\n", n);
}

static void accept_worker(void *arg)
{
    (void)arg;
    __atomic_add_fetch(&s_armed, 1, __ATOMIC_SEQ_CST);
    int fd = accept(s_lonely, NULL, NULL);
    printf("netexit: accept returned %d\n", fd);
}

/* "armed" once every worker has had time to get into its call. */
static void arm_watch(void *arg)
{
    (void)arg;
    while (__atomic_load_n(&s_armed, __ATOMIC_SEQ_CST) < 4) t5_delay_ms(10);
    t5_delay_ms(300);
    printf("netexit: armed\n");
}

static void on_create(t5_app_t *app)
{
    s_armed = 0;
    s_quiet = listener(&s_quiet_port);
    s_lonely = listener(NULL);
    static const struct {
        const char *name;
        t5_task_fn_t fn;
    } w[] = { { "nx.http", http_worker }, { "nx.sel", select_worker }, { "nx.recv", recv_worker },
              { "nx.acc", accept_worker }, { "nx.arm", arm_watch } };
    for (size_t i = 0; s_quiet >= 0 && s_lonely >= 0 && i < sizeof w / sizeof w[0]; i++) {
        if (t5_task_create(w[i].name, w[i].fn, NULL, 6 * 1024, 2, T5_CORE_ANY, NULL) != T5_OK) {
            printf("netexit: couldn't start %s\n", w[i].name);
        }
    }
    lv_obj_t *l = md_label_create(t5_app_screen(app), "netexit: blocked in the network", MD_TYPE_BODY1);
    lv_obj_center(l);
}

T5_APP(.on_create = on_create);
