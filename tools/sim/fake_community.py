# SPDX-License-Identifier: GPL-3.0-only
"""A stand-in for the community presets API (docs/community-api.md), to try the Community tab without the site.

    python tools/sim/fake_community.py [--port 8790] [--approve]

Seeds a few presets; what the app publishes stays pending unless --approve (then it shows at once). Point the
app at it: [community] api=http://127.0.0.1:8790/api/presets.php (the mock page ?mock uses it when it answers).
"""
import hashlib, json, sys, time, random, string
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT = int(sys.argv[sys.argv.index('--port') + 1]) if '--port' in sys.argv else 8790
APPROVE = '--approve' in sys.argv
now = int(time.time())
P = [
    dict(id='a1', name='Sunset over the sea', author='mira', effect='ocean', palette=['#FF6B35', '#F7C59F', '#2E86AB'], speed=3, bri=0, dl=48, likes=21, ts=now - 86400 * 9),
    dict(id='a2', name='Cyberpunk rain', author='neo_77', effect='matrix', palette=['#00FF9C', '#FF00E5'], speed=6, bri=80, dl=93, likes=40, ts=now - 86400 * 20),
    dict(id='a3', name='Cosy fireplace', author='haku', effect='candle', palette=['#FF8A00', '#FF3D00', '#FFD180'], speed=4, bri=55, dl=31, likes=17, ts=now - 86400 * 3),
    dict(id='a4', name='Northern lights', author='lumi', effect='aurora', palette=['#00F5A0', '#00D9F5', '#7B2FF7'], speed=2, bri=0, dl=12, likes=9, ts=now - 86400),
    dict(id='a5', name='Vaporwave', author='ret.ro', effect='flow', palette=['#FF71CE', '#01CDFE', '#05FFA1', '#B967FF'], speed=5, bri=0, dl=67, likes=25, ts=now - 3600 * 5),
    dict(id='a6', name='Lava lamp', author='mira', effect='lava', palette=['#FF2E00', '#FFB800', '#8A00FF'], speed=2, bri=70, dl=5, likes=2, ts=now - 600),
]
PENDING, MARKS = [], set()

def log(m): print(time.strftime('%H:%M:%S'), m, flush=True)

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_OPTIONS(self):
        self.send_response(204)
        for k, v in (('Access-Control-Allow-Origin', '*'), ('Access-Control-Allow-Methods', 'GET, POST'), ('Access-Control-Allow-Headers', 'Content-Type')):
            self.send_header(k, v)
        self.end_headers()
    def do_GET(self):
        u = urlparse(self.path)
        if u.path != '/api/presets.php': self.send_error(404); return
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        items = list(P)
        if q.get('q'): items = [p for p in items if q['q'].lower() in (p['name'] + ' ' + p['author']).lower()]
        if q.get('effect'): items = [p for p in items if p['effect'] == q['effect']]
        if q.get('sort') == 'new': items.sort(key=lambda p: -p['ts'])
        else: items.sort(key=lambda p: (-p['likes'], -p['dl'], -p['ts']))
        page = int(q.get('page', 0)); inst = q.get('install', '')
        out = [dict(p, liked=(p['id'], inst, 'like') in MARKS) for p in items[page * 24:page * 24 + 24]]
        log(f"list sort={q.get('sort')} q={q.get('q')!r} effect={q.get('effect')} page={page}: {len(out)}")
        self.send({'items': out, 'more': len(items) > page * 24 + 24})
    def do_POST(self):
        if urlparse(self.path).path != '/api/presets.php': self.send_error(404); return
        try: b = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
        except ValueError: self.send({'error': 'invalid'}); return
        log(f"POST {b.get('action')} {json.dumps(b)[:160]} (Content-Type {self.headers.get('Content-Type')})")
        a, inst = b.get('action'), str(b.get('install', ''))
        p = next((x for x in P if x['id'] == b.get('id')), None)
        if a == 'publish':
            ok = 1 <= len(str(b.get('name', '')).strip()) <= 40 and 1 <= len(str(b.get('author', '')).strip()) <= 24 and isinstance(b.get('palette'), list)
            if not ok: self.send({'error': 'invalid'}); return
            item = dict(id=''.join(random.choices(string.ascii_lowercase + string.digits, k=6)), name=b['name'].strip(), author=b['author'].strip(),
                        effect=b['effect'], palette=[c.upper() for c in b['palette']][:8], speed=int(b.get('speed', 5)), bri=int(b.get('bri', 0)), dl=0, likes=0, ts=int(time.time()))
            (P if APPROVE else PENDING).append(item)
            self.send({'ok': True, 'status': 'approved' if APPROVE else 'pending'})
        elif a in ('download', 'like'):
            if not p: self.send({'error': 'unknown'}); return
            key = (p['id'], inst, 'dl' if a == 'download' else 'like')
            if a == 'download':
                if key not in MARKS: MARKS.add(key); p['dl'] += 1
                self.send({'ok': True, 'dl': p['dl']})
            else:
                if key in MARKS: MARKS.discard(key); p['likes'] -= 1
                else: MARKS.add(key); p['likes'] += 1
                self.send({'ok': True, 'likes': p['likes'], 'liked': key in MARKS})
        else: self.send({'error': 'invalid'})

log(f'fake community API on http://127.0.0.1:{PORT}/api/presets.php' + (' (publishing approves at once)' if APPROVE else ''))
ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
