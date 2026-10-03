# SPDX-License-Identifier: GPL-3.0-only
"""A tiny stand-in for an MQTT broker with Home Assistant behind it, to try haku's MQTT (haku Pro) without one.

    python tools/sim/fake_mqtt.py [--port 1883] [--auth user:pass] [--script]

It accepts one client at a time (MQTT 3.1.1, QoS 0), prints what haku publishes (discovery configs, state, status)
and, with --script, sends the commands Home Assistant would: on with an effect, a brightness, a colour, a profile,
off, and "homeassistant/status online" (haku announces itself again). Point haku at it: [mqtt] on=1,
host=127.0.0.1, port=1883 (and user / password with --auth).
"""
import json, socket, struct, sys, threading, time

PORT = 1883
AUTH = None
SCRIPT = '--script' in sys.argv
args = sys.argv[1:]
if '--port' in args: PORT = int(args[args.index('--port') + 1])
if '--auth' in args: AUTH = args[args.index('--auth') + 1].split(':', 1)

def log(msg): print(time.strftime('%H:%M:%S'), msg, flush=True)

def read_exact(c, n):
    b = b''
    while len(b) < n:
        k = c.recv(n - len(b))
        if not k: raise ConnectionError
        b += k
    return b

def read_pkt(c):
    h = read_exact(c, 1)[0]
    n, mul = 0, 1
    while True:
        d = read_exact(c, 1)[0]
        n += (d & 127) * mul; mul *= 128
        if not d & 128: break
    return h, read_exact(c, n) if n else b''

def pkt(head, body):
    n, out = len(body), b''
    while True:
        d = n % 128; n //= 128
        out += bytes([d | (128 if n else 0)])
        if not n: break
    return bytes([head]) + out + body

def s16(b, i):
    n = struct.unpack('>H', b[i:i + 2])[0]
    return b[i + 2:i + 2 + n].decode('utf-8', 'replace'), i + 2 + n

def publish(c, topic, msg):
    t = topic.encode(); c.sendall(pkt(0x30, struct.pack('>H', len(t)) + t + msg.encode()))
    log(f'  -> {topic} {msg}')

def serve(c):
    subs, base = [], None
    h, b = read_pkt(c)
    if h >> 4 != 1: return
    _, i = s16(b, 0); i += 1
    flags = b[i]; i += 3
    cid, i = s16(b, i)
    if flags & 4: wt, i = s16(b, i); wm, i = s16(b, i); log(f'will: {wt} = {wm}')
    user = pw = ''
    if flags & 0x80: user, i = s16(b, i)
    if flags & 0x40: pw, i = s16(b, i)
    if AUTH and [user, pw] != AUTH:
        c.sendall(pkt(0x20, b'\x00\x05')); log(f'refused {cid}: user {user!r}'); return
    c.sendall(pkt(0x20, b'\x00\x00')); log(f'connected: {cid} (user {user!r})')
    def script():
        steps = [('light', {'state': 'ON', 'effect': 'Rainbow'}), ('light', {'brightness': 40}),
                 ('light', {'state': 'ON', 'color': {'r': 255, 'g': 40, 'b': 0}}), ('profile', 'Gaming'),
                 ('ha', 'online'), ('light', {'state': 'OFF'}), ('light', {'state': 'ON', 'brightness': 85})]
        time.sleep(3)
        for kind, msg in steps:
            if not base: return
            topic = 'homeassistant/status' if kind == 'ha' else f'{base}/{kind}/set'
            try: publish(c, topic, msg if isinstance(msg, str) else json.dumps(msg))
            except OSError: return
            time.sleep(4)
    while True:
        h, b = read_pkt(c)
        kind = h >> 4
        if kind == 3:
            t, i = s16(b, 0)
            if h & 6: i += 2
            m = b[i:].decode('utf-8', 'replace')
            if t.startswith('homeassistant/') and t.endswith('/config'):
                j = json.loads(m)
                log(f'discovery {t}: {j.get("name")} effects={len(j.get("effect_list", []))} options={j.get("options")}')
            else:
                log(f'{t} = {m[:200]}{" (retained)" if h & 1 else ""}')
        elif kind == 8:
            pid = b[:2]; i = 2; got = []
            while i < len(b):
                t, i = s16(b, i); i += 1; got.append(t)
            subs += got; c.sendall(pkt(0x90, pid + b'\x00' * len(got)))
            log(f'subscribed: {got}')
            base = next((t[:-len('/light/set')] for t in got if t.endswith('/light/set')), base)
            if SCRIPT and base: threading.Thread(target=script, daemon=True).start()
        elif kind == 12: c.sendall(pkt(0xD0, b''))
        elif kind == 14: log('disconnect'); return

srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', PORT)); srv.listen(2)
log(f'fake MQTT broker on 127.0.0.1:{PORT}' + (f' (user {AUTH[0]})' if AUTH else '') + (', scripted commands' if SCRIPT else ''))
while True:
    c, a = srv.accept()
    try: serve(c)
    except (ConnectionError, OSError) as e: log(f'client gone ({e.__class__.__name__})')
    finally: c.close()
