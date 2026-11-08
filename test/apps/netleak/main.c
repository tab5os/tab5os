/* Network stack test (t5/net.h): opens a TCP server, a UDP socket and a
 * TCP connection to its own server over loopback, checks that bytes flow,
 * and then leaks all of it. After the app exits, `net handles` on the
 * console must not list any of them (the app runtime closed them). */
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <t5/app.h>
#include <t5/net.h>
#include <t5/ui.h>

static void on_create(t5_app_t *app)
{
    t5_sock_t *srv = NULL, *cli = NULL, *acc = NULL, *udp = NULL;
    t5_err_t e1 = t5_tcp_listen(0, 2, &srv);
    t5_net_addr_t local = { 0 };
    t5_err_t e2 = srv ? t5_sock_local_addr(srv, &local) : T5_ERR_STATE;
    t5_err_t e3 = t5_tcp_connect("127.0.0.1", local.port, NULL, &cli);
    t5_err_t e4 = srv ? t5_tcp_accept(srv, 2000, &acc) : T5_ERR_STATE;
    int sent = cli ? t5_sock_send(cli, "loop", 4, 1000) : -1;
    char buf[8] = { 0 };
    int got = acc ? t5_sock_recv(acc, buf, sizeof buf - 1, 1000) : -1;
    t5_err_t e5 = t5_udp_open(0, T5_UDP_BROADCAST, &udp);
    printf("netleak: listen %d addr %d port %u connect %d accept %d sent %d got %d '%s' udp %d\n", e1, e2,
           local.port, e3, e4, sent, got, buf, e5);
    printf("netleak: %s\n", (e1 | e2 | e3 | e4 | e5) == 0 && sent == 4 && got == 4 && !strcmp(buf, "loop")
                                ? "PASS" : "FAIL");
    /* Raw BSD sockets and errno (lwIP underneath): one leaked, one closed. lwIP
     * reports a refused connect as ECONNRESET. */
    int udpfd = socket(AF_INET, SOCK_DGRAM, 0);
    int tcpfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(1), .sin_addr.s_addr = htonl(0x7f000001) };
    int cr = connect(tcpfd, (struct sockaddr *)&to, sizeof to);
    int err = errno;
    close(tcpfd);
    t5_httpd_t *web = NULL;
    t5_err_t e6 = t5_httpd_start(0, NULL, &web);
    printf("netleak: bsd udp %d connect %d errno %d (ECONNREFUSED %d) httpd %d port %u\n", udpfd, cr, err, ECONNREFUSED,
           e6, web ? t5_httpd_port(web) : 0);
    printf("netleak: bsd %s\n", udpfd >= 0 && cr < 0 && (err == ECONNREFUSED || err == ECONNRESET) && e6 == T5_OK ? "PASS" : "FAIL");
    lv_obj_t *l = md_label_create(t5_app_screen(app), "netleak: sockets left open", MD_TYPE_BODY1);
    lv_obj_center(l);
}

T5_APP(.on_create = on_create);
