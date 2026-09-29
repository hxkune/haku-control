# SPDX-License-Identifier: GPL-3.0-only
"""Stand-in for Ollama on 127.0.0.1:11434, to test the mood feature without a real model.

Answers /api/tags with two models (an embedding model first, which must be skipped) and /api/chat with a
fixed scene, JSON-escaped the way Ollama does it (\\n, \\" and \\u escapes for non-ASCII).
    python tools/sim/fake_ollama.py            normal answers
    python tools/sim/fake_ollama.py --bad      the model answers without colours
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

BAD = '--bad' in sys.argv
SCENES = [
    {"name": "Закат на пляже", "colors": ["#ff5a1f", "#ff2d6f", "#8a2be2", "#ffb347"], "effect": "flow", "speed": 3},
    {"name": "Cyberpunk rain", "colors": ["#00e5ff", "#ff00a8", "#7b2cff"], "effect": "caustic", "speed": 6},
]
n = 0


class H(BaseHTTPRequestHandler):
    def _json(self, obj):
        body = json.dumps(obj, ensure_ascii=True).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/api/tags':
            self._json({"models": [{"name": "nomic-embed-text:latest"}, {"name": "gemma3:4b"}]})
        else:
            self.send_error(404)

    def do_POST(self):
        global n
        req = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        user = req['messages'][-1]['content']
        print('chat:', req['model'], 'temp', req['options']['temperature'], '|', ascii(user), flush=True)
        time.sleep(1.5)
        scene = {"name": "?", "effect": "flow", "speed": 5} if BAD else SCENES[n % len(SCENES)]
        n += 1
        content = json.dumps(scene, ensure_ascii=False, indent=1)   # with newlines, like a real model
        self._json({"model": req['model'], "message": {"role": "assistant", "content": content}, "done": True})

    def log_message(self, *a):
        pass


HTTPServer(('127.0.0.1', 11434), H).serve_forever()
