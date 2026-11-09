#!/usr/bin/env python3
"""Network stack checks (t5_net sock/http/ws through t5.net) against a Tab5
on the same Wi-Fi as this machine.

This machine serves HTTP (plain, chunked, redirects, a 200 KB body, an echo
of method/headers/body), a TCP echo, a UDP echo and a WebSocket echo; the
Tab5 fetches the test script from it and runs it on the console's VM. The
script prints PASS/FAIL lines. Afterwards `net handles` must show nothing
left open. --external adds HTTPS/WSS checks against public servers (needs
internet).

    tools/t5/t5 hil net_stack [--external] [--host THIS_MACHINE]
"""
import argparse
import socketserver
import sys
import time

import _net as hn
from t5link import connect

DEVICE_LISTEN = 7781

DEVICE_SCRIPT = hn.SCRIPT_HEAD + r'''
def http_basic():
    r = t5.net.get(B + "/hello")
    check("get status", r.status == 200 and r.ok, r.status)
    check("get body", r.text() == "hello", r.content)
    check("get header", r.headers.get("content-type") == "text/plain", r.headers)
run("http basic", http_basic)

def http_methods():
    r = t5.net.post(B + "/echo", json={"a": 1}, headers={"X-Test": "yes"})
    j = r.json()
    check("post json body", json.loads(j["body"]) == {"a": 1}, j)
    check("post content-type", j["ctype"] == "application/json", j)
    check("custom header", j["xtest"] == "yes", j)
    for m in ("PUT", "PATCH", "DELETE"):
        r = t5.net.request(m.lower(), B + "/echo", data=b"x")
        check(m + " method", r.json()["method"] == m, r.content)
    r = t5.net.head(B + "/hello")
    check("HEAD empty", r.status == 200 and r.content == b"", r.content)
run("http methods", http_methods)

def http_framing():
    r = t5.net.get(B + "/chunked")
    check("chunked", r.text() == "chunk-one|chunk-two|end", r.content)
    r = t5.net.get(B + "/redirect")
    check("redirect followed", r.status == 200 and r.url.endswith("/hello"), (r.status, r.url))
    r = t5.net.get(B + "/redirect", redirects=False)
    check("redirect not followed", r.status == 302, r.status)
    r = t5.net.get(B + "/big")
    c = r.content
    check("200 KB body", len(c) == %(big_len)d and sum(c[i] for i in range(0, len(c), 997)) == %(big_sum)d, len(c))
    r = t5.net.get(B + "/big", stream=True)
    n = 0
    while True:
        part = r.read(7000)
        if not part: break
        n += len(part)
    r.close()
    check("streamed body", n == %(big_len)d, n)
    st, body = t5.net.http_get(B + "/hello")
    check("http_get compat", st == 200 and body == b"hello", (st, body))
    r = t5.net.get(B + "/missing")
    check("404", r.status == 404 and not r.ok, r.status)
run("http framing", http_framing)

def dns():
    check("resolve literal", t5.net.resolve(H) == H)
    check("resolve localhost", t5.net.resolve("localhost") == "127.0.0.1")
    try:
        t5.net.resolve("no-such-host.invalid", 5000)
        check("resolve failure", False, "resolved")
    except OSError:
        check("resolve failure", True)
run("dns", dns)

def tcp():
    with t5.net.tcp(H, %(port)d + 1) as s:
        data = bytes(range(256)) * 20
        check("tcp send", s.send(data) == len(data))
        got = b""
        while len(got) < len(data):
            part = s.recv(4096, 3000)
            if not part: break
            got += part
        check("tcp echo", got == data, len(got))
        check("tcp recv timeout", s.recv(100, 200) is None)
        check("tcp peer", s.peer()[1] == %(port)d + 1, s.peer())
run("tcp", tcp)

def udp():
    u = t5.net.udp()
    u.sendto(b"ping", (H, %(port)d + 2))
    got = u.recvfrom(100, 2000)
    check("udp echo", got is not None and got[0] == b"ping", got)
    u.connect((H, %(port)d + 2))
    u.send(b"pong")
    check("udp connected", u.recv(100, 2000) == b"pong")
    u.close()
run("udp", udp)

def ws():
    w = t5.net.websocket("ws://%%s:%%d/echo" %% (H, %(port)d + 3), protocols=["chat"])
    check("ws subprotocol", w.subprotocol == "chat", w.subprotocol)
    w.send("hi")
    check("ws text", w.recv(3000) == "hi")
    w.send(b"\x00\x01\x02")
    check("ws binary", w.recv(3000) == b"\x00\x01\x02")
    big = bytes(i & 0xff for i in range(40000))
    w.send(big)
    check("ws 40 KB", w.recv(5000) == big)
    w.send("frag")
    check("ws fragments joined", w.recv(3000) == "abcd")
    w.ping(b"p")
    w.send("after-ping")
    check("ws after ping", w.recv(3000) == "after-ping")
    check("ws recv timeout", w.recv(200) is None and not w.closed)
    w.send("close")
    r = w.recv(3000)
    check("ws server close", r is None and w.closed and w.close_code == 4000 and w.close_reason == "bye",
          (r, w.close_code, w.close_reason))
    w.close()
run("websocket", ws)

def poll():
    u = t5.net.udp()
    w = t5.net.websocket("ws://%%s:%%d/" %% (H, %(port)d + 3))
    check("poll timeout", t5.net.poll([u, w], 200) == [])
    u.sendto(b"x", (H, %(port)d + 2))
    r = t5.net.poll([u, w], 2000)
    check("poll udp ready", r == [u], r)
    u.recvfrom()
    w.send("y")
    r = t5.net.poll([u, w], 2000)
    check("poll ws ready", r == [w], r)
    u.close()
    w.close()
run("poll", poll)

def server():
    srv = t5.net.listen(%(dev_port)d)
    t5.net.get(B + "/connect?port=%(dev_port)d")
    got = srv.accept(5000)
    check("accept", got is not None)
    if got:
        c, addr = got
        check("accept addr", addr[0] == H, addr)
        check("server recv", c.recv(100, 3000) == b"hello-from-mac")
        c.send(b"bye")
        c.close()
    srv.close()
run("server", server)

def timeouts():
    t0 = time.ticks_ms()
    try:
        t5.net.tcp("10.255.255.1", 80, timeout=1500)
        check("connect timeout", False, "connected")
    except OSError as e:
        dt = time.ticks_diff(time.ticks_ms(), t0)
        check("connect timeout", 1200 < dt < 3500, dt)
run("timeouts", timeouts)

if %(external)s:
    def external():
        r = t5.net.get("https://example.com/")
        check("https get", r.status == 200 and b"Example Domain" in r.content, r.status)
        s = t5.net.tcp("example.com", 443, tls=True)
        s.send(b"HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n")
        check("raw tls", (s.recv(64, 5000) or b"").startswith(b"HTTP/1.1"))
        s.close()
        try:
            t5.net.get("https://self-signed.badssl.com/")
            check("bad cert refused", False, "accepted")
        except OSError:
            check("bad cert refused", True)
        r = t5.net.get("https://self-signed.badssl.com/", verify=False)
        check("verify=False", r.status == 200, r.status)
        w = t5.net.websocket("wss://echo.websocket.org/")
        w.recv(5000)                     # its banner
        w.send("tab5")
        check("wss echo", w.recv(5000) == "tab5")
        w.close()
    run("external", external)
''' + hn.SCRIPT_TAIL


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--external", action="store_true")
    ap.add_argument("--host", help="this machine's address as the Tab5 sees it")
    a = ap.parse_args()
    big = hn.BIG
    with connect(cmd_timeout=40) as t:
        if not hn.hold_network(t):
            print("the Tab5 isn't on Wi-Fi:\n" + t.cmd("net"))
            return 1
        host = a.host or hn.my_ip(hn.tab5_ip(t))
        hn.Http.script = (DEVICE_SCRIPT % {
            "host": host, "port": hn.BASE, "dev_port": DEVICE_LISTEN, "big_len": len(big),
            "big_sum": sum(big[::997]), "external": "True" if a.external else "False",
        }).encode()
        base = hn.BASE
        servers = hn.serve([hn.HServer((host, base), hn.Http), hn.TServer((host, base + 1), hn.Echo),
                                socketserver.ThreadingUDPServer((host, base + 2), hn.UdpEcho),
                                hn.TServer((host, base + 3), hn.WsEcho)])
        try:
            before = t.cmd("net handles")
            t0 = time.time()
            out = hn.run_script(t, host, "nettest")
            if out is None:
                print("couldn't fetch the test script")
                return 1
            print(out)
            print(f"({time.time() - t0:.1f} s)")
            left = hn.handles_left(t)
            print("handles before:", before.strip().splitlines()[-1], "after:", left)
            t.cmd("power unlock net")
        finally:
            hn.stop(servers)
    ok = hn.passed(out) and left.startswith("0 ")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
