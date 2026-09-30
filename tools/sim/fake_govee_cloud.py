# SPDX-License-Identifier: GPL-3.0-only
"""Stand-in for Govee's cloud API on http://127.0.0.1:8780, to test the Govee API key sign-in and drv_goveecloud.c.

Test build: [govee] server=http://127.0.0.1:8780 in %APPDATA%\\haku-control-dev\\settings.ini, API key "test-key".
Two devices: an AI Sync Box 2 (screen sync) and a floor lamp. Every command is printed; more than 2 per second for
one device gets 429, like the real service.
    python tools/sim/fake_govee_cloud.py
"""
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

DEVICES = [
    {"sku": "H6604", "device": "AA:BB:CC:DD:EE:FF:00:01", "deviceName": "AI Sync Box 2", "type": "devices.types.light",
     "capabilities": [{"type": "devices.capabilities.on_off", "instance": "powerSwitch"},
                      {"type": "devices.capabilities.range", "instance": "brightness"},
                      {"type": "devices.capabilities.toggle", "instance": "dreamViewToggle"}] +
                     ([] if "--box-no-colour" in __import__("sys").argv else [{"type": "devices.capabilities.color_setting", "instance": "colorRgb"}])},
    {"sku": "H6076", "device": "AA:BB:CC:DD:EE:FF:00:02", "deviceName": "Floor lamp", "type": "devices.types.light",
     "capabilities": [{"type": "devices.capabilities.on_off", "instance": "powerSwitch"},
                      {"type": "devices.capabilities.range", "instance": "brightness"},
                      {"type": "devices.capabilities.color_setting", "instance": "colorRgb"}]},
    {"sku": "H7131", "device": "AA:BB:CC:DD:EE:FF:00:03", "deviceName": "Heater", "type": "devices.types.heater",
     "capabilities": [{"type": "devices.capabilities.on_off", "instance": "powerSwitch"}]},
]
recent = {}


class H(BaseHTTPRequestHandler):
    def out(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def authed(self):
        if self.headers.get("Govee-API-Key") != "test-key":
            self.out(401, {"code": 401, "message": "Invalid API Key"})
            return False
        return True

    def do_GET(self):
        if not self.authed():
            return
        if self.path == "/router/api/v1/user/devices":
            return self.out(200, {"code": 200, "message": "success", "data": DEVICES})
        self.out(404, {})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if not self.authed():
            return
        p = body["payload"]
        dev = next((d for d in DEVICES if d["device"] == p["device"]), None)
        if self.path == "/router/api/v1/device/state":
            return self.out(200, {"requestId": body["requestId"], "msg": "success", "code": 200, "payload": {"sku": p["sku"], "device": p["device"],
                "capabilities": [{"type": c["type"], "instance": c["instance"], "state": {"value": 1 if c["instance"] != "colorRgb" else 16711680}}
                                 for c in dev["capabilities"]]}})
        if self.path != "/router/api/v1/device/control":
            return self.out(404, {})
        now = time.time()
        t = [x for x in recent.get(p["device"], []) if now - x < 1] + [now]
        recent[p["device"]] = t
        c = p["capability"]
        if len(t) > 2:
            print(time.strftime("%H:%M:%S"), p["sku"], "429 too many", flush=True)
            return self.out(429, {"code": 429, "message": "Too many requests"})
        if not any(x["instance"] == c["instance"] for x in dev["capabilities"]):   # like the real service: HTTP 200, error inside
            print(time.strftime("%H:%M:%S"), p["sku"], c["instance"], "not supported", flush=True)
            return self.out(200, {"requestId": body["requestId"], "msg": "Unsupported capability", "code": 400})
        v = c["value"]
        shown = f"#{v:06X}" if c["instance"] == "colorRgb" else v
        print(time.strftime("%H:%M:%S"), p["sku"], c["instance"], shown, flush=True)
        self.out(200, {"requestId": body["requestId"], "msg": "success", "code": 200, "capability": c})

    def log_message(self, *a):
        pass


HTTPServer(("127.0.0.1", 8780), H).serve_forever()
