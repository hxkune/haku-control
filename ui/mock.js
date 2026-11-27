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
// the state as a copy, as the core sends it; a device that is lit had a frame leave a moment ago (devices.c "sent"),
// unless a test sets its own
// and has been sent about 30 frames a second (devices.c ns) without a failed write (nf)
const sentOf = x => (x.ext || { devs: [] }).devs.forEach(v => { const lit = v.online && v.enabled && !v.own && !v.held && !v.covered;
  if (v.sent === undefined) v.sent = lit ? 120 : -1; if (v.ns === undefined) v.ns = lit ? Math.round(performance.now() * 0.03) : 0; if (v.nf === undefined) v.nf = 0; });
let VLVL = 0;   // the made-up microphone level while the logo is held
const emit = d => L.forEach(f => { const c = d === STATE ? JSON.parse(JSON.stringify(d)) : d; if (d === STATE) sentOf(c); f({ data: c }); });
window.chrome = { webview: {
  postMessage(s) {
    const o = JSON.parse(s); console.log('->', o);
    if (o.cmd === 'hello') setTimeout(() => emit(STATE), 30);
    if (o.cmd === 'frames') { const was = FRAMES.live; FRAMES.live = o.live === 1; if (FRAMES.live && !was) FRAMES.emit(); }   // ui_web.cpp: at once
    // profiles: kept in the fake settings only (the core keeps the setups)
    const C = STATE.cfg, G = C.general;
    // ui_web.cpp other_profile: a profile's setting sent for a profile no longer in use is dropped, the page gets the state
    const profSec = s => s === 'custom' || s === 'general' || STATE.effects.some(e => e.id === s);
    if (o.cmd === 'set' && o.p && o.p !== (G.profile || '') && profSec(o.s)) { setTimeout(() => emit(STATE), 30); return; }
    // main.c track_stored / comp_tracks_ok: without Pro a keyframe track is written only if its words (after the layer
    // number) are already stored in [custom], a scene or a group
    const body = v => String(v || '').trim().replace(/^\S+\s*/, '');
    const tracksOk = kv => STATE.pro.on || kv.every(([k, v]) => !/^t[1-8]$/i.test(k) || !body(v) ||
      ['custom'].concat([1, 2, 3, 4, 5, 6, 7, 8].map(n => 'cscene.' + n), Array.from({ length: 16 }, (_, i) => 'cgroup.' + (i + 1)))
        .some(s => [1, 2, 3, 4, 5, 6, 7, 8].some(i => C[s] && C[s]['t' + i] != null && body(C[s]['t' + i]) === body(v))));
    // ui_web.cpp set: scenarios, LCD layouts, scenes and groups only through their own commands; a new track in [custom]: Pro
    if (o.cmd === 'set' && (/^(rule|lcd|cscene|cgroup)\./i.test(o.s) || o.s === 'custom' && !tracksOk([[o.k, o.v]]))) { setTimeout(() => emit(STATE), 30); return; }
    if (o.cmd === 'set') (C[o.s] = C[o.s] || {})[o.k] = o.v;
    // devices.c load_config: a device's name is [dev.N] name
    if (o.cmd === 'set' && /^dev\.\d+$/.test(o.s) && o.k === 'name') { const d = STATE.ext.devs.find(x => 'dev.' + x.id === o.s); if (d) d.name = o.v; }
    // pro/wallpaper.cpp: 6 pictures in the lights' colours (SVG stand-ins), one set as the wallpaper, the old one kept
    if (/^wall_/.test(o.cmd)) { const W = STATE.walls, hues = [0, 210, 280, 330, 190, 20];
      const pic = (h, k) => 'data:image/svg+xml,' + encodeURIComponent(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 9"><defs><linearGradient id="g" x2="1" y2="1"><stop stop-color="hsl(${h},80%,${30 + k * 4}%)"/><stop offset="1" stop-color="hsl(${(h + 40) % 360},70%,12%)"/></linearGradient></defs><rect width="16" height="9" fill="url(#g)"/></svg>`);
      if (o.cmd === 'wall_find' || o.cmd === 'wall_more') { W.busy = 1; setTimeout(() => { W.busy = 0; W.seq++; W.err = '';
        W.items = hues.map((h, k) => ({ src: k % 2 ? 'wh' : 'pc', name: k % 2 ? 'wallhaven-' + (W.seq * 10 + k) : 'IMG_' + (2400 + W.seq * 10 + k) + '.jpg', w: 2560, h: 1440, thumb: pic((h + W.seq * 25) % 360, k) }));
        W.cur = -1; emit(STATE); }, 600); }
      if (o.cmd === 'wall_set' && o.k >= 0 && o.k < W.items.length) { W.busy = 2; W.setting = o.k; setTimeout(() => { W.busy = 0; W.cur = o.k; W.setting = -1; W.saved = 1; emit(STATE); }, 500); }
      if (o.cmd === 'wall_restore' && W.saved) { W.busy = 2; setTimeout(() => { W.busy = 0; W.cur = -1; emit(STATE); }, 400); }
      if (o.cmd === 'wall_dir') W.dir = 'D:\Pictures\Wallpapers';
      if (o.cmd === 'wall_dir_clear') W.dir = '';
      setTimeout(() => emit(STATE), 30); }
    if (o.cmd === 'set' && o.s === 'wall' && o.k === 'web') STATE.walls.web = +o.v;
    // pro/voice.c voice_hold: the logo held down listens (a made-up microphone level for the waves), let go it thinks, then
    // shows the command it ran
    if (o.cmd === 'voice_hold' && STATE.voice) { const V = STATE.voice; clearInterval(VLVL);
      if (o.on && V.ready && V.state !== 2) { V.mic = 1; V.state = 1; VLVL = setInterval(() => emit({ type: 'vlevel', l: Math.round(20 + 70 * Math.random()) }), 50); }
      else if (!o.on && V.state === 1) { V.state = 2; setTimeout(() => { Object.assign(V, { state: 0, mic: 0, heard: 'lights off', did: 'off', arg: '', seq: V.seq + 1 }); emit(STATE); }, 700); }
      setTimeout(() => emit(STATE), 30); }
    // config.c full (window.mockCfgFull = 1): what adds lines is refused whole and counted in cfg_full (comp_apply, draft_save,
    // preset_save and preset_copy here); the page is told by the reply's err 'cap' or by the count
    const full = window.mockCfgFull && ['comp_apply', 'draft_save', 'preset_save', 'preset_copy'].includes(o.cmd);
    if (full) { STATE.cfg_full++; if (o.cmd.startsWith('preset')) { setTimeout(() => emit(STATE), 30); return; } }
    // main.c app_comp_apply: the 20 lines (l1..8, k1..3, t1..8, sc) into [custom] at once; window.mockCompFail = 'bad' | 'save' | 'silent'
    if (o.cmd === 'comp_apply') { const no = window.mockCompFail, kv = String(o.kv).split('\n').map(l => [l.slice(0, l.indexOf('=')), l.slice(l.indexOf('=') + 1)]);
      const keys = Array.from({ length: 8 }, (_, i) => 'l' + (i + 1)).concat(['k1', 'k2', 'k3'], Array.from({ length: 8 }, (_, i) => 't' + (i + 1)), ['sc']);
      const shape = kv.length === 20 && kv.every((x, i) => x[0] === keys[i]);
      const err = !shape || no === 'bad' ? 'bad' : o.p && o.p !== (G.profile || '') ? 'prof' : !tracksOk(kv) ? 'pro' : full ? 'cap' : no === 'save' ? 'save' : '';
      if (no !== 'silent') { if (!err || err === 'save') o.kv.split('\n').forEach(l => { const e = l.indexOf('='); (C.custom = C.custom || {})[l.slice(0, e)] = l.slice(e + 1); });
        setTimeout(() => { emit(STATE); emit({ type: 'comp_done', req: o.req, err }); }, 40); } }
    // ui_web.cpp draft_save: the 22 lines in order into the first free [cdraft.N] or, slot r, [crecover] (no lines: cleared);
    // window.mockDraftFail = 'save' | 'silent'; a slot not taken back when not written
    if (o.cmd === 'draft_save') { const no = window.mockDraftFail, K = ['name', 'at', 'prof', 'l1', 'l2', 'l3', 'l4', 'l5', 'l6', 'l7', 'l8', 'k1', 'k2', 'k3', 't1', 't2', 't3', 't4', 't5', 't6', 't7', 't8'];
      const L = o.kv ? o.kv.split('\n').map(l => [l.slice(0, l.indexOf('=')), l.slice(l.indexOf('=') + 1)]) : [];
      let err = (o.slot === '' || o.slot === 'r') && (L.length === K.length ? L.every((x, i) => x[0] === K[i]) && L[1][1] : !L.length && o.slot === 'r') ? '' : 'bad', n = 0, sec = 'crecover';
      if (!err && o.slot === '') { n = 1; while (n <= 16 && (C['cdraft.' + n] || {}).at) n++; sec = 'cdraft.' + n; if (n > 16) err = 'full'; }
      if (!err && full) err = 'cap';
      if (!err && no === 'save') err = 'save';
      if (no === 'silent') return;
      if (err) n = 0; else { delete C[sec]; if (L.length) C[sec] = Object.fromEntries(L); }
      setTimeout(() => { emit(STATE); emit({ type: 'draft_done', req: o.req, err, n }); }, 40); }
    // pro/rules.c: rule_save / rule_on / rule_delete / rule_move on [rule.1..16], the number is the order; checked as the core does
    // (Pro for saving, moving and switching on; switching off and deleting work without it); window.mockRuleFail = 'bad' | 'save' | 'silent'
    if (/^rule_(save|on|delete|move)$/.test(o.cmd)) {
      const RK = ['name', 'on', 'mode', 'restore', 'c1', 'c2', 'c3', 'c4', 'c5', 'c6', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7', 'a8'];
      const sec = i => 'rule.' + i, has = i => !!(C[sec(i)] || {}).name, no = window.mockRuleFail, pro = STATE.pro.on;
      let last = 0; for (let i = 1; i <= 16; i++) if (has(i)) last = i;
      const n = +o.n || 0, num = (v, a, b) => /^\d+$/.test(v) && +v >= a && +v <= b, hm = v => /^([01]\d|2[0-3]):[0-5]\d$/.test(v);
      const cond = v => { const w = v.trim().split(/\s+/);
        if (w[0] === 'run' || w[0] === 'fg') { const a = w.slice(1).join(' ').split(',').map(x => x.trim()).filter(Boolean); return a.length >= 1 && a.length <= 8; }
        if (w[0] === 'time') return w.length <= 4 && hm(w[1]) && hm(w[2]) && (!w[3] || /^[1-7]{1,7}$/.test(w[3]));
        if (w[0] === 'idle') return w.length === 2 && num(w[1], 1, 1440);
        if (w[0] === 'lock') return w.length === 2 && /^[01]$/.test(w[1]);
        return w[0] === 'full' && w.length === 1; };
      const step = v => { const w = v.trim().split(/\s+/);
        return w[0] === 'profile' ? w.length === 2 && num(w[1], 0, 16) : w[0] === 'bright' ? w.length === 2 && num(w[1], 5, 100)
          : w[0] === 'fade' ? w.length === 3 && num(w[1], 5, 100) && num(w[2], 1, 3600) : w[0] === 'wait' && w.length === 2 && num(w[1], 1, 3600); };
      let err = '', at = 0;
      if (o.cmd === 'rule_save') {
        const L = (o.kv || '').split('\n').filter(Boolean).map(l => [l.slice(0, l.indexOf('=')), l.slice(l.indexOf('=') + 1)]), v = {};
        L.forEach(([k, x]) => { if (!RK.includes(k)) err = 'bad'; v[k] = x; });
        const cs = RK.slice(4, 10).map(k => v[k] || '').filter(Boolean), as = RK.slice(10).map(k => v[k] || '').filter(Boolean);
        if (!pro) err = 'pro';
        else if (err || !(v.name || '').trim() || v.name.length > 64 || !cs.length || !as.length || !cs.every(cond) || !as.every(step)) err = 'bad';
        else { at = n || last + 1; if (at > 16) err = 'full'; else if (at > last + 1) at = last + 1; }
        if (!err && no === 'save') err = 'save';
        if (!err) C[sec(at)] = Object.fromEntries(RK.map(k => [k, k === 'on' ? (v.on === '0' ? '0' : '1') : k === 'mode' ? (v.mode === 'any' ? 'any' : 'all')
          : k === 'restore' ? (v.restore === '1' ? '1' : '0') : k[0] === 'c' ? cs[+k[1] - 1] || '' : k[0] === 'a' ? as[+k[1] - 1] || '' : v.name.trim()]));
      } else if (o.cmd === 'rule_on') {
        if (!has(n)) err = 'bad'; else if (o.on === '1' && !pro) err = 'pro'; else if (no === 'save') err = 'save'; else { C[sec(n)].on = o.on === '1' ? '1' : '0'; at = n; }
      } else if (o.cmd === 'rule_delete') {
        if (n < 1 || n > last) err = 'bad'; else if (no === 'save') err = 'save';
        else { for (let i = n; i < last; i++) C[sec(i)] = C[sec(i + 1)]; delete C[sec(last)]; at = n; }
      } else {
        const m = n + (+o.d < 0 ? -1 : 1);
        if (!pro) err = 'pro'; else if (!has(n) || !has(m)) err = 'bad'; else if (no === 'save') err = 'save';
        else { [C[sec(n)], C[sec(m)]] = [C[sec(m)], C[sec(n)]]; at = m; }
      }
      if (no === 'silent') return;
      STATE.rules.on = pro && G.auto_pause !== '1' ? 1 : 0;
      setTimeout(() => { emit(STATE); emit({ type: 'rule_done', req: o.req, err, n: err ? 0 : at }); }, 40);
    }
    // main.c comp_store_*: scene_save / group_save (Pro, at least one layer), scene_delete / group_delete (free, later ones move up),
    // scene_move (Pro) on [cscene.1..8] / [cgroup.1..16]; window.mockSceneFail = 'save' | 'silent'
    if (/^(scene|group)_(save|delete|move)$/.test(o.cmd) && o.cmd !== 'group_move') {
      const scene = o.cmd[0] === 's', pre = scene ? 'cscene.' : 'cgroup.', max = scene ? 8 : 16, no = window.mockSceneFail, pro = STATE.pro.on;
      const sec = i => pre + i, has = i => Object.keys(C[sec(i)] || {}).length > 0, n = +o.n || 0;
      let cnt = 0; while (cnt < max && has(cnt + 1)) cnt++;
      const KK = ['dur', 'fade', 'nm', ...Array.from({ length: 8 }, (_, i) => 'l' + (i + 1)), ...Array.from({ length: 8 }, (_, i) => 't' + (i + 1))].slice(scene ? 0 : 2);
      let err = '', at = 0;
      if (/_save$/.test(o.cmd)) {
        const L = (o.kv || '').split('\n').filter(Boolean).map(l => [l.slice(0, l.indexOf('=')), l.slice(l.indexOf('=') + 1)]);
        if (L.length > 19 || n < 0 || n > cnt || L.some(([k]) => !KK.includes(k.toLowerCase())) || !L.some(([k, v]) => k[0].toLowerCase() === 'l' && v)) err = 'bad';
        else if (!pro) err = 'pro';
        else { at = n || cnt + 1; if (at > max) err = 'full'; }
        if (!err) { const keep = Object.entries(C[sec(at)] || {}).filter(([k]) => !KK.includes(k.toLowerCase()));   // a newer version's keys
          C[sec(at)] = Object.fromEntries(keep.concat(L.filter(([, v]) => v))); }
      } else if (/_delete$/.test(o.cmd)) {
        if (n < 1 || n > cnt) err = 'bad'; else { for (let i = n; i < cnt; i++) C[sec(i)] = C[sec(i + 1)]; delete C[sec(cnt)]; at = n; }
      } else {
        const m = n + (+o.d < 0 ? -1 : 1);
        if (n < 1 || n > cnt || m < 1 || m > cnt) err = 'bad'; else if (!pro) err = 'pro'; else { [C[sec(n)], C[sec(m)]] = [C[sec(m)], C[sec(n)]]; at = m; }
      }
      if (no === 'silent') return;
      if (!err && no === 'save') err = 'save';   // in memory, not on disk yet
      setTimeout(() => { emit(STATE); emit({ type: (scene ? 'scene' : 'group') + '_done', req: o.req, err, n: err ? 0 : at }); }, 40);
    }
    // pro/lcdlive.cpp: lcd_save (Pro) / lcd_delete on [lcd.1..6], checked as the core does; window.mockLcdFail = 'bad' | 'save' | 'silent'
    // lcd_watch: the values the core sends while the editor is open (S.lcd); window.mockLcd replaces them
    if (o.cmd === 'lcd_watch') { STATE.lcd = window.mockLcd || { cpu_t: 54.5, gpu_t: 61, cpu_l: 23, gpu_l: 41, mem: 47, fan: 1180, music: 1, title: 'Weightless', artist: 'Marconi Union' }; setTimeout(() => emit(STATE), 30); }
    if (o.cmd === 'lcd_save' || o.cmd === 'lcd_delete') {
      const LK = ['name', 'v', 'pages', 't1', 't2', 't3', 't4', 'bg1', 'bg2', 'bg3', 'bg4', ...Array.from({ length: 24 }, (_, i) => 'w' + (i + 1))];
      const sec = i => 'lcd.' + i, no = window.mockLcdFail, n = +o.n || 0, hex = x => /^[0-9a-f]{6}$/i.test(x), num = (x, a, b) => /^\d{1,6}$/.test(x) && +x >= a && +x <= b;
      let last = 0; for (let i = 1; i <= 6; i++) if ((C[sec(i)] || {}).name) last = i;
      let err = '', at = 0;
      if (o.cmd === 'lcd_save') {
        const v = {};
        (o.kv || '').split('\n').filter(Boolean).forEach(l => { const k = l.slice(0, l.indexOf('=')), x = l.slice(l.indexOf('=') + 1); if (!LK.includes(k) || x.length >= 256) err = 'bad'; v[k] = x; });
        const name = (v.name || '').trim(), pages = v.pages ? +v.pages : 1;
        const wid = s => { const m = /^\s*(\d+) (\S+) (\S+) (\d+) (\d+) (\d+) (\d+) (\S+) (\d+) (\S)(?: (.*))?$/.exec(s); if (!m) return false;
          const [, p, src, kind, x, y, w, h, col, size, unit] = m;
          return +p >= 1 && +p <= pages && ['time', 'date', 'cpu_t', 'gpu_t', 'cpu_l', 'gpu_l', 'mem', 'fan', 'music'].includes(src) && ['text', 'bar', 'ring', 'graph'].includes(kind)
            && (!['time', 'date', 'music'].includes(src) || kind === 'text') && +x < 480 && +y < 480 && +w >= 8 && +w <= 480 && +h >= 8 && +h <= 480 && +size >= 8 && +size <= 200
            && hex(col) && (unit === '-' || 'cf'.includes(unit) && (src === 'cpu_t' || src === 'gpu_t')); };
        const ws = LK.slice(11).map(k => v[k] || '').filter(Boolean);
        if (!STATE.pro.on) err = 'pro';
        else if (err || !name || name.length > 64 || v.v && v.v !== '1' || v.pages && !num(v.pages, 1, 4) || !ws.every(wid)
          || [1, 2, 3, 4].some(p => p <= pages && (v['t' + p] && !num(v['t' + p], 2, 600) || v['bg' + p] && !hex(v['bg' + p])))) err = 'bad';
        else { at = n || last + 1; if (at > 6) err = 'full'; else if (at > last + 1) at = last + 1; }
        if (!err && no === 'save') err = 'save';
        // unknown keys of a later version stay (the core writes only its own)
        if (!err) C[sec(at)] = Object.assign({}, C[sec(at)], Object.fromEntries(LK.map(k => [k, k === 'name' ? name : k === 'v' ? '1' : k === 'pages' ? String(pages)
          : /^t\d$/.test(k) ? (+k[1] <= pages ? v[k] || '10' : '') : /^bg\d$/.test(k) ? (+k[2] <= pages ? (v[k] || '000000').toUpperCase() : '') : ws[+k.slice(1) - 1] || ''])));
      } else if (n < 1 || n > last) err = 'bad';
      else if (no === 'save') err = 'save';
      else {
        for (let i = n; i < last; i++) C[sec(i)] = C[sec(i + 1)]; delete C[sec(last)]; at = n;
        Object.keys(C).filter(s => /^dev\.\d+$/.test(s)).forEach(s => { const k = +C[s].live || 0; if (k === n) C[s].live = '0'; else if (k > n) C[s].live = String(k - 1); });
      }
      if (no === 'silent') return;
      setTimeout(() => { emit(STATE); emit({ type: 'lcd_done', req: o.req, err, n: err ? 0 : at }); }, 40);
    }
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
    // a Corsair LCD's pack: corsairlcd-<N>.hlcd, no theme (ui_web.cpp); window.mockLastBuild keeps what came, for the tests
    if (o.cmd === 'tryx_build') { window.mockLastBuild = o; const no = window.mockTryxFail, lcd = o.ext === 'hlcd'; if (!no) Object.assign(C['dev.' + o.id] = C['dev.' + o.id] || {}, { media: 'C:\\Users\\me\\AppData\\Roaming\\haku-control\\' + (lcd ? 'corsairlcd-' : 'tryx-') + o.id + '.' + o.ext }, lcd ? {} : { theme: '-1' }); setTimeout(() => { emit(STATE); emit({ type: 'tryx_built', req: o.req, ok: no ? 0 : 1, err: no || '' }); }, 60); }
    // cf_close: a program with stay: 1 only hides in the tray (fail 1); cf_end ends it whatever it is
    if (o.cmd === 'cf_close' || o.cmd === 'cf_end') { const x = STATE.cf.a.find(a => a.id === o.id); STATE.cf.busy = o.id; emit(STATE);
      setTimeout(() => { STATE.cf.busy = ''; if (o.cmd === 'cf_close' && x.stay) x.fail = 1; else x.run = 0, x.fail = 0; emit(STATE); }, 600); }
    if (o.cmd === 'tryx_media') { Object.assign(C['dev.' + o.id] = C['dev.' + o.id] || {}, { media: 'C:\\Pictures\\cat.gif', theme: '-1' }); setTimeout(() => emit(STATE), 60); }
    // fans.c, roughly: a mode takes the channel and the QUADRO confirms it; '' gives it back (fans_set: all its keys are in first)
    // an iCUE LINK channel (lk) stays haku's: '' is its default (fan 50, pump 100) and a pump never goes under 50
    const fanMode = (k, m) => { const f = /^fan\d+$/.test(k) && STATE.fans.f[+k.slice(3) - 1]; if (!f) return; let w = m === 'fix' ? +(C.fans[k + '_pct'] || 50) : m ? 45 : -1;
      if (w >= 0 && f.lk && f.pump) w = Math.max(50, w); const out = w < 0 && f.lk ? (f.pump ? 100 : 50) : w;
      Object.assign(f, { own: m || f.lk ? 1 : 0, want: w, out, ok: 1, err: '' }); if (m || f.lk) f.pct = out; };
    // link.c + fans_link: [link] ctl=1 takes the hub a moment later; its fan (channel 1) and pump (20) show from index 11
    if (o.cmd === 'set' && o.s === 'link' && o.k === 'ctl') setTimeout(() => { const K = STATE.link, on = o.v === '1', f = STATE.fans.f; K.ctl = K.held = +on; f.length = 4;
      if (on) { for (let i = 4; i < 31; i++) f[i] = null; f[11] = { lk: 1, pump: 0, rpm: 311, pct: 50, own: 1, want: -1, out: 50, ok: 1, em: '', err: '' };
        f[30] = { lk: 1, pump: 1, rpm: 2277, pct: 100, own: 1, want: -1, out: 100, ok: 1, em: '', err: '' }; }
      emit(STATE); }, 300);
    if (o.cmd === 'set' && o.s === 'fans') fanMode(o.k, o.v);
    if (o.cmd === 'fans_set') o.kv.split('\n').forEach(l => { const e = l.indexOf('='); fanMode(l.slice(0, e), l.slice(e + 1)); });
    if (['effect', 'brightness', 'preset', 'profile', 'power'].includes(o.cmd)) STATE.preview = '';
    if (o.cmd === 'effect') STATE.effect = G.effect = o.id;
    // main.c app_preset_save: id 0 takes the first free slot (never one in use); a new preset carries the effect's settings as fx.*
    if (o.cmd === 'preset_save') { let id = +o.id; if (!id) { id = 1; while (C['preset.' + id] && C['preset.' + id].name) id++; }
      const e = o.effect || STATE.effect, p = { name: o.name, effect: e, palette: o.palette || '', speed: o.speed || '', fx: '1' };
      if (o.brightness) p.brightness = o.brightness;
      if (o.fxkv) o.fxkv.split('\n').forEach(l => { const q = l.indexOf('='); if (q > 0 && l.slice(q + 1) !== '') p['fx.' + l.slice(0, q)] = l.slice(q + 1); });   // the constructor's draft
      else Object.entries(C[e] || {}).forEach(([k, v]) => { if (v !== '' && k !== 'palette' && k !== 'speed') p['fx.' + k] = v; });
      C['preset.' + id] = p; if (o.apply === '1') { G.preset = String(id); STATE.effect = G.effect = e; } setTimeout(() => emit(STATE), 30); }
    // main.c app_preset_copy / app_preset_delete
    if (o.cmd === 'preset_copy' && C['preset.' + o.id]) { let id = 1; while (C['preset.' + id] && C['preset.' + id].effect) id++;
      if (id <= 32) { C['preset.' + id] = { ...C['preset.' + o.id], name: o.name || C['preset.' + o.id].name }; setTimeout(() => emit(STATE), 30); } }
    if (o.cmd === 'preset_delete') { delete C['preset.' + o.id]; if (G.preset === String(o.id)) G.preset = ''; setTimeout(() => emit(STATE), 30); }
    if (o.cmd === 'brightness') STATE.brightness = +o.v;
    if (o.cmd === 'get') setTimeout(() => emit(STATE), 30);
    // pro/referral.c: the site gives the link; window.mockRefFail = 'busy' | 'net': it refuses or does not answer
    if (o.cmd === 'ref_join') { STATE.ref = { ...STATE.ref, busy: 1, err: '' }; setTimeout(() => { STATE.ref = { ...STATE.ref, busy: 0, err: window.mockRefFail || '', code: window.mockRefFail ? '' : 'RV24MGS7' }; emit(STATE); }, 300); }
    if (o.cmd === 'diag') setTimeout(() => { STATE.diag = { busy: 0, ok: 1, file: 'haku-diagnostics-20261022-1200.txt', report: { busy: 0, ok: -1, id: '', err: '' } }; emit(STATE); }, 300);
    if (o.cmd === 'report') setTimeout(() => { STATE.diag = { busy: 0, ok: -1, file: '', report: { busy: 0, ok: o.text.includes('fail') ? 0 : 1, id: '3fa2c9d01e', err: o.text.includes('fail') ? 'offline' : '' } }; emit(STATE); }, 900);
    if (o.cmd === 'pc_own') { STATE.pc_own = STATE.msi_own = STATE.ram_own = +o.v; STATE.ext.devs.forEach(d => { if (['openrgb', 'wooting', 'nlusb', 'razer', 'steelseries', 'logitech', 'tryx', 'aura', 'lianli', 'corsairlcd', 'icuelink'].includes(d.kind)) d.own = +o.v; }); setTimeout(() => emit(STATE), 60); }
    if (o.cmd === 'dev_retry') setTimeout(() => emit(STATE), 60);   // the devices not connected try again (here: they all answer)
    if (o.cmd === 'save_retry') { if (!window.mockSaveFail) STATE.unsaved = 0; setTimeout(() => emit(STATE), 60); }   // window.mockSaveFail = 1: the disk still refuses
    if (o.cmd === 'dev_check') { STATE.netchk = { id: +o.id, busy: 1, res: '' }; emit(STATE); setTimeout(() => { STATE.netchk = { id: +o.id, busy: 0, res: o.id % 2 ? 'silent' : 'answers', ms: 0 }; emit(STATE); }, 1500); }
    // profile.c's refusals (prof_json): window.mockProfFail = 2 (the one in use can't be written), 3 (missing), 4 (damaged)
    const refuse = (err, id) => { STATE.prof = { err, id: +id, n: STATE.prof.n + 1 }; setTimeout(() => emit(STATE), 60); };
    if (o.cmd === 'profile') { if (window.mockProfFail) refuse(window.mockProfFail, window.mockProfFail === 2 ? G.profile : o.id); else { G.profile = o.id; STATE.prof_why = 1; STATE.prof.err = 0; setTimeout(() => emit(STATE), 120); } }
    if (o.cmd === 'profile_dup') { let n = 1; while (C['profile.' + n] && C['profile.' + n].name) n++; C['profile.' + n] = { name: o.name }; setTimeout(() => emit(STATE), 60); }
    // main.c app_test: one zone's steady test colour, '' ends it (the core's own minute is not run here)
    if (o.cmd === 'dev_test') { STATE.test = o.zone; setTimeout(() => emit(STATE), 30); }
    // main.c app_preview / app_preview_keep (preview.h): warm white over those zones, '' ends it; keep saves it as their white
    // light (1), finds it ended (0) or, window.mockKeepFail, the settings full (-1); its own 45 s are not run here
    if (o.cmd === 'preview') { STATE.preview = o.zones || ''; STATE.test = ''; setTimeout(() => emit(STATE), 30); }
    if (o.cmd === 'preview_keep') { const z = STATE.preview ? STATE.preview.split(',') : []; STATE.preview = '';
      const r = !z.length ? 0 : window.mockKeepFail ? -1 : 1;
      if (r === 1) { z.forEach(s => Object.assign(C[s] = C[s] || {}, { mode: 'white', kelvin: '2700', brightness: '55' })); if (STATE.effect === 'off') STATE.effect = G.effect = 'flow'; }
      setTimeout(() => { emit({ type: 'preview_done', r }); emit(STATE); }, 40); }
    // devices.c: the scan, short here; window.mockScanStuck: one that does not end
    if (o.cmd === 'scan') { STATE.ext.scanning = 1; emit(STATE); if (!window.mockScanStuck) setTimeout(() => { STATE.ext.scanning = 0; emit(STATE); }, 300); }
    // devices.c ext_add: the same hardware is never added twice
    if (o.cmd === 'dev_add') { const E = STATE.ext, same = x => x.kind === o.kind && x.host === o.host && x.sub === o.sub, f = E.found.find(same);
      if (!E.devs.some(same)) E.devs.push({ id: Math.max(0, ...E.devs.map(d => d.id)) + 1, kind: o.kind, title: f ? f.title : o.kind, name: o.name, host: o.host, sub: o.sub,
        leds: f ? f.leds : 1, per_led: 1, online: 1, enabled: 1, type: '', info: f ? f.info : '' });
      if (f) f.added = 1; setTimeout(() => emit(STATE), 60); }
    if (o.cmd === 'dev_remove') { STATE.ext.devs = STATE.ext.devs.filter(x => x.id !== +o.id); setTimeout(() => emit(STATE), 60); }
    // devices.c ext_orgb_split: the board's connectors each a device (added once, the board's count for it), the board off; or back
    if (o.cmd === 'orgb_split') { const D = STATE.ext.devs, b = D.find(x => x.id === +o.id), on = o.on === '1', set = (x, v) => { x.enabled = v; (C['dev.' + x.id] = C['dev.' + x.id] || {}).enabled = String(v); };
      (b.zones || '').split('|').filter(Boolean).forEach(z => { const [name, count, min, max, , idx] = z.split(':'), sub = b.sub + 1000 * (+idx + 1);
        let x = D.find(y => y.kind === 'openrgb' && y.host === b.host && y.sub === sub);
        if (!x && on) D.push(x = { id: Math.max(0, ...D.map(y => y.id)) + 1, kind: 'openrgb', title: 'OpenRGB', name: b.name + ' · ' + name, host: b.host, sub, leds: +count, per_led: 1, online: 1, type: 'fan', info: 'OpenRGB · ARGB connector', zones: [name, count, min, max, 0, idx].join(':') });
        if (x) set(x, on ? 1 : 0); });
      set(b, on ? 0 : 1); setTimeout(() => emit(STATE), 60); }
    // window.mockSaveFail: its file can't be written (1) or all are taken (5): no profile, the reason in prof; 'silent': no answer
    if (o.cmd === 'profile_save' && window.mockSaveFail && window.mockSaveFail !== 'silent') refuse(window.mockSaveFail === 5 ? 5 : 1, 0);
    if (o.cmd === 'profile_save' && !window.mockSaveFail) { let n = 1; while (C['profile.' + n] && C['profile.' + n].name) n++; C['profile.' + n] = { name: o.name }; G.profile = String(n); setTimeout(() => emit(STATE), 60); }
    if (o.cmd === 'profile_delete') { delete C['profile.' + o.id]; if (G.profile === o.id) G.profile = ''; setTimeout(() => emit(STATE), 60); }
  },
  addEventListener(t, f) { L.push(f); },
} };
const LAY = LAYOUTS[location.hash.slice(1)], MULTI = location.hash === '#multi';
const panels = (LAY || LAYOUTS.blocks).panels;
const STATE = {
  type: 'state', test: '', preview: '', unsaved: 0, cfg_full: 0, prof: { err: 0, id: 0, n: 0 }, perf: { fps10: 299, chg10: 299, avg_us: 420, max_us: 1900, want: 30 }, hw: { busy: 0, done: 1, items: [{ cat: 'board', maker: 'MSI', name: 'MAG B550 TOMAHAWK', id: '' }, { cat: 'ram', maker: 'Corsair', name: '2 × CMW16GX4M2C3200C16', id: '' },
    { cat: 'gpu', maker: 'ASUS', name: 'NVIDIA GeForce RTX 4070', id: '10DE:2786' }, { cat: 'usb', maker: 'Corsair', name: 'K70 RGB PRO', id: '1B1C:1BB3' },
    { cat: 'usb', maker: 'Wooting', name: 'Wooting 60HE v2', id: '31E3:1342' }, { cat: 'usb', maker: 'Lian Li', name: 'LianLi-Strimer Plus-1.5', id: '0CF2:A200' }] },
  orgb: { state: 2, exe: 1, auto: 1, ours: 1, setup: 0, pct: 0, err: '', ctls: [{ i: 0, type: 5, kind: 'keyboard', leds: 104, name: 'Keyboard' }, { i: 1, type: 3, kind: 'cooler', leds: 16, name: 'Fan hub' },
    { i: 2, type: 2, kind: 'graphics card', leds: 8, name: 'ASUS TUF RTX 4070' }, { i: 6, type: 1, kind: 'memory', leds: 8, name: 'ENE DRAM' }, { i: 7, type: 2, kind: 'graphics card', leds: 3, name: 'MSI RTX 3060 Ventus' }] }, autostart: 1, ollama: { exe: 0, model: 0, ours: 0, on_demand: 0, startup: 0, stage: 5, pct: 42, err: '' }, effect: 'flow', brightness: 85, msi: 1, sticks: 2, ram_src: [0, 0], gpu_temp: 41, hotspot: 1, fx_audio: -2, fx_temp: -2, fans: { on: 1, q: 1, link: 1, age: 400, paused: 0, rfail: 0, wd: [0, 0], gpu: 41, gage: 600, serial: '04976-37680', fw: 1024, flow: 0, t: [31.4, null, null, null], log: [], f: [{ rpm: 1180, pct: 52, v: 12, w: 1.4, own: 0, want: -1, out: -1, ok: 1, em: '', err: '' }, { rpm: 1210, pct: 52, v: 12, w: 1.5, own: 0, want: -1, out: -1, ok: 1, em: '', err: '' }, { rpm: 0, pct: 0, v: 0, w: 0, own: 0, want: -1, out: -1, ok: 1, em: '', err: '' }, { rpm: 0, pct: 0, v: 0, w: 0, own: 0, want: -1, out: -1, ok: 1, em: '', err: '' }] }, ll: { st: 'ok', err: 0, rep: 3, g: [{ mac: '7c:9c:06:f5:17:e1', own: 1, ch: 8, type: 0, n: 3, rpm: [1650, 1664, 1820], pct: [79, 79, 79] }, { mac: '3a:10:c4:22:9e:05', own: 0, ch: 8, type: 0, n: 2, rpm: [980, 1004], pct: [40, 40] }] }, cf: { busy: '', a: [{ id: 'armoury', name: 'ASUS Armoury Crate / Aura', w: 'board', cool: 1, run: 1, inst: 1, bad: 0, fail: 0 }, { id: 'icue', name: 'Corsair iCUE', w: 'corsair', cool: 1, run: 0, inst: 1, bad: 0, fail: 0 }, { id: 'asrock', name: 'ASRock Polychrome RGB', w: 'board', cool: 0, run: 1, inst: 1, bad: 0, fail: 0, stay: 1 }] }, mqtt: { state: 'connected', err: '', topic: 'haku/desk-pc' }, voice: { pro: 1, ready: 1, wake: 0, lang: '', setup: 0, pct: 0, serr: '', mic: 0, state: 0, err: '', heard: 'Haku, gaming profile', did: 'profile', arg: 'Gaming', seq: 1 }, walls: { pro: 1, busy: 0, seq: 0, err: '', cur: -1, setting: -1, saved: 0, web: 1, dir: '', items: [] }, pro: { on: 1, state: 'trial', kind: '', days: 12, until: '', checked: '', stale: 0, key: '', who: '', err: '', busy: 0, life: 0 }, rules: { on: 1, win: 0, by: 0, list: Array.from({ length: 16 }, () => ({ act: 0, yield: 0, err: 0, bad: 0, cv: [0, 0, 0, 0, 0, 0] })) }, ref: { code: '', points: 0, pct: 0, discount: '', busy: 0, err: '' }, remote: { enabled: 1, on: 1, port: 8723, pin: '481205', paired: 1, fw_blocks: 0, urls: ['http://172.20.10.4:8723', 'http://192.168.137.1:8723'] }, accounts: { aidot: { state: 0, msg: '', found: 0, countries: [['FR','France'],['DE','Germany'],['RU','Russia'],['US','United States']] } }, update: { version: '0.2.0', repo: 1, latest: '0.3.0', url: 'https://github.com/', can: 1, inst: 0, pct: 0, err: '' },
  effects: [['flow','Течение'],['caustic','Каустика'],['bubbles','Пузырьки'],['comet','Комета'],['lava','Лава'],['breathe','Дыхание'],['rainbow','Радуга'],['wave','Цветная волна'],['fire','Огонь'],['ocean','Океан'],['twinkle','Созвездие'],['meteor','Метеорный дождь'],['plasma','Плазма'],['aurora','Северное сияние'],['ripple','Капли'],['matrix','Матрица'],['candle','Свеча'],['pendulum','Маятник'],['interference','Интерференция'],['steps','Ступени'],['screen','Экран'],['temperature','Температура'],['pump','Поток по насосу'],['audio','Звук'],['static','Статичный цвет'],['custom','Конструктор'],['off','Выключить']].map(([id,title])=>({id,title})),
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
      { id: 8, kind: 'openrgb', title: 'OpenRGB', name: 'B550-F', host: '127.0.0.1', sub: 4, leds: 12, per_led: 1, online: 1, enabled: 1, type: 'board', info: 'OpenRGB · ASUS ROG STRIX B550-F GAMING · motherboard', zones: 'Addressable 1:24:0:1024:0:1|Addressable 2:0:0:1024:24:2' },
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
STATE.link = { st: 'ok', found: 1, app: '', fw: '3.10.636', hw: 0, age: 4000, ctl: 0, held: 0, refuse: 0, hot: 0, leds: 0, hub_t: 25.6, v: [3.31, 5.05, 12.03], a: [1.75, 5.5, 10.25],
  dev: [{ ch: 1, type: 15, model: 0, rpm: 311, t: null }, { ch: 17, type: 6, model: 0, rpm: null, t: null }, { ch: 20, type: 17, model: 2, rpm: 2277, t: 35.2 }, { ch: 25, type: 11, model: 0, rpm: 457, t: 40.5 }] };
const pal = [[0,200,255],[122,60,255],[255,45,149]];
const pc = p => { p = (p % 1 + 1) % 1 * 3; const i = Math.floor(p), f = p - i, a = pal[i % 3], b = pal[(i + 1) % 3]; return a.map((v, k) => Math.round(v + (b[k] - v) * f)); };
const hx = c => c.map(v => v.toString(16).padStart(2, '0')).join('');
// ui_web.cpp FRAME_TIMER: 20 a second while the page draws them, else every 8th (the background glow)
const FRAMES = { live: true, n: 0, sent: 0, emit: () => {} };
setInterval(() => { if (FRAMES.live || ++FRAMES.n % 8 === 0) FRAMES.emit(); }, 50);
FRAMES.emit = () => {
  FRAMES.sent++;
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
};
