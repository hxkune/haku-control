# SPDX-License-Identifier: GPL-3.0-only
"""Fake LAN lights for testing haku control without the hardware.

    python tools/sim/fake_devices.py            # all fakes on this PC
    python tools/sim/fake_devices.py wled hue   # only some

Every fake prints what it receives (frames per second, a sample colour), so a dev build of haku control
(build.cmd dev) can be pointed at them. Addresses to add in the app:
    WLED     127.0.0.1:8080  (real WLED uses port 80; mDNS answers point there when port 80 is free)
    OpenRGB  127.0.0.1       (port 6742: keyboard, fan hub, graphics card, memory, board; --orgb-shuffle reverses them)
    Govee    127.0.0.1       (scan answers on UDP 4002, commands on 4003)
    LIFX     127.0.0.1       (UDP 56700)
    WiZ      127.0.0.1       (UDP 38899)
    Yeelight 127.0.0.1       (TCP 55443, music mode)
    Hue      127.0.0.1:8081  (pairing succeeds on the second try, 3 colour lights)
    Elgato   127.0.0.1       (Key Light, HTTP 9123, found by mDNS) and 127.0.0.1:9124 (Light Strip, colour)
    Divoom   127.0.0.1:8082  (Times Gate, LocalToken 1234), 127.0.0.1:8083 (Times Frame)
"""
import json, socket, struct, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LOCK = threading.Lock()
def log(tag, msg):
    with LOCK:
        print(f'{time.strftime("%H:%M:%S")} [{tag}] {msg}', flush=True)

class Rate:
    """Counts frames and prints the rate once a second."""
    def __init__(self, tag): self.tag, self.n, self.t, self.last = tag, 0, time.time(), ''
    def hit(self, sample):
        self.n += 1; self.last = sample
        now = time.time()
        if now - self.t >= 1:
            log(self.tag, f'{self.n / (now - self.t):.1f} frames/s  {sample}')
            self.n, self.t = 0, now

def own_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('10.255.255.255', 1)); return s.getsockname()[0]
    except OSError:
        return '127.0.0.1'
    finally:
        s.close()

def mcast_socket(group, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('', port))
    mreq = struct.pack('4s4s', socket.inet_aton(group), socket.inet_aton(own_ip()))
    try: s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    except OSError as e: log('mcast', f'join {group} failed: {e}')
    return s

# ------------------------------------------------------------------ WLED
WLED = {'on': True, 'leds': 60}

class WledHttp(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, obj):
        b = json.dumps(obj).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path == '/json/info':
            self.reply({'ver': '0.14.4', 'name': 'Fake WLED', 'arch': 'esp32', 'leds': {'count': WLED['leds'], 'rgbw': False}, 'fxcount': 187})
        elif self.path == '/json/state':
            self.reply({'on': WLED['on'], 'bri': 128})
        else:
            self.send_error(404)
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
        log('wled', f'POST {self.path} {body}')
        try:
            js = json.loads(body)
            if 'on' in js: WLED['on'] = js['on']
        except ValueError: pass
        self.reply({'success': True})

def wled_udp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('', 21324))
    r = Rate('wled udp')
    while True:
        d, _ = s.recvfrom(2000)
        if d[0] == 4:
            start = d[2] << 8 | d[3]; n = (len(d) - 4) // 3
            r.hit(f'DNRGB timeout={d[1]} start={start} leds={n} first=#{d[4:7].hex()}')
        else:
            r.hit(f'protocol {d[0]} len {len(d)}')

def mdns_responder(services):
    """Answers legacy-unicast mDNS PTR queries for the given service labels (b'_wled', b'_hue')."""
    s = mcast_socket('224.0.0.251', 5353)
    while True:
        d, addr = s.recvfrom(1500)
        if len(d) < 12 or d[2] & 0x80: continue
        for label in services:
            if bytes([len(label)]) + label in d:
                # minimal answer: header (response, 1 answer) + the question name as a PTR record
                q = d[12:]
                end = q.index(0) + 1
                name = q[:end]
                ans = name + struct.pack('>HHIH', 12, 1, 120, len(name)) + name
                pkt = struct.pack('>HHHHHH', struct.unpack('>H', d[:2])[0], 0x8400, 0, 1, 0, 0) + ans
                s.sendto(pkt, addr)
                log('mdns', f'answered {label.decode()} to {addr}')

# ------------------------------------------------------------------ OpenRGB SDK (protocol 0)
def orgb_str(s):
    b = s.encode() + b'\0'
    return struct.pack('<H', len(b)) + b

def orgb_controller(name, ctype, nleds, matrix):
    body = struct.pack('<i', ctype) + orgb_str(name) + orgb_str('fake device') + orgb_str('1.0') + orgb_str('SN1') + orgb_str('HID: fake')
    modes = [('Direct', 0, 0), ('Static', 1, 1), ('Breathing', 2, 2)]
    body += struct.pack('<Hi', len(modes), 1)
    for mname, val, ncol in modes:
        body += orgb_str(mname) + struct.pack('<iIIIIIIII', val, 0, 0, 0, 0, ncol, 0, 0, 1) + struct.pack('<H', ncol) + b'\x11\x22\x33\x00' * ncol
    zones = [('Main', nleds - 4, matrix), ('Logo', 4, None)]
    body += struct.pack('<H', len(zones))
    for zname, cnt, mtx in zones:
        body += orgb_str(zname) + struct.pack('<iIII', 1, cnt, cnt, cnt)
        if mtx:
            h, w = mtx
            body += struct.pack('<HII', 8 + 4 * h * w, h, w) + b''.join(struct.pack('<I', i) for i in range(h * w))
        else:
            body += struct.pack('<H', 0)
    body += struct.pack('<H', nleds) + b''.join(orgb_str(f'Key {i}') + struct.pack('<I', i) for i in range(nleds))
    body += struct.pack('<H', nleds) + b''.join(struct.pack('<I', 0x00FF8000) for _ in range(nleds))
    return struct.pack('<I', 4 + len(body)) + body

ORGB_CTRL = [orgb_controller('Fake Keyboard', 5, 24, (4, 5)), orgb_controller('Fake Fan Hub', 3, 12, None),
             orgb_controller('ASUS TUF RTX 4070', 2, 8, None), orgb_controller('Corsair Vengeance RGB Pro', 1, 10, None),
             orgb_controller('ASUS ROG STRIX B550-F GAMING', 0, 12, None)]
if '--orgb-shuffle' in sys.argv:   # OpenRGB found its devices in another order: haku finds them again by name
    ORGB_CTRL.reverse()
    sys.argv.remove('--orgb-shuffle')

def orgb_client(c, addr):
    rates = {}
    def recv(n):
        b = b''
        while len(b) < n:
            x = c.recv(n - len(b))
            if not x: raise ConnectionError
            b += x
        return b
    try:
        while True:
            h = recv(16)
            if h[:4] != b'ORGB': log('openrgb', 'bad magic'); return
            dev, pid, size = struct.unpack('<III', h[4:])
            data = recv(size) if size else b''
            def out(pid2, dev2, payload):
                c.sendall(b'ORGB' + struct.pack('<III', dev2, pid2, len(payload)) + payload)
            if pid == 50: log('openrgb', f'client name {data.rstrip(bytes(1)).decode()}')
            elif pid == 0: out(0, 0, struct.pack('<I', len(ORGB_CTRL)))
            elif pid == 1: out(1, dev, ORGB_CTRL[dev])
            elif pid == 1100: log('openrgb', f'controller {dev}: custom mode')
            elif pid == 1050:
                n = struct.unpack('<H', data[4:6])[0]
                first = data[6:10]
                rates.setdefault(dev, Rate(f'openrgb {dev}')).hit(f'{n} colours, first=#{first[:3].hex()}')
            else: log('openrgb', f'packet {pid} ({size} bytes)')
    except (ConnectionError, OSError):
        log('openrgb', f'client {addr} gone')

def openrgb():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', 6742)); s.listen()
    while True:
        c, a = s.accept()
        threading.Thread(target=orgb_client, args=(c, a), daemon=True).start()

# ------------------------------------------------------------------ Govee LAN
GOVEE = {'onOff': 1, 'brightness': 70, 'color': {'r': 255, 'g': 120, 'b': 0}, 'colorTemInKelvin': 0}

def govee():
    scan = mcast_socket('239.255.255.250', 4001)
    ctl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ctl.bind(('', 4003))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    def scanner():
        while True:
            d, addr = scan.recvfrom(1500)
            if b'"scan"' in d:
                rep = {'msg': {'cmd': 'scan', 'data': {'ip': '127.0.0.1', 'device': 'AA:BB:CC:DD:EE:FF:00:11', 'sku': 'H6076',
                                                       'bleVersionHard': '3.01.01', 'wifiVersionSoft': '1.02.03'}}}
                tx.sendto(json.dumps(rep).encode(), (addr[0], 4002)); log('govee', f'scan from {addr[0]}')
    threading.Thread(target=scanner, daemon=True).start()
    r = Rate('govee')
    while True:
        d, addr = ctl.recvfrom(1500)
        js = json.loads(d)['msg']
        if js['cmd'] == 'devStatus':
            tx.sendto(json.dumps({'msg': {'cmd': 'devStatus', 'data': GOVEE}}).encode(), (addr[0], 4002)); log('govee', 'devStatus')
        elif js['cmd'] == 'colorwc': r.hit(f'colour {js["data"]["color"]}')
        else: log('govee', f'{js["cmd"]} {js["data"]}')

# ------------------------------------------------------------------ LIFX
def lifx():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('', 56700))
    state = {'hsbk': (0, 0, 40000, 3500), 'power': 65535}
    r = Rate('lifx')
    def reply(addr, source, seq, typ, payload):
        hdr = struct.pack('<HHI8s6sBBQHH', 36 + len(payload), 1024 | 0x1000, source, b'\xd0\x73\xd5\x00\x00\x01\x00\x00', b'\0' * 6, 0, seq, 0, typ, 0)
        s.sendto(hdr + payload, addr)
    while True:
        d, addr = s.recvfrom(1500)
        if len(d) < 36: continue
        size, proto, source, target, _, flags, seq, _, typ, _ = struct.unpack('<HHI8s6sBBQHH', d[:36])
        p = d[36:]
        if typ == 2: reply(addr, source, seq, 3, struct.pack('<BI', 1, 56700)); log('lifx', f'GetService from {addr[0]}')
        elif typ == 101:
            label = b'Fake LIFX'.ljust(32, b'\0')
            reply(addr, source, seq, 107, struct.pack('<HHHHhH', *state['hsbk'], 0, state['power']) + label + b'\0' * 8)
        elif typ == 102: h, sat, b, k = struct.unpack('<HHHH', p[1:9]); r.hit(f'hsbk {h} {sat} {b} {k}')
        elif typ == 117: log('lifx', f'power {struct.unpack("<H", p[:2])[0]}')
        else: log('lifx', f'type {typ}')

# ------------------------------------------------------------------ WiZ
def wiz():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('', 38899))
    pilot = {'mac': 'a8bb50c0ffee', 'rssi': -50, 'state': True, 'sceneId': 11, 'speed': 100, 'temp': 2700, 'dimming': 60}
    r = Rate('wiz')
    while True:
        d, addr = s.recvfrom(1500)
        try: js = json.loads(d)
        except ValueError: continue
        m, p = js.get('method'), js.get('params', {})
        def out(res): s.sendto(json.dumps({'method': m, 'env': 'pro', 'result': res}).encode(), addr)
        if m == 'registration': out({'mac': pilot['mac'], 'success': True}); log('wiz', f'registration from {addr[0]}')
        elif m == 'getSystemConfig': out({'mac': pilot['mac'], 'moduleName': 'ESP01_SHRGB_03', 'fwVersion': '1.25.0', 'homeId': 1})
        elif m == 'getPilot': out(pilot); log('wiz', 'getPilot')
        elif m == 'setPilot':
            out({'success': True})
            if 'r' in p: r.hit(f"rgb {p['r']} {p['g']} {p['b']} dim {p['dimming']}")
            else: log('wiz', f'setPilot {p}')

# ------------------------------------------------------------------ Yeelight
def yeelight():
    ssdp = mcast_socket('239.255.255.250', 1982)
    def search():
        while True:
            d, addr = ssdp.recvfrom(1500)
            if b'M-SEARCH' in d:
                rep = ('HTTP/1.1 200 OK\r\nCache-Control: max-age=3600\r\nLocation: yeelight://127.0.0.1:55443\r\n'
                       'id: 0x000000000015243f\r\nmodel: color\r\nfw_ver: 18\r\nsupport: get_prop set_rgb set_bright set_power set_music\r\n'
                       'power: on\r\nbright: 100\r\ncolor_mode: 2\r\nct: 4000\r\nrgb: 16711680\r\nname: Fake Yeelight\r\n\r\n')
                ssdp.sendto(rep.encode(), addr); log('yeelight', f'search from {addr}')
    threading.Thread(target=search, daemon=True).start()
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); srv.bind(('', 55443)); srv.listen()
    def lines(c):
        buf = b''
        while True:
            x = c.recv(4096)
            if not x: return
            buf += x
            while b'\r\n' in buf:
                ln, buf = buf.split(b'\r\n', 1)
                if ln: yield json.loads(ln)
    def music(ip, port):
        m = socket.create_connection((ip, port), timeout=3)
        log('yeelight', f'music mode connected to {ip}:{port}')
        r = Rate('yeelight music')
        try:
            for js in lines(m): r.hit(f'{js["method"]} {js["params"]}')
        except OSError: pass
        log('yeelight', 'music connection closed')
    def client(c):
        try:
            for js in lines(c):
                m = js['method']
                if m == 'get_prop': res = ['on', '100', '16711680', '4000', '2']
                else: res = ['ok']
                c.sendall((json.dumps({'id': js['id'], 'result': res}) + '\r\n').encode())
                log('yeelight', f'{m} {js["params"]}')
                if m == 'set_music' and js['params'][0] == 1:
                    threading.Thread(target=music, args=(js['params'][1], js['params'][2]), daemon=True).start()
        except (OSError, ValueError): pass
    while True:
        c, _ = srv.accept()
        threading.Thread(target=client, args=(c,), daemon=True).start()

# ------------------------------------------------------------------ Hue bridge
HUE = {'tries': 0, 'user': 'fakeuser0123456789'}
HUE_LIGHTS = {str(i): {'state': {'on': True, 'bri': 200, 'xy': [0.4, 0.4], 'ct': 300, 'colormode': 'xy', 'reachable': True},
                       'type': 'Extended color light', 'name': f'Lamp {i}'} for i in (1, 2, 4)}
HUE_LIGHTS['3'] = {'state': {'on': True, 'bri': 100, 'ct': 300, 'colormode': 'ct'}, 'type': 'Color temperature light', 'name': 'White lamp'}
HUE_RATE = Rate('hue')

class HueHttp(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, obj):
        b = json.dumps(obj).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path == '/api/config': self.reply({'name': 'Fake Hue', 'bridgeid': '001788FFFE000000', 'modelid': 'BSB002', 'apiversion': '1.60.0'})
        elif self.path == f'/api/{HUE["user"]}/lights': self.reply(HUE_LIGHTS)
        elif self.path.endswith('/lights'): self.reply([{'error': {'type': 1, 'description': 'unauthorized user'}}])
        else: self.send_error(404)
    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', 0)))
        HUE['tries'] += 1
        if HUE['tries'] < 2:
            log('hue', 'pairing: link button not pressed'); self.reply([{'error': {'type': 101, 'description': 'link button not pressed'}}])
        else:
            log('hue', 'pairing: ok'); self.reply([{'success': {'username': HUE['user']}}])
    def do_PUT(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
        HUE_RATE.hit(f'{self.path.split("/")[4]} {body}')
        self.reply([{'success': {}}])

# ------------------------------------------------------------------ Elgato (Key Light / Light Strip)
def elgato_http(product, colour):
    state = {'on': 1, 'brightness': 40, 'hue': 30.0, 'saturation': 60.0} if colour else {'on': 1, 'brightness': 40, 'temperature': 213}
    rate = Rate('elgato ' + ('strip' if colour else 'key'))
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def reply(self, obj):
            b = json.dumps(obj).encode()
            self.send_response(200); self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
        def do_GET(self):
            if self.path == '/elgato/accessory-info':
                self.reply({'productName': product, 'hardwareBoardType': 70 if colour else 53, 'firmwareBuildNumber': 218,
                            'firmwareVersion': '1.0.3', 'serialNumber': 'CW00SIM000', 'displayName': '', 'features': ['lights']})
            elif self.path == '/elgato/lights': self.reply({'numberOfLights': 1, 'lights': [state]})
            else: self.send_error(404)
        def do_PUT(self):
            body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
            if self.path != '/elgato/lights' or 'lights' not in body: self.send_error(400); return
            state.update(body['lights'][0])
            l = state
            k = f"{round(1e6 / l['temperature'])} K" if 'temperature' in l else f"hue {l['hue']} sat {l['saturation']}"
            rate.hit(f"on={l['on']} bri={l['brightness']} {k}")
            self.reply({'numberOfLights': 1, 'lights': [state]})
    return H

# ------------------------------------------------------------------ Divoom Times Gate (hardware 400: POST /post)
DIVOOM_TOKEN = 1234
DIVOOM_RATE = Rate('divoom')

class DivoomHttp(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, obj):
        b = json.dumps(obj).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
        if self.path == '/Device/ReturnSameLANDevice':   # Divoom's cloud list ([divoom] lan_url= points here)
            self.reply({'ReturnCode': 0, 'ReturnMessage': '', 'DeviceList': [
                {'DeviceName': 'Times Gate', 'DeviceId': 585010, 'DevicePrivateIP': '127.0.0.1:8082', 'DeviceMac': 'a8032a000000', 'Hardware': 400}]})
            return
        if self.path != '/post': self.send_error(404); return
        if body.get('LocalToken') != DIVOOM_TOKEN: log('divoom', f"refused {body.get('Command')}: token {body.get('LocalToken')}"); self.reply({'error_code': 'DeviceToken is err'}); return
        c = body.get('Command')
        if c == 'Channel/GetAllConf':
            self.reply({'error_code': 0, 'Brightness': 80, 'RotationFlag': 0, 'ClockTime': 60, 'GalleryTime': 60, 'LightSwitch': 1})
        elif c == 'Channel/SetRGBInfo':
            DIVOOM_RATE.hit(f"on={body['OnOff']} {body['Color']} bri={body['Brightness']} zone={body['SelectLightIndex']} fx={[x['SelectEffect'] for x in body['LightList']]}")
            self.reply({'error_code': 0})
        else: self.reply({'error_code': 1})

# Divoom Times Frame: GET with the JSON as its body, answers {"ReturnCode": ...}; its lights' command is unknown,
# so it refuses SetRGBInfo the way it refuses any command it lacks
class DivoomFrameHttp(DivoomHttp):
    def do_POST(self): self.send_error(404)
    def do_GET(self):
        body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
        if self.path != '/divoom_api': self.send_error(404); return
        c = body.get('Command')
        log('divoom-frame', f"{c}")
        if c == 'Channel/GetAllConf': self.reply({'ReturnCode': 0, 'ReturnMessage': '', 'Brightness': 70, 'DeviceId': 300256986})
        else: self.reply({'ReturnCode': 1, 'ReturnMessage': 'Only accept JSON parameters'})

def serve(cls, port, tag):
    try:
        ThreadingHTTPServer(('', port), cls).serve_forever()
    except OSError as e:
        log(tag, f'port {port}: {e}')

FAKES = {
    'wled': lambda: [threading.Thread(target=serve, args=(WledHttp, 8080, 'wled'), daemon=True).start(),
                     threading.Thread(target=serve, args=(WledHttp, 80, 'wled'), daemon=True).start(), wled_udp()],
    'openrgb': openrgb, 'govee': govee, 'lifx': lifx, 'yeelight': yeelight, 'wiz': wiz,
    'hue': lambda: serve(HueHttp, 8081, 'hue'),
    'elgato': lambda: [threading.Thread(target=serve, args=(elgato_http('Elgato Light Strip', True), 9124, 'elgato'), daemon=True).start(),
                       serve(elgato_http('Elgato Key Light Air', False), 9123, 'elgato')],
    'divoom': lambda: [threading.Thread(target=serve, args=(DivoomFrameHttp, 8083, 'divoom-frame'), daemon=True).start(),
                       serve(DivoomHttp, 8082, 'divoom')],
    'mdns': lambda: mdns_responder([b'_wled', b'_hue', b'_elg']),
}

if __name__ == '__main__':
    names = sys.argv[1:] or list(FAKES)
    for n in names:
        threading.Thread(target=FAKES[n], daemon=True, name=n).start()
    log('sim', 'running: ' + ', '.join(names))
    try:
        while True: time.sleep(1)
    except KeyboardInterrupt:
        pass
