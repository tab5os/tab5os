#!/usr/bin/env python3
"""Network stack checks (t5_net sock/http/ws through t5.net) against a Tab5
on the same Wi-Fi as this machine.

This machine serves HTTP (plain, chunked, redirects, a 200 KB body, an echo
of method/headers/body), a TCP echo, a UDP echo and a WebSocket echo; the
Tab5 fetches the test script from it and runs it on the console's VM. The
script prints PASS/FAIL lines. Afterwards `net handles` must show nothing
left open. --external adds HTTPS/WSS checks against public servers (needs
internet).

    ~/.espressif/python_env/idf6.1_py3.15_env/bin/python test/hil/net_stack.py [--external]
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import re
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools", "t5"))
from t5link import Tab5  # noqa: E402

BASE = 8770           # HTTP; +1 TCP echo, +2 UDP echo, +3 WebSocket echo
DEVICE_LISTEN = 7781
BIG = bytes((i * 7) & 0xff for i in range(200_000))

DEVICE_SCRIPT = r'''
import t5, json, time
H = "%(host)s"
B = "http://%%s:%%d" %% (H, %(port)d)
fails = 0
def check(n, ok, extra=""):
    global fails
    print(("PASS " if ok else "FAIL ") + n + ("" if ok else " " + str(extra)))
    if not ok: fails += 1
def run(name, fn):
    try:
        fn()
    except Exception as e:
        check(name, False, "raised %%r" %% (e,))

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
print("DONE fails=%%d" %% fails)
'''


class Http(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    script = b""

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="text/plain", extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _route(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        p = self.path
        if p == "/hello":
            return self._send(200, b"hello")
        if p == "/test.py":
            return self._send(200, Http.script)
        if p == "/echo":
            j = {"method": self.command, "body": body.decode(errors="replace"),
                 "ctype": self.headers.get("Content-Type"), "xtest": self.headers.get("X-Test")}
            return self._send(200, json.dumps(j).encode(), "application/json")
        if p == "/redirect":
            return self._send(302, b"", extra=[("Location", "/hello")])
        if p == "/big":
            return self._send(200, BIG, "application/octet-stream")
        if p == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in (b"chunk-one|", b"chunk-two|", b"end"):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                self.wfile.flush()
                time.sleep(0.05)
            self.wfile.write(b"0\r\n\r\n")
            return None
        m = re.match(r"/connect\?port=(\d+)", p)
        if m:
            ip = self.client_address[0]
            threading.Thread(target=connect_back, args=(ip, int(m.group(1))), daemon=True).start()
            return self._send(200, b"ok")
        return self._send(404, b"no")

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = _route


def connect_back(ip, port):
    time.sleep(0.5)
    try:
        with socket.create_connection((ip, port), timeout=5) as s:
            s.sendall(b"hello-from-mac")
            s.recv(10)
    except OSError as e:
        print("connect back failed:", e)


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            d = self.request.recv(4096)
            if not d:
                return
            self.request.sendall(d)


class UdpEcho(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        sock.sendto(data, self.client_address)


def ws_frame(op, payload, fin=True):
    h = bytes([(0x80 if fin else 0) | op])
    n = len(payload)
    if n < 126:
        h += bytes([n])
    elif n < 65536:
        h += bytes([126]) + struct.pack(">H", n)
    else:
        h += bytes([127]) + struct.pack(">Q", n)
    return h + payload


def recv_exact(s, n):
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d:
            raise ConnectionError
        b += d
    return b


class WsEcho(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request
        head = b""
        while b"\r\n\r\n" not in head:
            head += s.recv(1024)
        hdrs = dict(l.split(": ", 1) for l in head.decode().split("\r\n")[1:] if ": " in l)
        acc = base64.b64encode(hashlib.sha1((hdrs["Sec-WebSocket-Key"] +
                                             "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        resp = b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        resp += b"Sec-WebSocket-Accept: " + acc + b"\r\n"
        if "chat" in hdrs.get("Sec-WebSocket-Protocol", ""):
            resp += b"Sec-WebSocket-Protocol: chat\r\n"
        s.sendall(resp + b"\r\n")
        try:
            while True:
                b0, b1 = recv_exact(s, 2)
                op, n = b0 & 0x0f, b1 & 0x7f
                if not b1 & 0x80:
                    print("ws server: unmasked frame %02x %02x: dropping the client" % (b0, b1), flush=True)
                    return            # clients must mask
                if n == 126:
                    n = struct.unpack(">H", recv_exact(s, 2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", recv_exact(s, 8))[0]
                mask = recv_exact(s, 4)
                data = bytes(c ^ mask[i & 3] for i, c in enumerate(recv_exact(s, n)))
                if op == 8:
                    s.sendall(ws_frame(8, data[:2]))
                    return
                if op == 9:
                    s.sendall(ws_frame(10, data))
                elif data == b"frag" and op == 1:
                    s.sendall(ws_frame(1, b"ab", fin=False) + ws_frame(9, b"!") + ws_frame(0, b"cd"))
                elif data == b"close" and op == 1:
                    s.sendall(ws_frame(8, struct.pack(">H", 4000) + b"bye"))
                elif op in (1, 2):
                    s.sendall(ws_frame(op, data))
        except (ConnectionError, OSError) as e:
            print("ws server: connection ended: %r" % (e,), flush=True)
            return


class TServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class HServer(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def hold_network(t):
    """Wake the Tab5 and hold a net wake lock: in Sleep without one the
    radio is powered off (t5_net). Waits for Wi-Fi. True if connected."""
    t.cmd("power wake")
    t.cmd("power lock net")
    # A fresh SDIO link: the Wi-Fi data path degrades over long sessions
    # (eh_sdio mempool OOM), see docs/bench-results.md.
    t.cmd("net reset", timeout=30)
    for _ in range(20):
        if "state connected" in t.cmd("net"):
            return True
        time.sleep(1)
    return False


def my_ip():
    for ifc in ("en0", "en1"):
        r = subprocess.run(["ipconfig", "getifaddr", ifc], capture_output=True, text=True)
        if r.stdout.strip():
            return r.stdout.strip()
    sys.exit("no Wi-Fi address on en0/en1")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--external", action="store_true")
    ap.add_argument("--host", help="this machine's address as the Tab5 sees it")
    a = ap.parse_args()
    host = a.host or my_ip()
    Http.script = (DEVICE_SCRIPT % {
        "host": host, "port": BASE, "dev_port": DEVICE_LISTEN, "big_len": len(BIG),
        "big_sum": sum(BIG[::997]), "external": "True" if a.external else "False",
    }).encode()
    servers = [HServer((host, BASE), Http), TServer((host, BASE + 1), Echo),
               socketserver.ThreadingUDPServer((host, BASE + 2), UdpEcho), TServer((host, BASE + 3), WsEcho)]
    for s in servers:
        threading.Thread(target=s.serve_forever, daemon=True).start()
    ok = False
    try:
        with Tab5(timeout=40) as t:
            if not hold_network(t):
                print("the Tab5 isn't on Wi-Fi:\n" + t.cmd("net"))
                return 1
            before = t.cmd("net handles")
            # py exec allows 30 s, py run 60 s: fetch the script into a file first.
            code = f"import t5;open('/data/user/_nettest.py','wb').write(t5.net.get('http://{host}:{BASE}/test.py').content)"
            fetched = t.cmd("py exec " + code, timeout=40)
            if "[done]" not in fetched:
                print("couldn't fetch the test script:\n" + fetched)
                return 1
            t0 = time.time()
            out = t.cmd("py run /data/user/_nettest.py", timeout=90)
            print(out)
            print(f"({time.time() - t0:.1f} s)")
            time.sleep(1)
            after = t.cmd("net handles")
            left = after.strip().splitlines()[-1]
            print("handles before:", before.strip().splitlines()[-1], "after:", left)
            t.cmd("power unlock net")
            fails = re.search(r"DONE fails=(\d+)", out)
            ok = bool(fails) and fails.group(1) == "0" and left.startswith("0 ")
    finally:
        for s in servers:
            s.shutdown()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
