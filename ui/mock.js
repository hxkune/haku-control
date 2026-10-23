// SPDX-License-Identifier: GPL-3.0-only
// Design preview without the core (open index.html?mock): fakes the WebView2 bridge with sample state and frames.
const L = [];
// index.html?mock#tri / #hex: Nanoleaf Light Panels triangles / Shapes hexagons instead of Blocks. Positions are in
// Nanoleaf layout units (y up) and normalised the way dev_nanoleaf.c does it.
function mockLayout(shape, pts) {   // a point's own shape (4th value) wins over shape
  const xs = pts.map(p => p[0]), ys = pts.map(p => p[1]), x0 = Math.min(...xs), y0 = Math.min(...ys);
  const w = Math.max(...xs) - x0, h = Math.max(...ys) - y0, span = Math.max(w, h, 1);
  return { unit: 1 / span, panels: pts.map(([x, y, o, sh]) => [(x - x0 + (span - w) / 2) / span, 1 - (y - y0 + (span - h) / 2) / span, sh ?? shape, (720 - o) % 360]) };
}
const TRI_H = 150 * Math.sqrt(3) / 2;
const LAYOUTS = {
  tri: mockLayout(0, [...[0, 1, 2, 3, 4, 5].map(i => [75 * (i + 1), i % 2 ? 2 * TRI_H / 3 : TRI_H / 3, i % 2 ? 0 : 180]), [75, -TRI_H / 3, 0], [150, -2 * TRI_H / 3 + 0, 180]]),
  hex: mockLayout(7, [[0, 0, 0], [116, 0, 0], [232, 0, 0], [58, -100.5, 0], [174, -100.5, 0]]),
  // a real Blocks wall (NL81): squares (33) and pairs of mini squares (34), turned by its globalOrientation 179
  blocks: mockLayout(33, [[251, 301, 0], [251, 167, 0], [117, 100, 0], [251, 33, 0], [151, 0, 0, 34], [84, 0, 0, 34], [117, 301, 0], [151, 201, 0, 34], [84, 201, 0, 34]]
    .map(([x, y, o, sh]) => [-x, -y, o, sh])),
};
const emit = d => L.forEach(f => f({ data: d === STATE ? JSON.parse(JSON.stringify(d)) : d }));   // the state as a copy, as the core sends it
window.chrome = { webview: {
  postMessage(s) {
    const o = JSON.parse(s); console.log('->', o);
    if (o.cmd === 'hello') setTimeout(() => emit(STATE), 30);
    // profiles: kept in the fake settings only (the core keeps the setups)
    const C = STATE.cfg, G = C.general;
    if (o.cmd === 'set') (C[o.s] = C[o.s] || {})[o.k] = o.v;
    // devices.c load_config: an OpenRGB device's same=N pairs it with dev N, path= says which of the two lights it; the other lets go
    const pairUp = () => { const D = STATE.ext.devs; D.forEach(d => { if (d.held) d.online = 1; d.same = d.held = 0; });
      D.filter(d => d.kind === 'openrgb').forEach(m => { const c = C['dev.' + m.id] || {}, n = D.find(x => x.id === +c.same && x.kind !== 'openrgb' && !x.same); if (!n) return;
        m.same = n.id; n.same = m.id; const [use, h] = c.path === 'openrgb' ? [m, n] : [n, m]; if (use.enabled) { h.held = 1; h.online = 0; } }); };
    if (o.cmd === 'hello' || o.cmd === 'set' && /^dev\.\d+$/.test(o.s) && /^(same|path|own)$/.test(o.k)) {
      const d = o.k === 'own' && STATE.ext.devs.find(x => 'dev.' + x.id === o.s); if (d) d.own = +o.v;
      pairUp(); if (o.cmd === 'set') setTimeout(() => emit(STATE), 30);
    }
    if (o.cmd === 'fans_set') o.kv.split('\n').forEach(l => { const e = l.indexOf('='); (C.fans = C.fans || {})[l.slice(0, e)] = l.slice(e + 1); });
    // window.mockTryxFail = 'open' | 'write' | 'replace': the save fails as tryx_save does, the settings untouched
    if (o.cmd === 'tryx_build') { const no = window.mockTryxFail; if (!no) Object.assign(C['dev.' + o.id] = C['dev.' + o.id] || {}, { media: 'C:\\Users\\me\\AppData\\Roaming\\haku-control\\tryx-' + o.id + '.' + o.ext, theme: '-1' }); setTimeout(() => { emit(STATE); emit({ type: 'tryx_built', req: o.req, ok: no ? 0 : 1, err: no || '' }); }, 60); }
    // cf_close: a program with stay: 1 only hides in the tray (fail 1); cf_end ends it whatever it is
    if (o.cmd === 'cf_close' || o.cmd === 'cf_end') { const x = STATE.cf.a.find(a => a.id === o.id); STATE.cf.busy = o.id; emit(STATE);
      setTimeout(() => { STATE.cf.busy = ''; if (o.cmd === 'cf_close' && x.stay) x.fail = 1; else x.run = 0, x.fail = 0; emit(STATE); }, 600); }
    if (o.cmd === 'tryx_media') { Object.assign(C['dev.' + o.id] = C['dev.' + o.id] || {}, { media: 'C:\\Pictures\\cat.gif', theme: '-1' }); setTimeout(() => emit(STATE), 60); }
    // fans.c, roughly: a mode takes the channel and the QUADRO confirms it; '' gives it back (fans_set: all its keys are in first)
    const fanMode = (k, m) => { const f = /^fan\d+$/.test(k) && STATE.fans.f[+k.slice(3) - 1]; if (!f) return; const w = m === 'fix' ? +(C.fans[k + '_pct'] || 50) : m ? 45 : -1;
      Object.assign(f, { own: m ? 1 : 0, want: w, out: w, ok: 1, err: '' }); if (m) f.pct = w; };
    if (o.cmd === 'set' && o.s === 'fans') fanMode(o.k, o.v);
    if (o.cmd === 'fans_set') o.kv.split('\n').forEach(l => { const e = l.indexOf('='); fanMode(l.slice(0, e), l.slice(e + 1)); });
    if (o.cmd === 'effect') STATE.effect = G.effect = o.id;
    if (o.cmd === 'brightness') STATE.brightness = +o.v;
    if (o.cmd === 'get') setTimeout(() => emit(STATE), 30);
    if (o.cmd === 'diag') setTimeout(() => { STATE.diag = { busy: 0, ok: 1, file: 'haku-diagnostics-20261022-1200.txt', report: { busy: 0, ok: -1, id: '', err: '' } }; emit(STATE); }, 300);
    if (o.cmd === 'report') setTimeout(() => { STATE.diag = { busy: 0, ok: -1, file: '', report: { busy: 0, ok: o.text.includes('fail') ? 0 : 1, id: '3fa2c9d01e', err: o.text.includes('fail') ? 'offline' : '' } }; emit(STATE); }, 900);
    if (o.cmd === 'pc_own') { STATE.pc_own = STATE.msi_own = STATE.ram_own = +o.v; STATE.ext.devs.forEach(d => { if (['openrgb', 'wooting', 'nlusb', 'razer', 'steelseries', 'logitech', 'tryx', 'aura', 'lianli'].includes(d.kind)) d.own = +o.v; }); setTimeout(() => emit(STATE), 60); }
    if (o.cmd === 'dev_retry') setTimeout(() => emit(STATE), 60);   // the devices not connected try again (here: they all answer)
    if (o.cmd === 'dev_check') { STATE.netchk = { id: +o.id, busy: 1, res: '' }; emit(STATE); setTimeout(() => { STATE.netchk = { id: +o.id, busy: 0, res: o.id % 2 ? 'silent' : 'answers', ms: 0 }; emit(STATE); }, 1500); }
    if (o.cmd === 'profile') { G.profile = o.id; STATE.prof_why = 1; setTimeout(() => emit(STATE), 120); }
    if (o.cmd === 'profile_dup') { let n = 1; while (C['profile.' + n] && C['profile.' + n].name) n++; C['profile.' + n] = { name: o.name }; setTimeout(() => emit(STATE), 60); }
    if (o.cmd === 'profile_save') { let n = 1; while (C['profile.' + n] && C['profile.' + n].name) n++; C['profile.' + n] = { name: o.name }; G.profile = String(n); setTimeout(() => emit(STATE), 60); }
    if (o.cmd === 'profile_delete') { delete C['profile.' + o.id]; if (G.profile === o.id) G.profile = ''; setTimeout(() => emit(STATE), 60); }
  },
  addEventListener(t, f) { L.push(f); },
} };
const LAY = LAYOUTS[location.hash.slice(1)], MULTI = location.hash === '#multi';
const panels = (LAY || LAYOUTS.blocks).panels;
const STATE = {
  type: 'state', hw: { busy: 0, done: 1, items: [{ cat: 'board', maker: 'MSI', name: 'MAG B550 TOMAHAWK', id: '' }, { cat: 'ram', maker: 'Corsair', name: '2 × CMW16GX4M2C3200C16', id: '' },
    { cat: 'gpu', maker: 'ASUS', name: 'NVIDIA GeForce RTX 4070', id: '10DE:2786' }, { cat: 'usb', maker: 'Corsair', name: 'K70 RGB PRO', id: '1B1C:1BB3' },
    { cat: 'usb', maker: 'Wooting', name: 'Wooting 60HE v2', id: '31E3:1342' }, { cat: 'usb', maker: 'Lian Li', name: 'LianLi-Strimer Plus-1.5', id: '0CF2:A200' }] },
  orgb: { state: 2, exe: 1, auto: 1, ours: 1, setup: 0, pct: 0, err: '', ctls: [{ i: 0, type: 5, kind: 'keyboard', leds: 104, name: 'Keyboard' }, { i: 1, type: 3, kind: 'cooler', leds: 16, name: 'Fan hub' },
    { i: 2, type: 2, kind: 'graphics card', leds: 8, name: 'ASUS TUF RTX 4070' }, { i: 6, type: 1, kind: 'memory', leds: 8, name: 'ENE DRAM' }, { i: 7, type: 2, kind: 'graphics card', leds: 3, name: 'MSI RTX 3060 Ventus' }] }, autostart: 1, ollama: { exe: 0, model: 0, ours: 0, on_demand: 0, startup: 0, stage: 5, pct: 42, err: '' }, effect: 'flow', brightness: 85, msi: 1, sticks: 2, gpu_temp: 41, hotspot: 1, fans: { on: 1, q: 1, link: 1, age: 400, paused: 0, rfail: 0, gpu: 41, gage: 600, serial: '04976-37680', fw: 1024, flow: 0, t: [31.4, null, null, null], log: [], f: [{ rpm: 1180, pct: 52, v: 12, w: 1.4, own: 0, want: -1, out: -1, ok: 1, em: '', err: '' }, { rpm: 1210, pct: 52, v: 12, w: 1.5, own: 0, want: -1, out: -1, ok: 1, em: '', err: '' }, { rpm: 0, pct: 0, v: 0, w: 0, own: 0, want: -1, out: -1, ok: 1, em: '', err: '' }, { rpm: 0, pct: 0, v: 0, w: 0, own: 0, want: -1, out: -1, ok: 1, em: '', err: '' }] }, ll: { st: 'ok', err: 0, rep: 3, g: [{ mac: '7c:9c:06:f5:17:e1', own: 1, ch: 8, type: 0, n: 3, rpm: [1650, 1664, 1820], pct: [79, 79, 79] }, { mac: '3a:10:c4:22:9e:05', own: 0, ch: 8, type: 0, n: 2, rpm: [980, 1004], pct: [40, 40] }] }, cf: { busy: '', a: [{ id: 'armoury', name: 'ASUS Armoury Crate / Aura', w: 'board', cool: 1, run: 1, inst: 1, bad: 0, fail: 0 }, { id: 'icue', name: 'Corsair iCUE', w: 'corsair', cool: 1, run: 0, inst: 1, bad: 0, fail: 0 }, { id: 'asrock', name: 'ASRock Polychrome RGB', w: 'board', cool: 0, run: 1, inst: 1, bad: 0, fail: 0, stay: 1 }] }, mqtt: { state: 'connected', err: '', topic: 'haku/desk-pc' }, pro: { on: 1, state: 'trial', kind: '', days: 12, until: '', checked: '', stale: 0, key: '', who: '', err: '', busy: 0 }, remote: { enabled: 1, on: 1, port: 8723, pin: '481205', paired: 1, urls: ['http://172.20.10.4:8723', 'http://192.168.137.1:8723'] }, accounts: { aidot: { state: 0, msg: '', found: 0, countries: [['FR','France'],['DE','Germany'],['RU','Russia'],['US','United States']] } }, update: { version: '0.2.0', repo: 1, latest: '0.3.0', url: 'https://github.com/', can: 1, inst: 0, pct: 0, err: '' },
  effects: [['flow','Течение'],['caustic','Каустика'],['bubbles','Пузырьки'],['comet','Комета'],['lava','Лава'],['breathe','Дыхание'],['rainbow','Радуга'],['wave','Цветная волна'],['fire','Огонь'],['ocean','Океан'],['twinkle','Мерцание'],['meteor','Метеор'],['plasma','Плазма'],['aurora','Северное сияние'],['ripple','Капли'],['matrix','Матрица'],['candle','Свеча'],['screen','Экран'],['temperature','Температура'],['pump','Поток по насосу'],['audio','Звук'],['static','Статичный цвет'],['off','Выключить']].map(([id,title])=>({id,title})),
  bulbs: [{name:'Desk lamp',online:1,ip:'192.168.1.50'},{name:'Ceiling',online:1,ip:'192.168.1.51'},{name:'Bedside',online:0,ip:''}],
  // #multi: a second controller (Shapes hexagons) and a Secretlab MAGRGB strip next to the Blocks
  nano: { pair: 0, max: 8, ctls: [{ slot: 1, online: 1, ip: '192.168.1.40', name: 'Blocks 1A2B', model: 'NL81', side: .45, unit: (LAY || LAYOUTS.blocks).unit, stream: 0, panels }].concat(MULTI ? [
    { slot: 2, online: 1, ip: '192.168.1.41', name: 'Shapes 77C1', model: 'NL42', side: .3, unit: LAYOUTS.hex.unit, stream: 0, panels: LAYOUTS.hex.panels },
    { slot: 3, online: 1, ip: '192.168.1.42', name: 'Secretlab MAGRGB', model: 'NL72S2', side: .02, unit: 0, stream: 1, panels: Array.from({ length: 41 }, (_, i) => [(40 - i) / 40, .5, 0, 0]) },
  ] : []) },
  ext: {
    devs: [
      { id: 1, kind: 'wled', title: 'WLED', name: 'Desk strip', host: '192.168.1.60', sub: -1, leds: 60, per_led: 1, online: 1, enabled: 1, type: 'strip', info: 'WLED 0.14.4 · esp32' },
      { id: 2, kind: 'openrgb', title: 'OpenRGB', name: 'Keyboard', host: '127.0.0.1', sub: 0, leds: 104, per_led: 1, online: 1, enabled: 1, type: 'keyboard', info: 'OpenRGB · keyboard' },
      { id: 3, kind: 'hue', title: 'Philips Hue', name: 'Living room', host: '192.168.1.20', sub: -1, leds: 3, per_led: 0, online: 0, enabled: 1, type: 'bulb', info: 'Press the link button on the Hue bridge', ev: [[Date.now() / 1e3 - 900 | 0, 1, 0, 1], [Date.now() / 1e3 - 300 | 0, 4, 0, 1], [Date.now() / 1e3 - 60 | 0, 3, 0, 4]] },
      { id: 4, kind: 'govee', title: 'Govee', name: 'Govee H6076', host: '192.168.1.71', sub: -1, leds: 1, per_led: 0, online: 1, enabled: 1, type: 'floor', info: 'Govee H6076' },
      { id: 5, kind: 'goveecloud', title: 'Govee (cloud)', name: 'AI Sync Box 2', host: 'AA:BB:CC:DD:EE:FF:00:01', sub: -1, leds: 1, per_led: 0, online: 1, enabled: 0, type: 'tv', info: 'Govee H6604 · cloud · screen sync' },
      { id: 6, kind: 'openrgb', title: 'OpenRGB', name: 'ASUS TUF RTX 4070', host: '127.0.0.1', sub: 2, leds: 8, per_led: 1, online: 1, enabled: 1, type: 'gpu', info: 'OpenRGB · ASUS TUF RTX 4070 · graphics card' },
      { id: 7, kind: 'openrgb', title: 'OpenRGB', name: 'Vengeance RGB', host: '127.0.0.1', sub: 3, leds: 10, per_led: 1, online: 1, enabled: 1, type: 'ram', info: 'OpenRGB · Corsair Vengeance RGB Pro · memory' },
      { id: 8, kind: 'openrgb', title: 'OpenRGB', name: 'B550-F', host: '127.0.0.1', sub: 4, leds: 12, per_led: 1, online: 1, enabled: 1, type: 'board', info: 'OpenRGB · ASUS ROG STRIX B550-F GAMING · motherboard', zones: 'Addressable 1:24:0:1024|Addressable 2:0:0:1024' },
      { id: 9, kind: 'openrgb', title: 'OpenRGB', name: 'Front fans', host: '127.0.0.1', sub: 1, leds: 16, per_led: 1, online: 1, enabled: 1, type: 'fan', info: 'OpenRGB · Fan hub · cooler' },
      { id: 10, kind: 'openrgb', title: 'OpenRGB', name: 'Mouse', host: '127.0.0.1', sub: 5, leds: 3, per_led: 1, online: 1, enabled: 1, type: 'mouse', info: 'OpenRGB · Mouse · mouse' },
      { id: 11, kind: 'elgato', title: 'Elgato', name: 'Key Light Air', host: '192.168.1.80', sub: -1, leds: 1, per_led: 0, online: 1, enabled: 1, type: 'keylight', info: 'Elgato Key Light Air · fw 1.0.3' },
      { id: 12, kind: 'divoom', title: 'Divoom', name: 'Times Gate', host: '300183039', sub: -1, leds: 1, per_led: 0, online: 1, enabled: 1, type: 'gate', info: 'Divoom · hardware 400 · lights' },
      { id: 13, kind: 'divoom', title: 'Divoom', name: 'TimesFrame', host: '300256986', sub: -1, leds: 1, per_led: 0, online: 1, enabled: 1, type: 'frame', info: 'Times Frame · lights' },
    ],
    found: [
      { kind: 'wled', title: 'WLED', host: '192.168.1.60', sub: -1, name: 'Desk strip', leds: 60, info: 'WLED 0.14.4', added: 1 },
      { kind: 'govee', title: 'Govee', host: '192.168.1.71', sub: -1, name: 'Govee H6076', leds: 1, info: '', added: 0 },
      { kind: 'openrgb', title: 'OpenRGB', host: '127.0.0.1', sub: 1, name: 'Corsair Lighting Node', leds: 48, info: 'OpenRGB · cooler', added: 0 },
    ],
    scanning: 0,
    kinds: ['wled:WLED:1', 'openrgb:OpenRGB:1', 'govee:Govee:0', 'lifx:LIFX:0', 'yeelight:Yeelight:0', 'hue:Philips Hue:0', 'wiz:WiZ:0'].map(x => { const [kind, title, p] = x.split(':'); return { kind, title, per_led: +p }; }),
  },
  cfg: { remote: { enabled: '1' }, general: { palette: '#00C8FF, #7A3CFF, #FF2D95', speed: '5', fps: '30', profile: '1' }, 'profile.1': { name: 'Gaming', hotkey: 'Ctrl+Alt+1' }, 'dev.11': { kind: 'elgato', caps: 'white', kmin: '2900', kmax: '7000' }, 'zone.dev11': { mode: 'white', kelvin: '4500' }, 'dev.13': { kind: 'divoom', screen: 'dial', clock: '1044', clocks: '923:Pixel Cat|1044:Cyber Calendar' }, 'profile.2': { name: 'Work' }, 'profile.3': { name: 'Night' }, layout: { gpu_leds: '8', board_led: '1', ram_enabled: '1', gpu_enabled: '1', lights_enabled: '1', nanoleaf_enabled: '1' },
    caustic: { palette: '#00E5FF, #0060FF, #00FFB0' }, bubbles: { palette: '#001830, #00E5FF, #FFFFFF' }, comet: { palette: '#FFFFFF, #00C8FF, #7A3CFF' },
    lava: { palette: '#FF2D00, #FF9000, #B0006A', speed: '3' }, breathe: { palette: '#00C8FF, #FF2D95', speed: '3' },
    temperature: { palette: '#0050FF, #00FF80, #FFB000, #FF0020', cold: '35', hot: '75' }, static: { palette: '#7A3CFF' },
    'zone.light1': { mode: 'white', kelvin: '3200', brightness: '70' }, 'zone.gpu': { mode: 'palette', palette: '#00FFD5, #0068FF', effect: 'lava' }, 'zone.nanoleaf': { effect: 'breathe' },
    'preset.1': { name: 'Evening', effect: 'lava', palette: '#FF3300, #FF8800, #CC1100', speed: '3', brightness: '60', zones: '1' },
    'preset.2': { name: 'Cyberpunk', effect: 'comet', palette: '#00E5FF, #FF00A8, #7B2CFF', speed: '7' },
    hotkeys: { next: 'Ctrl+Alt+Right', prev: 'Ctrl+Alt+Left', off: 'Ctrl+Alt+Down', brighter: 'Ctrl+Alt+PageUp', dimmer: 'Ctrl+Alt+PageDown' } },
};
// link.c: the iCUE LINK hub of a real report (psuprobe, 2026-10-17), shortened
STATE.link = { st: 'ok', found: 1, app: '', fw: '3.10.636', hw: 0, age: 4000, hub_t: 25.6, v: [3.31, 5.05, 12.03], a: [1.75, 5.5, 10.25],
  dev: [{ ch: 1, type: 15, model: 0, rpm: 311, t: null }, { ch: 17, type: 6, model: 0, rpm: null, t: null }, { ch: 20, type: 17, model: 2, rpm: 2277, t: 35.2 }, { ch: 25, type: 11, model: 0, rpm: 457, t: 40.5 }] };
const pal = [[0,200,255],[122,60,255],[255,45,149]];
const pc = p => { p = (p % 1 + 1) % 1 * 3; const i = Math.floor(p), f = p - i, a = pal[i % 3], b = pal[(i + 1) % 3]; return a.map((v, k) => Math.round(v + (b[k] - v) * f)); };
const hx = c => c.map(v => v.toString(16).padStart(2, '0')).join('');
setInterval(() => {
  const t = performance.now() / 1000, l = [];
  for (let s = 0; s < 2; s++) for (let i = 0; i < 8; i++) l.push([s, i, hx(pc((s * 8 + i) / 36 - t * .12))]);
  for (let i = 0; i < 8; i++) l.push([2, i, hx(pc((16 + i) / 36 - t * .12))]);
  for (let i = 0; i < 3; i++) l.push([4, i, i === 0 ? 'ffd0a0' : hx(pc(i / 3 - t * .12))]);
  STATE.nano.ctls.forEach(c => c.panels.forEach((p, i) => l.push([200 + c.slot - 1, i, hx(pc(p[1] * .6 + p[0] * .3 + .4 - t * .12 + c.slot * .3))])));
  l.push([3, 0, hx(pc(.5 - t * .12))]);
  for (let i = 0; i < 60; i++) l.push([100, i, hx(pc(i / 60 - t * .12))]);
  for (let i = 0; i < 52; i++) l.push([101, i, hx(pc(i / 52 + .3 - t * .12))]);
  for (let i = 0; i < 3; i++) l.push([102, i, hx(pc(i / 3 - t * .12))]);
  l.push([103, 0, hx(pc(.6 - t * .12))]);
  [[105, 8], [106, 10], [107, 12], [108, 16], [109, 3]].forEach(([k, n], j) => { for (let i = 0; i < n; i++) l.push([k, i, hx(pc(i / n + j * .2 - t * .12))]); });   // PC hardware through OpenRGB
  l.push([110, 0, 'ffd9b0']);   // the Key Light: warm white
  emit({ type: 'frame', l });
}, 50);
