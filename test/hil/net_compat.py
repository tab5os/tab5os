#!/usr/bin/env python3
"""Standard-library networking on a Tab5: does ordinary Python code run?

Checks, from the Tab5's Python: socket (TCP client and server, UDP),
select.poll, ssl (against a TLS server here, with its CA and without), asyncio
(open_connection, start_server), requests/urequests, aiohttp (HTTP and
WebSocket), umqtt.simple and t5.net.mqtt (against a small MQTT broker here),
network.WLAN, deflate, and the Tab5's own servers: t5.net.serve over HTTP
and HTTPS, and a WebSocket upgrade, with this machine as the client.
--external adds checks against public servers (needs internet).

This machine runs the servers from _net.py plus a TLS echo, a minimal
MQTT 3.1.1 broker and client threads the Tab5 starts through HTTP. Ends with
`net handles` showing nothing left open.

    tools/t5/t5 hil net_compat [--external] [--host THIS_MACHINE]
"""
import argparse
import base64
import json
import os
import re
import socketserver
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

import _net as hn
from t5link import connect

BASE = hn.BASE
MQTT_PORT = BASE + 4
TLS_PORT = BASE + 5
DEV_TCP, DEV_HTTP, DEV_HTTPS, DEV_ASYNC = 7782, 7783, 7784, 7785
RESULTS = {}

DEVICE_SCRIPT = hn.SCRIPT_HEAD + r'''
CA = """%(ca)s"""
CERT = """%(dev_cert)s"""
KEY = """%(dev_key)s"""
def result(name, wait=5000):
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < wait:
        r = t5.net.get(B + "/result?name=" + name).json()
        if r is not None: return r
        time.sleep_ms(100)

import socket, select
def sockets():
    s = socket.socket()
    s.connect((H, %(port)d + 1))
    s.sendall(b"hello\nworld\n")
    f = s.makefile("rb")
    check("socket readline", f.readline() == b"hello\n")
    check("socket read", s.read(6) == b"world\n")
    s.settimeout(0.2)
    try:
        s.recv(10)
        check("socket timeout", False, "no timeout")
    except OSError as e:
        check("socket timeout", e.errno == 116, e)
    s.setblocking(False)
    check("non-blocking read", s.read(10) is None)
    p = select.poll()
    p.register(s, select.POLLIN)
    check("poll idle", p.poll(100) == [])
    s.write(b"x")
    r = p.poll(2000)
    check("poll ready", len(r) == 1 and r[0][1] & select.POLLIN, r)
    s.close()
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    u.settimeout(2)
    u.sendto(b"dgram", (H, %(port)d + 2))
    d, a = u.recvfrom(100)
    check("udp sendto/recvfrom", d == b"dgram" and a[0] == H, (d, a))
    u.close()
    ai = socket.getaddrinfo(H, 80)
    check("getaddrinfo", ai[0][-1] == (H, 80), ai)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", %(dev_tcp)d))
    srv.listen(1)
    t5.net.get(B + "/connect?port=%(dev_tcp)d")
    srv.settimeout(5)
    c, addr = srv.accept()
    check("socket accept", addr[0] == H and c.recv(100) == b"hello-from-mac", addr)
    c.close()
    srv.close()
run("socket", sockets)

import ssl
def tls():
    s = socket.socket()
    s.connect((H, %(tls_port)d))
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.load_verify_locations(cadata=CA)
    s = ctx.wrap_socket(s, server_hostname="tab5-test")
    s.write(b"secure")
    check("ssl with our CA", s.read(6) == b"secure")
    s.close()
    s = socket.socket()
    s.connect((H, %(tls_port)d))
    try:
        ssl.wrap_socket(s, server_hostname="tab5-test")       # bundle only: must refuse
        check("ssl refuses unknown CA", False, "accepted")
    except OSError:
        check("ssl refuses unknown CA", True)
    s.close()
    s = socket.socket()
    s.connect((H, %(tls_port)d))
    s = ssl.wrap_socket(s, cert_reqs=ssl.CERT_NONE)
    s.write(b"open")
    check("ssl CERT_NONE", s.read(4) == b"open")
    s.close()
run("ssl", tls)

import asyncio
def aio():
    async def client():
        r, w = await asyncio.open_connection(H, %(port)d + 1)
        w.write(b"async\n")
        await w.drain()
        line = await r.readline()
        w.close()
        await w.wait_closed()
        return line
    async def ticker(out):
        for i in range(5):
            out.append(i)
            await asyncio.sleep_ms(20)
    async def main():
        out = []
        line, _ = await asyncio.gather(client(), ticker(out))
        return line, out
    line, out = asyncio.run(main())
    check("asyncio open_connection", line == b"async\n", line)
    check("asyncio tasks", out == [0, 1, 2, 3, 4], out)
    got = []
    async def handle(r, w):
        got.append(await r.read(100))
        w.close()
    async def serve():
        srv = await asyncio.start_server(handle, "0.0.0.0", %(dev_async)d)
        t5.net.get(B + "/connect?port=%(dev_async)d")
        for _ in range(100):
            if got: break
            await asyncio.sleep_ms(50)
        srv.close()
        await srv.wait_closed()
    asyncio.run(serve())
    check("asyncio start_server", got == [b"hello-from-mac"], got)
run("asyncio", aio)

import requests, urequests
def req():
    r = requests.get(B + "/echo", params={"q": "a b"})
    check("requests get", r.status_code == 200 and r.ok and r.json()["method"] == "GET", r.status_code)
    r = requests.post(B + "/echo", json={"k": 1}, headers={"X-Test": "r"}, timeout=5)
    j = r.json()
    check("requests post json", json.loads(j["body"]) == {"k": 1} and j["xtest"] == "r", j)
    r = requests.post(B + "/echo", data={"a": "1", "b": "x y"})
    check("requests form", r.json()["body"] == "a=1&b=x%%20y", r.text)
    r = urequests.get(B + "/hello")
    check("urequests", r.text == "hello")
    r.close()
    try:
        requests.get(B + "/missing").raise_for_status()
        check("raise_for_status", False)
    except requests.HTTPError:
        check("raise_for_status", True)
    r = requests.get(B + "/big", stream=True)
    n = sum(len(c) for c in r.iter_content(8192))
    check("requests stream", n == 200000, n)
run("requests", req)

import aiohttp
def aioh():
    async def main():
        async with aiohttp.ClientSession() as s:
            async with s.get(B + "/hello") as resp:
                body = await resp.text()
                st = resp.status
            async with s.ws_connect("ws://%%s:%%d/" %% (H, %(port)d + 3)) as ws:
                await ws.send_str("aio-ws")
                msg = await ws.receive_str()
        return st, body, msg
    st, body, msg = asyncio.run(main())
    check("aiohttp get", st == 200 and body == "hello", (st, body))
    check("aiohttp websocket", msg == "aio-ws", msg)
run("aiohttp", aioh)

from umqtt.simple import MQTTClient
def umqtt():
    got = []
    c = MQTTClient("tab5-umqtt", H, port=%(mqtt)d, keepalive=30)
    c.set_callback(lambda t, m: got.append((t, m)))
    c.connect()
    c.subscribe(b"t5/test")
    c.publish(b"t5/test", b"from umqtt")
    for _ in range(20):
        c.check_msg()
        if got: break
        time.sleep_ms(100)
    c.disconnect()
    check("umqtt pub/sub", got == [(b"t5/test", b"from umqtt")], got)
run("umqtt", umqtt)

def mqtt():
    m = t5.net.mqtt("mqtt://%%s:%%d" %% (H, %(mqtt)d), client_id="tab5-esp")
    check("mqtt connected", m.connected)
    m.subscribe("t5/esp", 1)
    m.publish("t5/esp", b"x" * 3000, qos=1)
    kinds, payload = [], None
    for _ in range(10):
        e = m.recv(2000)
        if e is None: break
        kinds.append(e.type)
        if e.type == "message":
            payload = e.payload
            break
    check("mqtt message", payload == b"x" * 3000, (kinds, payload and len(payload)))
    check("mqtt acks", "subscribed" in kinds, kinds)
    m.close()
run("mqtt", mqtt)

def servers():
    srv = t5.net.serve(%(dev_http)d)
    check("serve port", srv.port == %(dev_http)d)
    t5.net.get(B + "/httpget?port=%(dev_http)d&tls=0")
    req = srv.next(5000)
    check("serve request", req is not None and req.method == "POST" and req.path == "/api" and req.query == "x=1"
          and req.body == b"ping" and req.headers.get("x-mac") == "1", req and (req.method, req.path, req.body))
    if req: req.json({"pong": True}, 201)
    r = result("httpget")
    check("serve response", r == {"status": 201, "body": '{"pong": true}'}, r)
    t5.net.get(B + "/wsclient?port=%(dev_http)d")
    req = srv.next(5000)
    if req:
        ws = req.websocket()
        msg = ws.recv(3000)
        ws.send(msg.upper())
        ws.close()
        check("ws server", msg == "hello" and result("wsclient") == "HELLO", msg)
    srv.close()
    srvs = t5.net.serve(%(dev_https)d, cert=CERT, key=KEY)
    t5.net.get(B + "/httpget?port=%(dev_https)d&tls=1")
    req = srvs.next(8000)
    if req:
        req.begin(200, "text/plain")
        req.write(b"stre")
        req.write("amed")
        req.end()
    r = result("httpget", 8000)
    check("https serve, streamed", r == {"status": 200, "body": "streamed"}, r)
    srvs.close()
run("servers", servers)

import network, deflate, io
def misc():
    w = network.WLAN(network.STA_IF)
    check("network isconnected", w.isconnected() and w.active())
    check("network ifconfig", w.ifconfig()[0] == t5.net.status()["ip"], w.ifconfig())
    w.connect("x", "y")                                    # connected already: nothing happens
    gz = t5.net.get(B + "/gzip").content
    check("deflate gzip", deflate.DeflateIO(io.BytesIO(gz), deflate.GZIP).read() == b"compressed " * 50)
run("misc", misc)

if %(external)s:
    def external():
        s = socket.socket()
        s.connect(socket.getaddrinfo("example.com", 443)[0][-1])
        s = ssl.wrap_socket(s, server_hostname="example.com")
        s.write(b"HEAD / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n")
        check("ssl to example.com", s.read(8) == b"HTTP/1.1")
        s.close()
        r = requests.get("https://httpbin.org/gzip")
        check("https gzip json", r.status_code == 200, r.status_code)
        m = t5.net.mqtt("mqtts://broker.emqx.io:8883", client_id="tab5-ext-%%d" %% time.ticks_ms())
        m.subscribe("tab5os/test", 1)
        time.sleep_ms(500)
        m.publish("tab5os/test", "hi", 1)
        msg = None
        for _ in range(10):
            e = m.recv(3000)
            if e and e.type == "message":
                msg = e.payload
                break
        m.close()
        check("mqtts public broker", msg == b"hi", msg)
    run("external", external)
''' + hn.SCRIPT_TAIL


class Http(hn.Http):
    def _route(self):
        p = self.path
        if p.startswith("/echo?"):
            self.path = "/echo"
            return super()._route()
        if p.startswith("/result"):
            name = p.split("name=", 1)[1]
            return self._send(200, json.dumps(RESULTS.pop(name, None)).encode(), "application/json")
        if p == "/gzip":
            import gzip
            return self._send(200, gzip.compress(b"compressed " * 50), "application/gzip")
        m = re.match(r"/httpget\?port=(\d+)&tls=(\d)", p)
        if m:
            threading.Thread(target=http_client, args=(self.client_address[0], int(m.group(1)), m.group(2) == "1"),
                             daemon=True).start()
            return self._send(200, b"ok")
        m = re.match(r"/wsclient\?port=(\d+)", p)
        if m:
            threading.Thread(target=ws_client, args=(self.client_address[0], int(m.group(1))), daemon=True).start()
            return self._send(200, b"ok")
        return super()._route()

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = _route


def http_client(ip, port, tls):
    if tls:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        rq, kw = f"https://{ip}:{port}/stream", {"context": ctx}
    else:
        rq, kw = urllib.request.Request(f"http://{ip}:{port}/api?x=1", data=b"ping", headers={"X-Mac": "1"}), {}
    end = time.time() + 5
    while True:
        try:
            r = urllib.request.urlopen(rq, timeout=10, **kw)
            RESULTS["httpget"] = {"status": r.status, "body": r.read().decode()}
            return
        except urllib.error.HTTPError as e:
            RESULTS["httpget"] = {"status": e.code, "body": e.read().decode()}
            return
        except urllib.error.URLError as e:
            if isinstance(e.reason, ConnectionRefusedError) and time.time() < end:
                time.sleep(0.1)         # the Tab5's server isn't listening yet
                continue
            RESULTS["httpget"] = {"error": repr(e)}
            return
        except OSError as e:
            RESULTS["httpget"] = {"error": repr(e)}
            return


def ws_client(ip, port):
    try:
        s = hn.connect_retry(ip, port, timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall((f"GET /ws HTTP/1.1\r\nHost: {ip}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            head += s.recv(1024)
        rest = head.split(b"\r\n\r\n", 1)[1]
        mask = os.urandom(4)
        s.sendall(bytes([0x81, 0x80 | 5]) + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(b"hello")))
        data = rest
        while len(data) < 2:
            data += s.recv(1024)
        n = data[1] & 0x7f
        while len(data) < 2 + n:
            data += s.recv(1024)
        RESULTS["wsclient"] = data[2 : 2 + n].decode()
        s.close()
    except (OSError, UnicodeDecodeError) as e:
        RESULTS["wsclient"] = repr(e)


class Broker(socketserver.BaseRequestHandler):
    """Just enough MQTT 3.1.1: CONNECT, SUBSCRIBE, PUBLISH QoS 0/1, PING, DISCONNECT."""
    subs = {}               # topic -> set of handlers
    lock = threading.Lock()

    def send(self, b):
        with self.wlock:
            self.request.sendall(b)

    def read_packet(self):
        h = self.request.recv(1)
        if not h:
            raise ConnectionError
        n, mult = 0, 1
        while True:
            b = self.request.recv(1)[0]
            n += (b & 0x7f) * mult
            mult *= 128
            if not b & 0x80:
                break
        body = b""
        while len(body) < n:
            d = self.request.recv(n - len(body))
            if not d:
                raise ConnectionError
            body += d
        return h[0], body

    @staticmethod
    def pkt(t, body):
        n, enc = len(body), b""
        while True:
            b = n % 128
            n //= 128
            enc += bytes([b | (0x80 if n else 0)])
            if not n:
                break
        return bytes([t]) + enc + body

    def handle(self):
        self.wlock = threading.Lock()
        mine = []
        try:
            while True:
                t, body = self.read_packet()
                kind = t >> 4
                if kind == 1:
                    self.send(b"\x20\x02\x00\x00")
                elif kind == 8:
                    pid, i, granted = body[:2], 2, b""
                    while i < len(body):
                        tl = struct.unpack(">H", body[i : i + 2])[0]
                        topic = body[i + 2 : i + 2 + tl].decode()
                        granted += bytes([min(body[i + 2 + tl], 1)])
                        i += 3 + tl
                        with Broker.lock:
                            Broker.subs.setdefault(topic, set()).add(self)
                        mine.append(topic)
                    self.send(self.pkt(0x90, pid + granted))
                elif kind == 3:
                    qos = (t >> 1) & 3
                    tl = struct.unpack(">H", body[:2])[0]
                    topic = body[2 : 2 + tl].decode()
                    off = 2 + tl
                    if qos:
                        self.send(self.pkt(0x40, body[off : off + 2]))
                        off += 2
                    out = self.pkt(0x30, body[: 2 + tl] + body[off:])          # deliver at QoS 0
                    with Broker.lock:
                        targets = list(Broker.subs.get(topic, ()))
                    for h in targets:
                        h.send(out)
                elif kind == 12:
                    self.send(b"\xd0\x00")
                elif kind == 14:
                    return
        except (ConnectionError, OSError, IndexError):
            pass
        finally:
            with Broker.lock:
                for topic in mine:
                    Broker.subs.get(topic, set()).discard(self)


class TlsEcho(socketserver.BaseRequestHandler):
    ctx = None

    def handle(self):
        try:
            with self.ctx.wrap_socket(self.request, server_side=True) as s:
                while True:
                    d = s.recv(4096)
                    if not d:
                        return
                    s.sendall(d)
        except (ssl.SSLError, OSError):
            return


def make_certs(d, host):
    """A CA, a server cert from it for this machine, and a self-signed one for the Tab5."""
    def run(*a):
        subprocess.run(["openssl", *a], cwd=d, check=True, capture_output=True)
    run("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-keyout", "ca.key",
        "-out", "ca.pem", "-days", "2", "-subj", "/CN=tab5 test CA")
    run("req", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-keyout", "srv.key",
        "-out", "srv.csr", "-subj", "/CN=tab5-test")
    with open(os.path.join(d, "ext.cnf"), "w") as f:
        f.write(f"subjectAltName=DNS:tab5-test,IP:{host}\n")
    run("x509", "-req", "-in", "srv.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", "srv.pem",
        "-days", "2", "-extfile", "ext.cnf")
    run("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-keyout", "dev.key",
        "-out", "dev.pem", "-days", "2", "-subj", "/CN=tab5")
    return {n: open(os.path.join(d, n)).read() for n in ("ca.pem", "srv.pem", "srv.key", "dev.pem", "dev.key")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--external", action="store_true")
    ap.add_argument("--host", help="this machine's address as the Tab5 sees it")
    a = ap.parse_args()
    with connect(cmd_timeout=40) as t:
        if not hn.hold_network(t):
            print("the Tab5 isn't on Wi-Fi")
            return 1
        host = a.host or hn.my_ip(hn.tab5_ip(t))
        tmp = tempfile.mkdtemp()
        certs = make_certs(tmp, host)
        TlsEcho.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        TlsEcho.ctx.load_cert_chain(os.path.join(tmp, "srv.pem"), os.path.join(tmp, "srv.key"))
        Http.script = (DEVICE_SCRIPT % {
            "host": host, "port": BASE, "mqtt": MQTT_PORT, "tls_port": TLS_PORT, "dev_tcp": DEV_TCP,
            "dev_http": DEV_HTTP, "dev_https": DEV_HTTPS, "dev_async": DEV_ASYNC, "ca": certs["ca.pem"],
            "dev_cert": certs["dev.pem"], "dev_key": certs["dev.key"], "external": "True" if a.external else "False",
        }).encode()
        servers = hn.serve([hn.HServer((host, BASE), Http), hn.TServer((host, BASE + 1), hn.Echo),
                            socketserver.ThreadingUDPServer((host, BASE + 2), hn.UdpEcho),
                            hn.TServer((host, BASE + 3), hn.WsEcho),
                            hn.TServer((host, MQTT_PORT), Broker), hn.TServer((host, TLS_PORT), TlsEcho)])
        try:
            t0 = time.time()
            out = hn.run_script(t, host, "netcompat")
            if out is None:
                print("couldn't fetch the test script")
                return 1
            print("\n".join(l for l in out.splitlines() if not re.match(r"^[IW] \(\d+\)", l)))
            print(f"({time.time() - t0:.1f} s)")
            t.cmd("power unlock net")
            left = hn.handles_left(t)
            print("handles after:", left)
        finally:
            hn.stop(servers)
    ok = hn.passed(out) and left.startswith("0 ")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
