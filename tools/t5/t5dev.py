#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The app developer half of the t5 CLI (plan 8.6).

  t5 new NAME [--python] [--id ID] [--dir DIR]   an app from the C or Python template
  t5 app-build [DIR]                             build (C) and pack into DIR/build/<id>.t5app
  t5 pack [DIR] [-o OUT] [--sign KEY]            pack only (signed or unsigned)
  t5 keygen KEY.pem                              an ECDSA P-256 signing key
  t5 run [DIR] [--host H] [--usb] [--no-logs]    build, push, launch and stream the log
  t5 pair HOST                                   pair with a Tab5 in developer mode
  t5 devices                                     paired Tab5s
  t5 push FILE.t5app | t5 launch ID | t5 stop | t5 rm ID | t5 ls
  t5 logs                                        stream the device log
  t5 py FILE.py | -c CODE                        run Python on the device, print its output
  t5 screenshot OUT.png                          over Wi-Fi (`t5 shot` is the USB one)
  t5 ota-pack [--sign KEY] [-o OUT.t5os]         an OS update from the firmware build (plan 9.1)
  t5 ota [FILE.t5os] [--no-restart]              push an OS update to the paired Tab5

Wi-Fi first: commands talk to the developer-mode endpoint of the paired
Tab5 (`t5 pair` stores a token in ~/.config/t5/devices.json); --host picks
another one. `t5 run` falls back to the USB-C console when the Tab5 isn't
reachable over Wi-Fi (or with --usb): the package goes across with `app put`
(t5link.py), then `app install` and `app run`, and the console output
streams.

Stdlib only, plus pyserial for the USB path (the ESP-IDF Python has it).
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import tomllib
import urllib.error
import urllib.parse
import urllib.request

import t5const
import t5img
import t5link
import t5pack

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ota"))
import sign  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SDK = os.path.join(ROOT, "sdk")
CONFIG = os.path.join(os.path.expanduser("~"), ".config", "t5", "devices.json")
DEV_PORT = t5const.define("t5_dev/include/t5_dev/dev.h", "T5_DEV_PORT")


class CliError(Exception):
    pass


def load_config():
    try:
        with open(CONFIG) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"default": None, "devices": {}}


def save_config(cfg):
    os.makedirs(os.path.dirname(CONFIG), exist_ok=True)
    tmp = CONFIG + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f, indent=2)
    os.chmod(tmp, 0o600)
    os.replace(tmp, CONFIG)


def target(host=None):
    cfg = load_config()
    host = host or os.environ.get("T5_HOST") or cfg.get("default")
    if not host:
        raise CliError("no paired Tab5: turn on Settings > Developer mode, then run `t5 pair <address>`")
    dev = cfg["devices"].get(host)
    return host, dev["token"] if dev else None


def api(host, path, method="GET", body=None, token=None, timeout=20, headers=None, raw=False):
    url = f"http://{host}:{DEV_PORT}/api/{path}"
    h = dict(headers or {})
    if token:
        h["Authorization"] = "Bearer " + token
    if body is not None and "Content-Type" not in h:
        h["Content-Type"] = "application/octet-stream"
    req = urllib.request.Request(url, data=body, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = r.read()
            if raw:
                return data, r.headers
            return json.loads(data) if data.startswith((b"{", b"[")) else data.decode(errors="replace")
    except urllib.error.HTTPError as e:
        msg = e.read().decode(errors="replace")
        try:
            msg = json.loads(msg).get("error", msg)
        except ValueError:
            pass
        raise CliError(f"{e.code}: {msg}") from None
    except (urllib.error.URLError, socket.timeout, ConnectionError, OSError) as e:
        raise CliError(f"can't reach {host}:{DEV_PORT} ({getattr(e, 'reason', e)}). Is developer mode on and the "
                       "Tab5 on this network?") from None


def reachable(host):
    try:
        api(host, "info", timeout=3)
        return True
    except CliError:
        return False


def read_manifest(app_dir):
    path = os.path.join(app_dir, "manifest.toml")
    if not os.path.isfile(path):
        raise CliError(f"{app_dir}: no manifest.toml (run `t5 new` to make an app)")
    with open(path, "rb") as f:
        return tomllib.load(f)


def firmware_build():
    return os.path.join(ROOT, "firmware", "build")


def build(app_dir, sign=None):
    """Build (native) or stage (Python) and pack; returns the .t5app path."""
    m = read_manifest(app_dir)
    out_dir = os.path.join(app_dir, "build")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, f"{m['id']}.t5app")
    if m.get("runtime") == "native":
        fw = firmware_build()
        if not os.path.isfile(os.path.join(fw, "compile_commands.json")):
            raise CliError(f"no firmware build at {fw}: run `t5 build` in the repo first (apps compile against it)")
        if not shutil.which("riscv32-esp-elf-gcc"):
            raise CliError("the RISC-V compiler isn't on PATH: run `. tools/env.sh`")
        cmake = ["cmake", "-S", app_dir, "-B", out_dir, "-G", "Ninja", f"-DT5_FIRMWARE_BUILD={fw}", f"-DT5_SDK={SDK}"]
        if subprocess.run(cmake, stdout=subprocess.DEVNULL).returncode:
            raise CliError("cmake configure failed")
        if subprocess.run(["cmake", "--build", out_dir]).returncode:
            raise CliError("build failed")
        built = [os.path.join(out_dir, f) for f in os.listdir(out_dir) if f.endswith(".t5app")]
        if not built:
            raise CliError("the build made no .t5app")
        src = max(built, key=os.path.getmtime)
        if sign:
            # Re-pack the build's staging dir with a signature.
            stage = src[:-len(".t5app")] + ".pkg"
            try:
                t5pack.pack(stage, out, sign)
            except t5pack.PackError as e:
                raise CliError(str(e)) from None
        elif src != out:
            shutil.copyfile(src, out)
        return out
    try:
        t5pack.pack(app_dir, out, sign)
    except t5pack.PackError as e:
        raise CliError(str(e)) from None
    return out


PLACEHOLDER = re.compile(r"@([A-Z0-9_]+)@")


def render_template(tmpl, dest, values):
    """Copy the template folder tmpl to dest, replacing @KEY@ with values[KEY]
    in every file. An unknown placeholder is an error (dest is removed)."""
    shutil.copytree(tmpl, dest)
    try:
        for fn in sorted(os.listdir(dest)):
            p = os.path.join(dest, fn)
            with open(p, encoding="utf-8") as f:
                text = f.read()
            unknown = sorted(set(PLACEHOLDER.findall(text)) - set(values))
            if unknown:
                raise CliError(f"{os.path.join(tmpl, fn)}: unknown placeholder(s) {', '.join(unknown)}")
            with open(p, "w", encoding="utf-8") as f:
                f.write(PLACEHOLDER.sub(lambda m: values[m.group(1)], text))
    except BaseException:
        shutil.rmtree(dest, ignore_errors=True)
        raise


def cmd_new(a):
    name = a.name
    if '"' in name or "\\" in name:
        raise CliError("the name can't contain quotes or backslashes")
    slug = re.sub(r"[^a-z0-9_]", "_", name.lower()).strip("_") or "app"
    app_id = a.id or f"com.example.{slug}"
    if not t5pack.ID_RE.match(app_id):
        raise CliError(f"bad id '{app_id}': reverse DNS, lower case, like com.you.{slug}")
    dest = os.path.abspath(a.dir or slug)
    if os.path.exists(dest):
        raise CliError(f"{dest} exists")
    tmpl = os.path.join(SDK, "templates", "python" if a.python else "c")
    render_template(tmpl, dest, {"APP_ID": app_id, "APP_NAME": name, "APP_SLUG": slug, "T5_SDK": SDK})
    print(f"{dest}: {'Python' if a.python else 'C'} app {app_id}")
    print(f"next: cd {os.path.relpath(dest)} && t5 run")


def cmd_build(a):
    print(build(os.path.abspath(a.dir or "."), a.sign))


def cmd_pack(a):
    try:
        t5pack.pack(os.path.abspath(a.dir or "."), a.output, a.sign)
    except t5pack.PackError as e:
        raise CliError(str(e)) from None


def cmd_keygen(a):
    if os.path.exists(a.key):
        raise CliError(f"{a.key} exists")
    try:
        sign.generate(a.key)
        kid = sign.key_id(sign.public_der(a.key))
    except sign.SignError as e:
        raise CliError(str(e)) from None
    print(f"{a.key}: ECDSA P-256, key {kid.hex()}")
    print("Keep it private. Sign packages with `t5 pack --sign`, OS updates with `t5 ota-pack --sign`.")


def cmd_pair(a):
    host = a.host
    info = api(host, "info", timeout=5)
    client = a.name or socket.gethostname().split(".")[0]
    r = api(host, "pair?" + urllib.parse.urlencode({"client": client}), "POST", b"", timeout=10)
    print(f"{info.get('model')} running {info.get('os')} {info.get('version')} at {host}")
    code = input("Type the six-digit code shown on the Tab5: ").strip()
    t = api(host, "pair/confirm?" + urllib.parse.urlencode({"request": r["request"], "code": code}), "POST", b"")
    cfg = load_config()
    cfg["devices"][host] = {"token": t["token"], "paired": int(time.time()), "version": info.get("version")}
    cfg["default"] = host
    save_config(cfg)
    print(f"Paired. {host} is now the default target ({CONFIG}).")


def cmd_devices(a):
    cfg = load_config()
    if not cfg["devices"]:
        print("no paired Tab5")
    for host, d in cfg["devices"].items():
        mark = "*" if host == cfg.get("default") else " "
        state = "reachable" if reachable(host) else "not reachable"
        print(f"{mark} {host}  paired {time.strftime('%Y-%m-%d', time.localtime(d.get('paired', 0)))}  {state}")


def install(host, token, pkg):
    with open(pkg, "rb") as f:
        return api(host, "install", "POST", f.read(), token, timeout=120)


def stream_logs(host, token, pos=None, match=None, until=None):
    """Print new log lines until Ctrl-C (or until(line) is true)."""
    if pos is None:
        _, h = api(host, "logs?wait=0", token=token, raw=True)
        pos = int(h.get("X-Log-Pos", "0"))
    partial = ""
    try:
        while True:
            try:
                data, h = api(host, f"logs?pos={pos}&wait=4000", token=token, raw=True, timeout=15)
            except CliError as e:
                print(f"[log stream: {e}; retrying]", file=sys.stderr)
                time.sleep(2)
                continue
            pos = int(h.get("X-Log-Pos", pos))
            text = partial + data.decode(errors="replace")
            lines = text.split("\n")
            partial = lines.pop()
            for line in lines:
                line = re.sub(r"\x1b\[[0-9;]*m", "", line)
                if match is None or match in line:
                    print(line, flush=True)
                if until and until(line):
                    return pos
    except KeyboardInterrupt:
        print()
    return pos


def run_wifi(host, token, pkg, m, logs):
    t0 = time.time()
    _, h = api(host, "logs?wait=0", token=token, raw=True)
    pos = int(h.get("X-Log-Pos", "0"))
    r = install(host, token, pkg)
    print(f"installed {r.get('id')} {r.get('version')} ({time.time() - t0:.1f} s)")
    api(host, "launch?" + urllib.parse.urlencode({"id": m["id"]}), "POST", b"", token)
    print(f"launched {m['id']} ({time.time() - t0:.1f} s); log follows, Ctrl-C to stop watching")
    if logs:
        stream_logs(host, token, pos)


def run_usb(port, pkg, m, logs):
    with open(pkg, "rb") as f:
        data = f.read()
    t0 = time.time()
    try:
        t = t5link.Tab5(port).open()
    except (RuntimeError, *t5link.SERIAL_ERRORS) as e:
        raise CliError(str(e)) from None
    with t:
        try:
            t.install(data, m["id"])
        except (RuntimeError, ValueError) as e:
            raise CliError(str(e)) from None
        print(f"sent and installed {len(data)} bytes over USB ({time.time() - t0:.1f} s)")
        t.cmd(f"app run {m['id']}", timeout=20)
        print(f"launched {m['id']} ({time.time() - t0:.1f} s); console follows, Ctrl-C to stop watching")
        if logs:
            try:
                while True:
                    d = t.ser.read(1024)
                    if d:
                        sys.stdout.write(d.decode(errors="replace"))
                        sys.stdout.flush()
            except KeyboardInterrupt:
                print()


def cmd_run(a):
    app_dir = os.path.abspath(a.dir or ".")
    m = read_manifest(app_dir)
    pkg = build(app_dir, a.sign)
    if not a.usb:
        try:
            host, token = target(a.host)
        except CliError:
            host, token = None, None
        if host and token and reachable(host):
            run_wifi(host, token, pkg, m, not a.no_logs)
            return
        print("Wi-Fi: no paired Tab5 reachable; using the USB-C console", file=sys.stderr)
    run_usb(a.port, pkg, m, not a.no_logs)


def need_target(a):
    host, token = target(a.host)
    if not token:
        raise CliError(f"{host} isn't paired: run `t5 pair {host}`")
    return host, token


def cmd_push(a):
    host, token = need_target(a)
    print(install(host, token, a.file))


def cmd_launch(a):
    host, token = need_target(a)
    api(host, "launch?" + urllib.parse.urlencode({"id": a.id}), "POST", b"", token)


def cmd_stop(a):
    host, token = need_target(a)
    api(host, "exit", "POST", b"", token)


def cmd_rm(a):
    host, token = need_target(a)
    api(host, "remove?" + urllib.parse.urlencode({"id": a.id, "keep": "1" if a.keep_data else "0"}), "POST", b"",
        token)


def cmd_ls(a):
    host, token = need_target(a)
    for x in api(host, "apps", token=token):
        flags = ("built-in " if x["builtin"] else "") + ("running " if x["running"] else "")
        print(f"{x['id']:<36} {x['version']:<10} {flags}{x['name']}")


def cmd_logs(a):
    host, token = need_target(a)
    stream_logs(host, token, match=a.grep)


def cmd_py(a):
    host, token = need_target(a)
    if a.code is not None:
        src = a.code
    else:
        with open(a.file, encoding="utf-8") as f:
            src = f.read()
    out, h = api(host, "py", "POST", src.encode(), token, timeout=60, raw=True)
    sys.stdout.write(out.decode(errors="replace"))
    state = h.get("X-Py-State", "?")
    if state != "done":
        print(f"[{state}]", file=sys.stderr)
        sys.exit(1)


def cmd_screenshot(a):
    host, token = need_target(a)
    data, h = api(host, f"screenshot?div={a.div}", token=token, raw=True, timeout=30)
    try:
        ow, oh = t5img.save(a.out, data, int(h["X-Width"]), int(h["X-Height"]), int(h["X-Rotation"]))
    except ValueError as e:
        raise CliError(f"bad screenshot: {e}") from None
    print(f"{a.out}: {ow}x{oh}")


def paced_post(host, path, data, token, chunk=16384, pause=0.02):
    """POST a large body in paced writes. The Tab5's Wi-Fi link (SDIO to the
    C6) drops packets when a sender floods it, so big uploads go at a few
    hundred KB/s."""
    import http.client
    c = http.client.HTTPConnection(host, DEV_PORT, timeout=120)
    try:
        c.putrequest("POST", "/api/" + path)
        c.putheader("Authorization", "Bearer " + token)
        c.putheader("Content-Type", "application/octet-stream")
        c.putheader("Content-Length", str(len(data)))
        c.endheaders()
        for i in range(0, len(data), chunk):
            c.send(data[i:i + chunk])
            if i and i % (512 * 1024) < chunk:
                print(f"  {i * 100 // len(data)}%", flush=True)
            time.sleep(pause)
        r = c.getresponse()
        body = r.read()
    except (OSError, http.client.HTTPException) as e:
        raise CliError(f"upload to {host} failed: {e}") from None
    finally:
        c.close()
    try:
        out = json.loads(body)
    except ValueError:
        raise CliError(f"{r.status}: {body[:200]!r}") from None
    if r.status != 200:
        raise CliError(f"{r.status}: {out.get('error', out)}")
    return out


def cmd_ota_pack(a):
    image = os.path.join(firmware_build(), "tab5os.bin")
    if not os.path.isfile(image):
        raise CliError(f"no {image}: run `t5 build`")
    args = [sys.executable, os.path.join(ROOT, "tools", "ota", "t5ota.py"), "pack", image]
    if a.output:
        args += ["-o", a.output]
    if a.sign:
        args += ["--sign", a.sign]
    if subprocess.run(args).returncode:
        raise CliError("packing failed")


def cmd_ota(a):
    host, token = need_target(a)
    path = a.file
    if not path:
        cands = sorted((f for f in os.listdir(".") if f.endswith(".t5os")), key=os.path.getmtime)
        if not cands:
            raise CliError("no .t5os here: run `t5 ota-pack` or name the file")
        path = cands[-1]
    with open(path, "rb") as f:
        data = f.read()
    t0 = time.time()
    print(f"sending {path} ({len(data) // 1024} KB) to {host}...")
    r = paced_post(host, "ota" + ("" if a.no_restart else "?reboot=1"), data, token)
    print(f"installed Tab5 OS {r.get('version')} in {time.time() - t0:.0f} s" +
          ("; the Tab5 is restarting into it (on trial for its first minute)" if r.get("restarting") else
           "; restart the Tab5 to run it"))


def main(argv=None):
    # --host and --port go before or after the command. On the commands they
    # default to SUPPRESS, so they don't overwrite a value given before it.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--host", default=argparse.SUPPRESS,
                        help="Tab5 address (default: the last one paired, or $T5_HOST)")
    common.add_argument("--port", default=argparse.SUPPRESS, help="USB serial port for the fallback")
    ap = argparse.ArgumentParser(prog="t5", description=__doc__.split("\n")[0])
    ap.add_argument("--host", help="Tab5 address (default: the last one paired, or $T5_HOST)")
    ap.add_argument("--port", help="USB serial port for the fallback")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def command(name, fn, *args, parents=(common,), **kw):
        p = sub.add_parser(name, parents=list(parents), **kw)
        for flags, opts in args:
            p.add_argument(*flags, **opts)
        p.set_defaults(fn=fn)
        return p

    command("new", cmd_new, (["name"], {}), (["--python"], dict(action="store_true", help="a Python app (default C)")),
            (["--id"], {}), (["--dir"], {}), help="create an app")
    command("app-build", cmd_build, (["dir"], dict(nargs="?")), (["--sign"], dict(metavar="KEY")))
    command("run", cmd_run, (["dir"], dict(nargs="?")), (["--sign"], dict(metavar="KEY")),
            (["--usb"], dict(action="store_true", help="skip Wi-Fi")), (["--no-logs"], dict(action="store_true")))
    command("pack", cmd_pack, (["dir"], dict(nargs="?")), (["-o", "--output"], {}), (["--sign"], dict(metavar="KEY")))
    command("keygen", cmd_keygen, (["key"], {}))
    command("pair", cmd_pair, (["host"], {}), (["--name"], dict(help="how the Tab5 lists this computer")), parents=())
    command("devices", cmd_devices)
    command("push", cmd_push, (["file"], {}))
    command("launch", cmd_launch, (["id"], {}))
    command("stop", cmd_stop)
    command("rm", cmd_rm, (["id"], {}), (["--keep-data"], dict(action="store_true")))
    command("ls", cmd_ls)
    command("logs", cmd_logs, (["--grep"], {}))
    command("py", cmd_py, (["file"], dict(nargs="?")), (["-c"], dict(dest="code")))
    command("screenshot", cmd_screenshot, (["out"], dict(nargs="?", default="screenshot.png")),
            (["--div"], dict(type=int, default=2)))
    command("ota-pack", cmd_ota_pack, (["-o", "--output"], {}), (["--sign"], dict(metavar="KEY")))
    command("ota", cmd_ota, (["file"], dict(nargs="?")), (["--no-restart"], dict(action="store_true")))
    a = ap.parse_args(argv)
    if a.cmd == "py" and a.code is None and not a.file:
        ap.error("py needs a file or -c CODE")
    try:
        a.fn(a)
        return 0
    except CliError as e:
        print(f"t5: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
