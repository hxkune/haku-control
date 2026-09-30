# SPDX-License-Identifier: GPL-3.0-only
"""Stand-in for a Nanoleaf controller on 127.0.0.1:16021 (+ UDP 60222), to test the Nanoleaf driver.

Four Shapes triangles. Checks every custom animation it gets (panel count, frames, colour values, fade times)
and counts extControl UDP frames, so you can see whether the app streams or lets the panels play the loop.
    python tools/sim/fake_nanoleaf.py [--keep-open] [--ip 127.0.0.2] [--strip]
--strip: a Secretlab MAGRGB-like lightstrip instead: 41 zones numbered 0..40, all at the same layout spot, and
custom animations refused (the app must stream to it). Several controllers: one per loopback address, with
    {"controllers": [{"ip": "127.0.0.1", "token": "test", "slot": 1}, {"ip": "127.0.0.2", "token": "test", "slot": 2}]}
in nanoleaf.json (the old one-controller form below still works).
Test build: put {"ip": "127.0.0.1", "token": "test"} into %APPDATA%\\haku-control-dev\\nanoleaf.json.
"""
import json
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

H = 134 * 3 ** 0.5 / 2
PANELS = [(11, 67, H / 3, 60), (12, 134, 2 * H / 3, 0), (13, 201, H / 3, 60), (14, 268, 2 * H / 3, 0)]
import sys
KEEP_OPEN = "--keep-open" in sys.argv   # like the real controller: ignores "Connection: close"
STRIP = "--strip" in sys.argv
IP = sys.argv[sys.argv.index("--ip") + 1] if "--ip" in sys.argv else "127.0.0.1"
if STRIP:
    PANELS = [(i, 0, 0, 0) for i in range(41)]
state = {"on": True, "brightness": 80, "select": "Northern Lights", "udp": 0, "writes": 0}


def info():
    return {
        "name": "MAGRGB FAKE" if STRIP else "Shapes FAKE", "model": "NL72S2" if STRIP else "NL42", "serialNo": "FAKE-" + IP,
        "state": {"on": {"value": state["on"]}, "brightness": {"value": state["brightness"]}, "hue": {"value": 0},
                  "sat": {"value": 0}, "ct": {"value": 4000}, "colorMode": "effect"},
        "effects": {"select": state["select"], "effectsList": ["Northern Lights"]},
        # the real MAGRGB (NL72S2) reports no layout at all: its zones are known by model (41)
        **({} if STRIP else {"panelLayout": {"globalOrientation": {"value": 0}, "layout": {"numPanels": len(PANELS) + 1, "sideLength": 0,
            "positionData": [{"panelId": 0, "x": 150, "y": -40, "o": 0, "shapeType": 12}] +
                            [{"panelId": i, "x": round(x), "y": round(y), "o": o, "shapeType": 8} for i, x, y, o in PANELS]}}}),
    }


def check_anim(w):
    nums = [int(v) for v in w["animData"].split()]
    n, k, frames = nums[0], 1, None
    assert n == len(PANELS), f"numPanels {n}"
    ids = []
    for _ in range(n):
        pid, nf = nums[k], nums[k + 1]
        k += 2
        ids.append(pid)
        frames = frames or nf
        assert nf == frames and nf >= 1, "frame counts differ"
        for _ in range(nf):
            r, g, b, w_, t = nums[k:k + 5]
            k += 5
            assert all(0 <= v <= 255 for v in (r, g, b)) and w_ == 0 and t >= 1, f"bad frame {nums[k-5:k]}"
    assert k == len(nums), "trailing numbers"
    assert sorted(ids) == sorted(p[0] for p in PANELS), f"panel ids {ids}"
    return frames, nums[4 + 2 + 1]  # frames, fade time of the first frame


class Api(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def reply(self, code, obj=None):
        body = b"" if obj is None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if not KEEP_OPEN:
            self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = self.path
        if not p.startswith("/api/v1/test"):
            return self.reply(404 if STRIP else 401)
        p = p[len("/api/v1/test"):]
        if STRIP and p.startswith("/panelLayout"):
            return self.reply(500)   # not implemented on the 1D strip
        if p in ("", "/"):
            return self.reply(200, info())
        if p == "/state/on":
            return self.reply(200, {"value": state["on"]})
        if p == "/effects/select":
            return self.reply(200, state["select"])
        self.reply(404)

    def do_POST(self):
        # pairing: 403 until the "Connect to API" window opens (here: the 3rd try), then the token
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        if self.path != "/api/v1/new":
            return self.reply(404)
        state["pair_tries"] = state.get("pair_tries", 0) + 1
        if state["pair_tries"] < 3:
            print(time.strftime("%H:%M:%S"), "pairing refused (window closed)", flush=True)
            return self.reply(403)
        print(time.strftime("%H:%M:%S"), "pairing: token handed out", flush=True)
        self.reply(200, {"auth_token": "test"})

    def do_PUT(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])) or b"{}")
        p = self.path[len("/api/v1/test"):]
        if p == "/state":
            if "on" in body:
                state["on"] = body["on"]["value"]
                print(time.strftime("%H:%M:%S"), "power", "on" if state["on"] else "OFF", flush=True)
            return self.reply(204)
        if p == "/effects" and "select" in body:
            state["select"] = body["select"]
            print(time.strftime("%H:%M:%S"), "scene", body["select"], flush=True)
            return self.reply(204)
        if p == "/effects":
            w = body["write"]
            if w["animType"] == "extControl":
                state["select"] = "*ExtControl*"
                print(time.strftime("%H:%M:%S"), "extControl (streaming)", flush=True)
            elif w["animType"] == "custom":
                if STRIP:
                    print(time.strftime("%H:%M:%S"), "custom animation refused (strip)", flush=True)
                    return self.reply(500)
                try:
                    frames, fade = check_anim(w)
                except (AssertionError, ValueError, IndexError) as e:
                    print("BAD ANIMATION:", e, flush=True)
                    return self.reply(400)
                state["select"] = "*Dynamic*"
                state["writes"] += 1
                print(time.strftime("%H:%M:%S"), f"custom loop: {frames} frames, fade {fade / 10:.1f} s, loop={w.get('loop')},"
                      f" {len(w['animData'])} chars (write #{state['writes']})", flush=True)
            return self.reply(204)
        self.reply(404)

    def log_message(self, *a):
        pass


def udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((IP, 60222))
    while True:
        d = s.recv(2048)
        state["udp"] += 1
        n = int.from_bytes(d[:2], "big")
        ids = [int.from_bytes(d[2 + i * 8:4 + i * 8], "big") for i in range(n)]
        if len(d) != 2 + n * 8 or any(i >= len(PANELS) for i in ids):   # the real strip drops such frames whole
            print(time.strftime("%H:%M:%S"), f"BAD FRAME: {n} panels, ids {min(ids, default=0)}..{max(ids, default=0)}", flush=True)


def report():
    last = 0
    while True:
        time.sleep(5)
        if state["udp"] != last:
            print(time.strftime("%H:%M:%S"), f"UDP frames in the last 5 s: {state['udp'] - last}", flush=True)
            last = state["udp"]


threading.Thread(target=udp, daemon=True).start()
threading.Thread(target=report, daemon=True).start()
ThreadingHTTPServer((IP, 16021), Api).serve_forever()
