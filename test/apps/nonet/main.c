/* Without the net permission every t5/net.h call is bound to a stub. */
#include <stdio.h>
#include <t5/app.h>
#include <t5/net.h>
#include <t5/ui.h>

static void on_create(t5_app_t *app)
{
    t5_sock_t *s = NULL;
    t5_http_t *h = NULL;
    t5_http_req_t req = { .url = "http://127.0.0.1/" };
    t5_err_t a = t5_udp_open(0, 0, &s);
    t5_err_t b = t5_http_open(&req, &h);
    t5_net_addr_t ad;
    t5_err_t c = t5_net_addr_parse("10.0.0.1", &ad);     /* no permission needed */
    printf("nonet: udp %d http %d parse %d -> %s\n", a, b, c,
           a == T5_ERR_DENIED && b == T5_ERR_DENIED && c == T5_OK && !s && !h ? "PASS" : "FAIL");
    lv_obj_center(md_label_create(t5_app_screen(app), "nonet", MD_TYPE_BODY1));
}

T5_APP(.on_create = on_create);
