# The UI tests' file server (tools/sim/ui-tests.ps1): python -m http.server with a deeper queue of connections and
# connections kept open.
# Its queue of 5 overflowed on a 2-core runner when Chrome opened its connections at once; Windows then dropped the
# rest and Chrome tried again 1, 2, 4... s later: a frame's style sheet came after 7 s, a fetch after 18 s.
#   python tools/sim/ui-server.py <port> <folder>
import ctypes, functools, http.server, sys, time

# High priority: on the 2-core runner headless Chrome keeps both cores busy, its GPU process at AboveNormal, and a
# Normal server read a request from a new connection only 8-21 s after it was sent (a frame's scripts missed the
# test's wait). The app serves these files itself, without that wait. It uses little CPU, so it takes little from Chrome.
# The handle must go as a pointer: an int -1 is cut to 32 bits on x64 and the call fails with an invalid handle.
if not ctypes.windll.kernel32.SetPriorityClass(ctypes.c_void_p(-1), 0x80):
    sys.exit('ui-server: SetPriorityClass failed, error %d' % ctypes.windll.kernel32.GetLastError())

class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 128

class Files(http.server.SimpleHTTPRequestHandler):
    # connections kept open: with HTTP/1.0 each file was a new connection, and on the 2-core runner a new connection
    # now and then took seconds, so a frame's files came one by one 1-4 s apart and its scripts missed the test's wait
    protocol_version = 'HTTP/1.1'
    # each request also when it begins: one that never ends shows as a 'begin' with no answer after it (ui-tests.ps1)
    def parse_request(self):
        ok = super().parse_request()
        if ok: self.log_message('begin %s', self.requestline)
        return ok
    # to the ms and with the client's port: lined up with the browser's net log (netlog-slow.py), connection by connection
    def log_date_time_string(self):
        t = time.time()
        return time.strftime('%H:%M:%S', time.localtime(t)) + '.%03d' % (t % 1 * 1000)
    def address_string(self):
        return '%s:%d' % self.client_address[:2]

port, folder = int(sys.argv[1]), sys.argv[2]
Server(('127.0.0.1', port), functools.partial(Files, directory=folder)).serve_forever()
