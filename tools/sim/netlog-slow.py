# The test server's requests in a Chrome net log (chrome --log-net-log), printed by ui-tests.ps1 when a test failed:
# those from the last frame of the main page on (when the browser asked for each and how long it took), then the slowest
# with their steps: whether a request waited for the cache, for a connection or for the server.
#   python tools/sim/netlog-slow.py <netlog.json> [min ms = 1000] [how many = 6]
# The browser is killed at the end of a run, so the file's end is cut off: it is read one event per line.
import datetime, json, sys

path, least, most = sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 1000, int(sys.argv[3]) if len(sys.argv) > 3 else 6
text = open(path, encoding='utf-8', errors='replace').read()
cut = text.find('"events"')
if cut < 0: sys.exit('no events in ' + path)
head = text[:cut].rstrip().rstrip(',') + '}'
C = json.loads(head)['constants']
etype = {v: k for k, v in C['logEventTypes'].items()}
stype = {v: k for k, v in C['logSourceType'].items()}
off = float(C.get('timeTickOffset', 0))
by = {}   # source id -> [(ms, phase, type name, params)]
kind = {}
for line in text[cut:].splitlines()[1:]:
    line = line.strip().rstrip(',')
    if not line.startswith('{'): continue
    try: e = json.loads(line)
    except ValueError: continue   # the last line, half written
    s = e['source']
    by.setdefault(s['id'], []).append((float(e['time']), e.get('phase', 0), etype.get(e['type'], str(e['type'])), e.get('params') or {}))
    kind[s['id']] = stype.get(s['type'], str(s['type']))

def clock(ms): return datetime.datetime.fromtimestamp((ms + off) / 1000).strftime('%H:%M:%S.%f')[:-3]

reqs = []
for sid, ev in by.items():
    if kind[sid] != 'URL_REQUEST': continue
    url = next((p['url'] for _, _, n, p in ev if n == 'URL_REQUEST_START_JOB' and 'url' in p), None)
    if not url or not url.startswith('http://127.0.0.1:'): continue   # not Chrome's own calls to Google
    done = any(n == 'REQUEST_ALIVE' and ph == 2 for _, ph, n, _ in ev)
    reqs.append((ev[-1][0] - ev[0][0], done, sid, url, ev[0][0]))
reqs.sort(key=lambda r: r[4])
frame = [i for i, r in enumerate(reqs) if r[3].split('#')[0].endswith('/index.html?mock')]
if frame:
    print('the requests from the last frame of the main page on (started, took):')
    for took, done, sid, url, t in reqs[frame[-1]:frame[-1] + 40]:
        print(f'  {clock(t)}  ' + (f'{took:6.0f} ms' if done else ' never ended') + f'  {url[url.find("/", 7):]}')
slow = sorted((r for r in reqs if r[0] >= least or not r[1]), key=lambda r: (r[1], -r[0]))
print(f'{len(slow)} requests took {least:.0f} ms or more, or never ended; the slowest {min(most, len(slow))}:')
for took, done, sid, url, _ in slow[:most]:
    ev = list(by[sid])
    # the steps of the sources it waited on (the connection job, the socket), one level down
    for _, _, _, p in by[sid]:
        d = p.get('source_dependency')
        if d and d['id'] in by: ev += [(t, ph, kind[d['id']] + ':' + n, q) for t, ph, n, q in by[d['id']]]
    ev.sort(key=lambda x: x[0])
    t0 = ev[0][0]
    print(f'  {url}  started {clock(t0)}, ' + (f'{took:.0f} ms' if done else 'never ended'))
    shown, prev = 0, t0
    for t, ph, n, p in ev:
        if shown >= 30: print('      ...'); break
        # the steps that took time, and those that say where it waited
        if t - prev < 20 and not any(w in n for w in ('START_JOB', 'HTTP_CACHE', 'STALL', 'SOCKET_POOL', 'HTTP_STREAM_REQUEST', 'SEND_REQUEST', 'READ_HEADERS', 'FAILED', 'CANCEL', 'ALIVE')): continue
        extra = {k: v for k, v in p.items() if k in ('net_error', 'load_flags', 'is_preconnect', 'group_id', 'group_name', 'status')}
        print(f'    +{t - t0:7.0f} ms  {"BE"[ph - 1] if ph in (1, 2) else " "} {n}' + (f'  {extra}' if extra else ''))
        shown += 1; prev = t
