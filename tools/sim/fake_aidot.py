# SPDX-License-Identifier: GPL-3.0-only
"""Stand-in for the AiDot cloud on http://127.0.0.1:8779, to test the in-app AiDot sign-in.

Test build: [aidot] server=http://127.0.0.1:8779 in %APPDATA%\\haku-control-dev\\settings.ini. The password must
arrive RSA-encrypted (128 bytes, base64); e-mail "wrong@test" is refused, "empty@test" has no bulbs.
    python tools/sim/fake_aidot.py
"""
import base64
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

LIGHTS = [
    {"id": "dev-1", "name": "Desk lamp", "mac": "AA:BB:CC:00:00:01", "modelId": "LK.light.A001931", "type": "light",
     "aesKey": ["0123456789abcdef"], "password": "päss\"1", "simpleVersion": "2", "product": {"id": "nested-id", "name": "x"}},
    {"id": "dev-2", "name": "Лампа", "mac": "AA:BB:CC:00:00:02", "modelId": "LK.light.A001931", "type": "light",
     "aesKey": ["fedcba9876543210"], "password": "pw2", "simpleVersion": None},
    {"id": "dev-3", "name": "Plug", "mac": "AA:BB:CC:00:00:03", "type": "plug", "aesKey": ["x"]},
]
EMAIL = {}


class H(BaseHTTPRequestHandler):
    def out(self, code, obj):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path != "/v35/users/loginWithFreeVerification" or self.headers.get("Appid") != "1383974540041977857":
            return self.out(404, {})
        raw = base64.b64decode(body["password"])
        ok = len(raw) == 128 and body["countryKey"].startswith("region:") and len(body["terminalId"]) >= 16
        print("login:", body["countryKey"], "| password: RSA block of", len(raw), "bytes", "| well-formed" if ok else "| BAD", flush=True)
        if body["username"] == "wrong@test" or not ok:
            return self.out(400, {"code": 21003, "desc": "Wrong account or password"})
        EMAIL["last"] = body["username"]
        self.out(200, {"id": "user-42", "accessToken": "tok-123", "refreshToken": "r", "nested": {"id": "not-this"}})

    def do_GET(self):
        if self.headers.get("Token") != "tok-123":
            return self.out(401, {})
        u = urlparse(self.path)
        if u.path == "/v35/houses":
            return self.out(200, [{"id": "h1", "isOwner": True, "name": "Home"}, {"id": "h2", "isOwner": False}])
        if u.path == "/v35/devices":
            hid = parse_qs(u.query).get("houseId", [""])[0]
            print("devices of", hid, flush=True)
            if hid == "h2":
                return self.out(200, [dict(LIGHTS[0], id="shared-should-not-appear")])
            return self.out(200, [] if EMAIL.get("last") == "empty@test" else LIGHTS)
        self.out(404, {})

    def log_message(self, *a):
        pass


HTTPServer(("127.0.0.1", 8779), H).serve_forever()
