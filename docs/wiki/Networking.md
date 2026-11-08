# Networking

Everything needs `net = true`. Everything blocks, so everything runs on a [worker task](Threading.md) with a 12 KB stack if it uses TLS. Include `<t5/net.h>`, and `<t5/sys.h>` for status. Fragments on this page leave out error handling. The [recipes](Cookbook.md) don't.

## Which API

| Need | Use |
| --- | --- |
| Fetch a small resource | `t5_http_fetch`: the whole body into your buffer |
| Download, or a large response | `t5_http_open`, `t5_http_read`, `t5_http_close`: streamed |
| Live two-way stream | `t5_ws_*` |
| Pub/sub, IoT | `t5_mqtt_*` |
| Your own protocol | `t5_tcp_*`, `t5_udp_*` |
| Serve a web page or API | `t5_httpd_*` |
| Wait on several | `t5_net_poll` |
| Porting existing code | BSD sockets, with lwIP behaviour |

The `t5_*` APIs are preferred over raw sockets. They hold the net wake lock, respect close-from-another-task, count against the handle limit and clean up on exit.

## Conventions

Timeouts are in milliseconds. `0` doesn't wait, and `T5_NET_FOREVER` waits until something happens. Calls that move bytes return a count, or a negative `t5_err_t`. A receive of 0 means the peer closed.

| Return | Meaning |
| --- | --- |
| `> 0` | Bytes moved |
| `0`, on receive | Peer closed |
| `T5_ERR_TIMEOUT` | Nothing in time: often just loop |
| `T5_ERR_IO` | Connection failed: close and reconnect |
| `T5_ERR_STATE` | The handle was closed |

Up to 12 handles at once. Closing a handle from another task ends a wait on it within about 50 ms, which is how you cancel.

## Is the network up?

```c
t5_net_status_t st;
t5_net_status(&st);
if (st.state != T5_NET_CONNECTED) show_offline();
```

States: `T5_NET_OFF`, `T5_NET_STARTING`, `T5_NET_IDLE`, `T5_NET_CONNECTING`, `T5_NET_CONNECTED`, `T5_NET_FAILED`. Apps don't join networks. People do that in Settings. Subscribe to `T5_EV_NET_GOT_IP` to react to connection, and handle it dropping at any moment. After wake, Wi-Fi takes a few seconds.

## HTTP

```c
/* Whole body. The result is its length, or a negative error. */
char buf[8192];
int status;
t5_http_req_t req = { .url = "https://api.example.com/v1/state" };
int n = t5_http_fetch(&req, buf, sizeof buf - 1, &status);
if (n >= 0 && status == 200) { buf[n] = 0; parse(buf); }
```

```c
/* POST with headers */
t5_http_header_t hdr[] = { { "Authorization", "Bearer xyz" }, { "Content-Type", "application/json" } };
t5_http_req_t req = { .method = "POST", .url = url, .headers = hdr, .n_headers = 2,
                      .body = json, .body_len = strlen(json) };
```

```c
/* Streaming */
t5_http_t *h;
if (t5_http_open(&req, &h) == T5_OK) {
    char chunk[1024];
    for (int n; (n = t5_http_read(h, chunk, sizeof chunk, 10000)) > 0;) consume(chunk, n);
    T5_IGNORE(t5_http_close(h));          /* nothing to do if closing fails */
}
```

A body bigger than the buffer returns `T5_ERR_NO_MEM`. Redirects are followed. `t5_http_header(h, "ETag")` reads response headers. Response bodies must be parsed defensively: treat them as hostile.

## WebSocket

```c
t5_ws_t *ws;
if (t5_ws_connect("wss://host/feed", &(t5_ws_cfg_t){ .subprotocols = "v1" }, &ws) != T5_OK) return;
if (t5_ws_send(ws, T5_WS_TEXT, "hello", 5, 5000) < 0) { /* reconnect */ }
t5_ws_op_t op;
int n = t5_ws_recv(ws, buf, sizeof buf, &op, 500);
```

Messages arrive whole, and pings are answered for you. A message bigger than the buffer returns `T5_ERR_NO_MEM` and stays queued: `t5_ws_next_len` gives its size. See the [reconnecting recipe](Cookbook.md#reconnecting-websocket).

## MQTT

```c
t5_mqtt_t *m;
t5_mqtt_cfg_t cfg = { .uri = "mqtts://broker.example.com", .username = "me", .password = "pw" };
if (t5_mqtt_connect(&cfg, &m) != T5_OK) return;
t5_mqtt_subscribe(m, "home/+/temp", 1);

while (t5_app_running(app)) {
    t5_mqtt_event_t ev;
    t5_err_t e = t5_mqtt_next(m, &ev, buf, sizeof buf, 500);
    if (e == T5_OK && ev.type == T5_MQTT_EV_MESSAGE) use(ev.topic, ev.data, ev.len);
}
```

Reconnection is automatic. Events are taken on your task, and no callbacks run behind your back. If you don't take messages fast enough they're dropped, and `t5_mqtt_dropped` counts them. Supports MQTT 3.1.1 and 5, with `mqtt://`, `mqtts://`, `ws://`, `wss://`, QoS 0 to 2 and last will.

## TCP and UDP

```c
t5_sock_t *s;
t5_tcp_cfg_t cfg = { .flags = T5_TCP_TLS | T5_TCP_NODELAY, .timeout_ms = 8000 };
if (t5_tcp_connect("example.com", 443, &cfg, &s) == T5_OK) { ... }

t5_sock_t *u;
t5_udp_open(5000, T5_UDP_BROADCAST, &u);   /* t5_udp_sendto, t5_udp_recvfrom, t5_udp_join */
```

`t5_sock_recv` returns what's arrived, at least one byte. `t5_sock_send` sends all bytes or returns fewer on timeout.

## Servers

```c
t5_httpd_t *srv;
T5_IGNORE(t5_httpd_start(8080, NULL, &srv));
while (t5_app_running(app)) {
    t5_httpd_req_t *req;
    if (t5_httpd_next(srv, 500, &req) != T5_OK) continue;
    T5_IGNORE(t5_httpd_respond(req, 200, "application/json", "{}", 2, NULL, 0));
}
```

Every request must be answered, which frees it. Servers listen on every interface and nothing authenticates for you, so think about who's on the network.

## TLS

`T5_TCP_TLS`, `https://`, `wss://` and `mqtts://` verify certificates against the built-in Mozilla bundle.

| Field | Use |
| --- | --- |
| `ca_pem` | Trust one CA instead of the bundle: for a private server |
| `cert_pem`, `key_pem` | Client certificate, or a server's own |
| `T5_TCP_TLS_NO_VERIFY`, `T5_HTTP_TLS_NO_VERIFY`, `T5_MQTT_TLS_NO_VERIFY` | Skip verification. **Testing only** |

Each TLS connection takes tens of KB of PSRAM, and its task needs a 12 KB stack. Reuse connections when you can.

The clock must be right for certificates to validate. Check `t5_time_valid()`, since a wrong date shows up as a failed handshake.

## BSD sockets

`<sys/socket.h>` and `<lwip/netdb.h>` work, and at most 8 per app are allowed. Differences from what you may expect:

- `getaddrinfo` and friends are exported as `lwip_getaddrinfo`, `lwip_socket`, and so on, and the headers map them.
- A refused connect reports `ECONNRESET`.
- They don't hold a wake lock. Take `T5_LOCK_NET` yourself across Sleep.

## Sleep

While any `t5/net.h` handle is open, Wi-Fi stays connected through Sleep. With none open, Sleep turns the radio off. A long-lived connection therefore keeps the radio on and costs battery. Close it when you're not using it, or reconnect periodically instead.

## Debugging

`net` on the console shows state, `net handles` lists every open handle and its owner, and `net reset` restarts the link. The handle-leak test is `test/apps/netleak`.
