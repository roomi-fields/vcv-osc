#!/usr/bin/env python3
"""Example OSC client for the vcv-osc "OSC Controller" module.

Demonstrates every feature: turn a knob, press a button, add/remove a cable,
and dump the patch state. Uses python-osc (`pip install python-osc`).

The module listens (by default) on UDP 7770 and sends replies to UDP 7771 —
matching the osc-bridge passthrough surface. When you drive it *through*
osc-bridge, prefix every address with `/vcv` (the bridge strips it); when you
talk to the module directly (as this script does), use the bare addresses.

Modules, params and ports may be given either by numeric id (as printed by
`dump`) or by *name* — a stable, human-readable reference (Phase 1):
    module : "VCO"  or  "Fundamental/VCO"  (add ":N" for the Nth instance)
    param  : the knob label, e.g. "Frequency"   (case-insensitive)
    port   : the port name,  e.g. "Sine"        (case-insensitive)
This client auto-detects: an argument that parses as an int is sent as an id,
otherwise it is sent as a name string.

Usage:
    python vcv_osc_client.py dump [includeParams=1] [includePorts=1]
    python vcv_osc_client.py registry                            # list installed modules
    python vcv_osc_client.py set <module> <param> <value>
    python vcv_osc_client.py get <module> <param>
    python vcv_osc_client.py press <module> <param>              # momentary 1 then 0
    python vcv_osc_client.py cable-add <outMod> <outPort> <inMod> <inPort>
    python vcv_osc_client.py cable-remove <inMod> <inPort>
    python vcv_osc_client.py module-add <pluginSlug> <modelSlug> [x] [y]
    python vcv_osc_client.py module-remove <module>
    python vcv_osc_client.py preset-save <module> <path>
    python vcv_osc_client.py preset-load <module> <path>
    python vcv_osc_client.py watch <module> <param> [0|1]
    python vcv_osc_client.py struct-watch [0|1]                  # stream module/cable add/remove
    python vcv_osc_client.py addr <module> <param> [value]       # /param/<m>/<p> RESTful form
    python vcv_osc_client.py oscquery [path]                     # fetch OSCQuery namespace (HTTP)
    python vcv_osc_client.py listen                              # just print replies
    python vcv_osc_client.py demo <knobModule> <knobParam>       # scripted showcase

Options:
    --host H   module host (default 127.0.0.1)
    --port P   module listen port (default 7770)
    --reply P  local port to listen for replies on (default 7771)
"""
import argparse
import sys
import time
import threading

from pythonosc.udp_client import SimpleUDPClient
from pythonosc.dispatcher import Dispatcher
from pythonosc.osc_server import BlockingOSCUDPServer


def make_reply_server(reply_port):
    """Start a background OSC server that prints every reply from the module."""
    disp = Dispatcher()

    def show(addr, *args):
        print(f"  <- {addr} {list(args)}")

    disp.set_default_handler(show)
    server = BlockingOSCUDPServer(("0.0.0.0", reply_port), disp)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    return server


def main():
    ap = argparse.ArgumentParser(description="vcv-osc example client")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7770)
    ap.add_argument("--reply", type=int, default=7771)
    ap.add_argument("--oscquery-port", type=int, default=7772)
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()

    client = SimpleUDPClient(a.host, a.port)
    # Listen for replies (dump output, /param/value, /cable/added, /error…).
    make_reply_server(a.reply)

    def send(addr, *args):
        print(f"  -> {addr} {list(args)}")
        client.send_message(addr, list(args))

    def ref(x):
        """A module/param/port reference: int id if numeric, else a name string."""
        try:
            return int(x)
        except ValueError:
            return x

    cmd, args = a.cmd, a.args

    if cmd == "dump":
        include_params = int(args[0]) if len(args) > 0 else 1
        include_ports = int(args[1]) if len(args) > 1 else 1
        send("/state/dump", include_params, include_ports)
        time.sleep(1.5)

    elif cmd == "registry":
        send("/registry/dump")
        time.sleep(1.5)

    elif cmd == "set":
        mod, param, val = ref(args[0]), ref(args[1]), float(args[2])
        send("/param/set", mod, param, val)
        time.sleep(0.3)

    elif cmd == "get":
        send("/param/get", ref(args[0]), ref(args[1]))
        time.sleep(0.3)

    elif cmd == "press":
        # Momentary button: value 1.0 then 0.0.
        mod, param = ref(args[0]), ref(args[1])
        send("/param/set", mod, param, 1.0)
        time.sleep(0.15)
        send("/param/set", mod, param, 0.0)
        time.sleep(0.2)

    elif cmd == "cable-add":
        om, op, im, ip = (ref(x) for x in args[:4])
        send("/cable/add", om, op, im, ip)
        time.sleep(0.3)

    elif cmd == "cable-remove":
        im, ip = ref(args[0]), ref(args[1])
        send("/cable/remove", im, ip)
        time.sleep(0.3)

    elif cmd == "module-add":
        plugin_slug, model_slug = args[0], args[1]
        rest = [float(x) for x in args[2:4]]  # optional x y
        send("/module/add", plugin_slug, model_slug, *rest)
        time.sleep(0.3)

    elif cmd == "module-remove":
        send("/module/remove", ref(args[0]))
        time.sleep(0.3)

    elif cmd == "preset-save":
        send("/module/preset_save", ref(args[0]), args[1])
        time.sleep(0.3)

    elif cmd == "preset-load":
        send("/module/preset_load", ref(args[0]), args[1])
        time.sleep(0.3)

    elif cmd == "watch":
        mod, param = ref(args[0]), ref(args[1])
        on = int(args[2]) if len(args) > 2 else 1
        send("/param/watch", mod, param, on)
        print("  (watching — turn the knob in Rack; Ctrl-C to stop)")
        while True:
            time.sleep(0.5)

    elif cmd == "struct-watch":
        on = int(args[0]) if len(args) > 0 else 1
        send("/state/watch", on)
        print("  (watching topology — add/remove modules & cables in Rack; Ctrl-C to stop)")
        while True:
            time.sleep(0.5)

    elif cmd == "addr":
        # RESTful per-address form: /param/<moduleId>/<paramId> [value].
        mod, param = int(args[0]), int(args[1])
        rest = [float(args[2])] if len(args) > 2 else []
        send(f"/param/{mod}/{param}", *rest)
        time.sleep(0.3)

    elif cmd == "oscquery":
        # OSCQuery discovery is plain HTTP/JSON — no python-osc needed.
        import json
        import urllib.request
        path = args[0] if args else "/"
        url = f"http://{a.host}:{a.oscquery_port}{path}"
        print(f"  GET {url}")
        with urllib.request.urlopen(url, timeout=3) as r:
            print(json.dumps(json.loads(r.read().decode()), indent=2))
        print(f"  GET http://{a.host}:{a.oscquery_port}/?HOST_INFO")
        with urllib.request.urlopen(f"http://{a.host}:{a.oscquery_port}/?HOST_INFO", timeout=3) as r:
            print(json.dumps(json.loads(r.read().decode()), indent=2))
        return

    elif cmd == "listen":
        send("/ping")
        print("  (listening for replies — Ctrl-C to stop)")
        while True:
            time.sleep(0.5)

    elif cmd == "demo":
        # Scripted showcase: sweep a knob, ping, dump.
        mod, param = int(args[0]), int(args[1])
        print("== ping ==");        send("/ping"); time.sleep(0.3)
        print("== sweep knob ==")
        for i in range(11):
            send("/param/set", mod, param, i / 10.0)
            time.sleep(0.1)
        send("/param/get", mod, param); time.sleep(0.3)
        print("== dump ==");         send("/state/dump", 0); time.sleep(1.0)

    else:
        print(f"unknown command: {cmd}", file=sys.stderr)
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
