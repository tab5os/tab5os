# SPDX-License-Identifier: Apache-2.0
# The standard network module (MicroPython API), as far as an app can use
# it on Tab5 OS: Wi-Fi is joined and switched in Settings, so an app can
# read the connection (isconnected, ifconfig, status, config) but not
# change it. connect() on an already connected Tab5 does nothing, which is
# what the usual "if not wlan.isconnected(): wlan.connect(...)" code needs.
import t5

STA_IF = 0
AP_IF = 1
STAT_IDLE = 1000
STAT_CONNECTING = 1001
STAT_GOT_IP = 1010
STAT_NO_AP_FOUND = 201
STAT_WRONG_PASSWORD = 202
STAT_CONNECT_FAIL = 203


class WLAN:
    def __init__(self, interface=STA_IF):
        if interface != STA_IF:
            raise OSError("Tab5 OS has no access point mode for apps")
        self._if = interface

    def active(self, is_active=None):
        on = t5.net.status()["state"] not in ("off", "failed")
        if is_active is not None and bool(is_active) != on:
            raise OSError("turn Wi-Fi on or off in Settings")
        return on

    def isconnected(self):
        return t5.net.connected()

    def connect(self, ssid=None, key=None, *args, **kw):
        if t5.net.connected():
            return
        raise OSError("not on Wi-Fi: join a network in Settings")

    def disconnect(self):
        pass

    def status(self, param=None):
        s = t5.net.status()
        if param == "rssi":
            return s["rssi"]
        if param is not None:
            raise ValueError("unknown status parameter")
        if s["connected"]:
            return STAT_GOT_IP
        return STAT_CONNECTING if s["state"] in ("connecting", "starting") else STAT_IDLE

    def ifconfig(self, cfg=None):
        if cfg is not None:
            raise OSError("addresses come from DHCP")
        s = t5.net.status()
        return (s["ip"], s["mask"], s["gw"], s["dns"])

    def ipconfig(self, *args, **kw):
        s = t5.net.status()
        if args and args[0] == "addr4":
            return (s["ip"], s["mask"])
        if args and args[0] == "gw4":
            return s["gw"]
        if args and args[0] == "dhcp4":
            return True
        raise ValueError("unknown parameter")

    def config(self, *args, **kw):
        if kw:
            raise OSError("Wi-Fi settings belong to Settings")
        s = t5.net.status()
        if args and args[0] in ("ssid", "essid"):
            return s["ssid"]
        if args and args[0] == "channel":
            return s.get("channel", 0)
        if args and args[0] == "hostname":
            return "tab5"
        raise ValueError("unknown parameter")

    def scan(self):
        raise OSError("scanning belongs to Settings")


def hostname(name=None):
    if name is not None:
        raise OSError("the hostname can't be changed by apps")
    return "tab5"


def country(code=None):
    return ""
