// Design preview without the core (open index.html?mock): fakes the WebView2 bridge with sample state and frames.
const L = [];
// index.html?mock#tri / #hex: Nanoleaf Light Panels triangles / Shapes hexagons instead of Blocks. Positions are in
// Nanoleaf layout units (y up) and normalised the way dev_nanoleaf.c does it.
function mockLayout(shape, pts) {
  const xs = pts.map(p => p[0]), ys = pts.map(p => p[1]), x0 = Math.min(...xs), y0 = Math.min(...ys);
  const w = Math.max(...xs) - x0, h = Math.max(...ys) - y0, span = Math.max(w, h, 1);
  return { unit: 1 / span, panels: pts.map(([x, y, o]) => [(x - x0 + (span - w) / 2) / span, 1 - (y - y0 + (span - h) / 2) / span, shape, (720 - o) % 360]) };
}
const TRI_H = 150 * Math.sqrt(3) / 2;
const LAYOUTS = {
  tri: mockLayout(0, [...[0, 1, 2, 3, 4, 5].map(i => [75 * (i + 1), i % 2 ? 2 * TRI_H / 3 : TRI_H / 3, i % 2 ? 0 : 180]), [75, -TRI_H / 3, 0], [150, -2 * TRI_H / 3 + 0, 180]]),
  hex: mockLayout(7, [[0, 0, 0], [116, 0, 0], [232, 0, 0], [58, -100.5, 0], [174, -100.5, 0]]),
};
const emit = d => L.forEach(f => f({ data: d }));
window.chrome = { webview: {
  postMessage(s) { const o = JSON.parse(s); console.log('->', o); if (o.cmd === 'hello') setTimeout(() => emit(STATE), 30); },
  addEventListener(t, f) { L.push(f); },
} };
const LAY = LAYOUTS[location.hash.slice(1)];
const panels = LAY ? LAY.panels : [[.83,.12,33],[.83,.5,33],[.83,.88,33],[.45,.69,33],[.45,.12,33],[.54,.99,34],[.36,.99,34],[.54,.44,34],[.36,.44,34]];
const STATE = {
  type: 'state', autostart: 1, effect: 'flow', brightness: 85, msi: 1, sticks: 2, gpu_temp: 41, hotspot: 1, remote: { enabled: 1, on: 1, port: 8723, pin: '481205', paired: 1, urls: ['http://172.20.10.4:8723', 'http://192.168.137.1:8723'] }, update: { version: '0.2.0', repo: 1, latest: '0.3.0', url: 'https://github.com/' },
  effects: [['flow','Течение'],['caustic','Каустика'],['bubbles','Пузырьки'],['comet','Комета'],['lava','Лава'],['breathe','Дыхание'],['temperature','Температура'],['pump','Поток по насосу'],['audio','Звук'],['static','Статичный цвет'],['off','Выключить']].map(([id,title])=>({id,title})),
  bulbs: [{name:'Desk lamp',online:1,ip:'192.168.1.50'},{name:'Ceiling',online:1,ip:'192.168.1.51'},{name:'Bedside',online:0,ip:''}],
  nano: { configured: 1, online: 1, ip: '192.168.1.40', name: 'Blocks 1A2B', side: .45, unit: LAY ? LAY.unit : 0, pair: 0, panels },
  ext: {
    devs: [
      { id: 1, kind: 'wled', title: 'WLED', name: 'Desk strip', host: '192.168.1.60', sub: -1, leds: 60, per_led: 1, online: 1, enabled: 1, info: 'WLED 0.14.4 · esp32' },
      { id: 2, kind: 'openrgb', title: 'OpenRGB', name: 'Keyboard', host: '127.0.0.1', sub: 0, leds: 104, per_led: 1, online: 1, enabled: 1, info: 'OpenRGB · keyboard' },
      { id: 3, kind: 'hue', title: 'Philips Hue', name: 'Living room', host: '192.168.1.20', sub: -1, leds: 3, per_led: 0, online: 0, enabled: 1, info: 'Press the link button on the Hue bridge' },
    ],
    found: [
      { kind: 'wled', title: 'WLED', host: '192.168.1.60', sub: -1, name: 'Desk strip', leds: 60, info: 'WLED 0.14.4', added: 1 },
      { kind: 'govee', title: 'Govee', host: '192.168.1.71', sub: -1, name: 'Govee H6076', leds: 1, info: '', added: 0 },
      { kind: 'openrgb', title: 'OpenRGB', host: '127.0.0.1', sub: 1, name: 'Corsair Lighting Node', leds: 48, info: 'OpenRGB · cooler', added: 0 },
    ],
    scanning: 0,
    kinds: ['wled:WLED:1', 'openrgb:OpenRGB:1', 'govee:Govee:0', 'lifx:LIFX:0', 'yeelight:Yeelight:0', 'hue:Philips Hue:0'].map(x => { const [kind, title, p] = x.split(':'); return { kind, title, per_led: +p }; }),
  },
  cfg: { remote: { enabled: '1' }, general: { palette: '#00C8FF, #7A3CFF, #FF2D95', speed: '5', fps: '30' }, layout: { gpu_leds: '8', board_led: '1', ram_enabled: '1', gpu_enabled: '1', lights_enabled: '1', nanoleaf_enabled: '1' },
    caustic: { palette: '#00E5FF, #0060FF, #00FFB0' }, bubbles: { palette: '#001830, #00E5FF, #FFFFFF' }, comet: { palette: '#FFFFFF, #00C8FF, #7A3CFF' },
    lava: { palette: '#FF2D00, #FF9000, #B0006A', speed: '3' }, breathe: { palette: '#00C8FF, #FF2D95', speed: '3' },
    temperature: { palette: '#0050FF, #00FF80, #FFB000, #FF0020', cold: '35', hot: '75' }, static: { palette: '#7A3CFF' },
    'zone.light1': { mode: 'white', kelvin: '3200', brightness: '70' }, 'zone.gpu': { mode: 'palette', palette: '#00FFD5, #0068FF', effect: 'lava' }, 'zone.nanoleaf': { effect: 'breathe' },
    hotkeys: { next: 'Ctrl+Alt+Right', prev: 'Ctrl+Alt+Left', off: 'Ctrl+Alt+Down', brighter: 'Ctrl+Alt+PageUp', dimmer: 'Ctrl+Alt+PageDown' } },
};
const pal = [[0,200,255],[122,60,255],[255,45,149]];
const pc = p => { p = (p % 1 + 1) % 1 * 3; const i = Math.floor(p), f = p - i, a = pal[i % 3], b = pal[(i + 1) % 3]; return a.map((v, k) => Math.round(v + (b[k] - v) * f)); };
const hx = c => c.map(v => v.toString(16).padStart(2, '0')).join('');
setInterval(() => {
  const t = performance.now() / 1000, l = [];
  for (let s = 0; s < 2; s++) for (let i = 0; i < 8; i++) l.push([s, i, hx(pc((s * 8 + i) / 36 - t * .12))]);
  for (let i = 0; i < 8; i++) l.push([2, i, hx(pc((16 + i) / 36 - t * .12))]);
  for (let i = 0; i < 3; i++) l.push([4, i, i === 0 ? 'ffd0a0' : hx(pc(i / 3 - t * .12))]);
  panels.forEach((p, i) => l.push([5, i, hx(pc(p[1] * .6 + .4 - t * .12))]));
  l.push([3, 0, hx(pc(.5 - t * .12))]);
  for (let i = 0; i < 60; i++) l.push([100, i, hx(pc(i / 60 - t * .12))]);
  for (let i = 0; i < 52; i++) l.push([101, i, hx(pc(i / 52 + .3 - t * .12))]);
  for (let i = 0; i < 3; i++) l.push([102, i, hx(pc(i / 3 - t * .12))]);
  emit({ type: 'frame', l });
}, 50);
