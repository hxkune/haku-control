# The UI tests' file server (tools/sim/ui-tests.ps1): python -m http.server with a deeper queue of connections.
# Its queue of 5 overflowed on a 2-core runner when Chrome opened its connections at once; Windows then dropped the
# rest and Chrome tried again 1, 2, 4... s later: a frame's style sheet came after 7 s, a fetch after 18 s.
#   python tools/sim/ui-server.py <port> <folder>
import functools, http.server, sys

class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 128

class Files(http.server.SimpleHTTPRequestHandler):
    # each request also when it begins: one that never ends shows as a 'begin' with no answer after it (ui-tests.ps1)
    def parse_request(self):
        ok = super().parse_request()
        if ok: self.log_message('begin %s', self.requestline)
        return ok

port, folder = int(sys.argv[1]), sys.argv[2]
Server(('127.0.0.1', port), functools.partial(Files, directory=folder)).serve_forever()
