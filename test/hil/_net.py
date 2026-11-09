# SPDX-License-Identifier: Apache-2.0
"""What the network bench scripts (net_stack.py, net_compat.py) share: the
servers this machine runs for the Tab5 (HTTP, TCP/UDP/WebSocket echo), the
head and tail of the test script the Tab5 runs, and running it there."""
import base64
import hashlib
import http.server
import json
import re
import socket
import socketserver
import struct
import threading
import time

import t5const

BASE = 8770           # HTTP; +1 TCP echo, +2 UDP echo, +3 WebSocket echo
BIG = bytes((i * 7) & 0xff for i in range(200_000))
USER_DIR = t5const.define("t5_storage/include/t5_storage/paths.h", "T5_USER_DIR")

# Device script head, formatted with %% (host, port): check() prints the
# PASS/FAIL lines; run() turns an exception into a failure.
SCRIPT_HEAD = r'''
import t5, json, time, sys
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
        sys.print_exception(e)
        check(name, False, "raised %%r" %% (e,))
'''
SCRIPT_TAIL = '''
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
            return self._send(200, self.script)
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


def connect_retry(ip, port, timeout=5.0, **kw):
    """A connection to a server the Tab5 is starting: retried while it refuses."""
    end = time.time() + timeout
    while True:
        try:
            return socket.create_connection((ip, port), timeout=timeout, **kw)
        except ConnectionRefusedError:
            if time.time() >= end:
                raise
            time.sleep(0.1)


def connect_back(ip, port):
    try:
        with connect_retry(ip, port) as s:
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


def serve(servers):
    """Start servers on daemon threads; returns them, for stop()."""
    for s in servers:
        threading.Thread(target=s.serve_forever, daemon=True).start()
    return servers


def stop(servers):
    for s in servers:
        s.shutdown()


def hold_network(t):
    """Wake the Tab5 and hold a net wake lock: in Sleep without one the
    radio is powered off (t5_net). Waits for Wi-Fi. True if connected."""
    t.cmd("power wake")
    t.cmd("power lock net")
    # A fresh SDIO link: the Wi-Fi data path degrades over long sessions
    # (eh_sdio mempool OOM), see docs/bench-results.md.
    t.cmd("net reset", timeout=30)
    return t.poll("net", r"state connected", 20, every=1) is not None


def my_ip(tab5_ip):
    """This machine's address on the route to the Tab5."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((tab5_ip, 9))          # no packet is sent: this only picks the route
        return s.getsockname()[0]


def tab5_ip(t):
    m = re.search(r"^ip (\d+\.\d+\.\d+\.\d+)", t.cmd("net"), re.M)
    return m.group(1) if m else None


def run_script(t, host, name, timeout=90):
    """Have the Tab5 fetch /test.py from this machine's Http server into its
    user folder, run it, and delete it. Returns its output, or None if the
    fetch failed."""
    path = f"{USER_DIR}/_{name}.py"
    # py exec allows 30 s, py run 60 s: fetch the script into a file first.
    code = f"import t5;open('{path}','wb').write(t5.net.get('http://{host}:{BASE}/test.py').content)"
    if "[done]" not in t.cmd("py exec " + code, timeout=40):
        return None
    try:
        return t.cmd(f"py run {path}", timeout=timeout)
    finally:
        t.cmd(f"py exec import os;os.remove('{path}')")


def handles_left(t, timeout=5):
    """The last line of `net handles` ("N open") once nothing is open, or as
    it is after timeout: closing finishes in the background."""
    m = t.poll("net handles", r"^0 open", timeout)
    return m.group(0) if m else t.cmd("net handles").strip().splitlines()[-1]


def passed(out):
    m = re.search(r"DONE fails=(\d+)", out or "")
    return bool(m) and m.group(1) == "0"
