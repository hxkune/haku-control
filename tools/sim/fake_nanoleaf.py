# SPDX-License-Identifier: GPL-3.0-only
"""Stand-in for a Nanoleaf controller on 127.0.0.1:16021 (+ UDP 60222), to test the Nanoleaf driver.

Four Shapes triangles. Checks every custom animation it gets (panel count, frames, colour values, fade times)
and counts extControl UDP frames, so you can see whether the app streams or lets the panels play the loop.
    python tools/sim/fake_nanoleaf.py [--keep-open]
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
state = {"on": True, "brightness": 80, "select": "Northern Lights", "udp": 0, "writes": 0}


def info():
    return {
        "name": "Shapes FAKE", "model": "NL42",
        "state": {"on": {"value": state["on"]}, "brightness": {"value": state["brightness"]}, "hue": {"value": 0},
                  "sat": {"value": 0}, "ct": {"value": 4000}, "colorMode": "effect"},
        "effects": {"select": state["select"], "effectsList": ["Northern Lights"]},
        "panelLayout": {"globalOrientation": {"value": 0}, "layout": {"numPanels": len(PANELS) + 1, "sideLength": 0,
            "positionData": [{"panelId": 0, "x": 150, "y": -40, "o": 0, "shapeType": 12}] +
                            [{"panelId": i, "x": round(x), "y": round(y), "o": o, "shapeType": 8} for i, x, y, o in PANELS]}},
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
            return self.reply(401)
        p = p[len("/api/v1/test"):]
        if p in ("", "/"):
            return self.reply(200, info())
        if p == "/state/on":
            return self.reply(200, {"value": state["on"]})
        if p == "/effects/select":
            return self.reply(200, state["select"])
        self.reply(404)

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
    s.bind(("127.0.0.1", 60222))
    while True:
        s.recv(2048)
        state["udp"] += 1


def report():
    last = 0
    while True:
        time.sleep(5)
        if state["udp"] != last:
            print(time.strftime("%H:%M:%S"), f"UDP frames in the last 5 s: {state['udp'] - last}", flush=True)
            last = state["udp"]


threading.Thread(target=udp, daemon=True).start()
threading.Thread(target=report, daemon=True).start()
ThreadingHTTPServer(("127.0.0.1", 16021), Api).serve_forever()
