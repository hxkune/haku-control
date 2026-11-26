// Cooling: the fans page, its curve editor and the fan tiles on Home. A plain script loaded before app.js: at the top
// level only declarations (they use app.js's helpers and state when called, never while loading); fanBind() at the end.

// ------------------------------------------------------------------ fans: Aqua Computer QUADRO and the board's Super I/O (fans.c, sio.c)
// channels 1..4 are the QUADRO's (null in S.fans.f without one), 5.. the board's (b: 1)
// [fans] fanN = '' the QUADRO's own setting | fix | curve, fanN_pct 20..100, fanN_src gpu | cpu | t1..t4 (unset: gpu; the CPU is read only while a curve follows it), fanN_curve '30:30,50:45,85:100' (for fanN_src),
//        fanN_min the channel's floor (20 and up), fanN_fail its speed while the temperature is lost or the curve does not read (100),
//        fanN_down how many % a curve may slow per 2 s (3)
// the panel only (fans.c reads none of these): fanN_curve_<src> the curve kept for a source not in use, fanN_name, fanN_hide = 1,
//        fanN_kind = pump, fanN_notach = 1 (no speed signal), tN_name, gN_name (the group exists while it has one), gN = '1,2'
//        cN_name, cN, cN_k = gpu | t: curves saved by name (only their points are ever used)
const FAN_Q = 4, FAN_LK = 11, FAN_GROUPS = 4, FAN_HIST = 5 * 60000;
// the chart and the default curve follow the source: a GPU runs far hotter than the loop's water (fans.c has the same defaults)
const FAN_SCALE = { cpu: { lo: 20, hi: 100, def: '40:30,60:45,75:75,90:100' }, gpu: { lo: 20, hi: 100, def: '30:30,50:45,70:75,85:100' }, t: { lo: 15, hi: 55, def: '25:30,30:40,35:70,40:100' } };
// open: { i } a fan, { g } a group, { g, i } a fan of a group; draft: the curve being edited, sent only by Apply
const FAN = { drag: null, sel: 0, open: null, sig: '', last: null, hist: [], zero: [], back: 0, draft: null, done: null, fold: {}, hl: { w: 1, p: 1, r: 1, t: 1 }, hov: null };
// the settings beside the panel when the window is wide enough (as .fan-main in style.css), in the sheet otherwise
const FAN_WIDE = matchMedia('(min-width: 1100px)');
const fanKey = (i, k) => `fan${i + 1}${k ? '_' + k : ''}`;
const fanSrc = i => cv('fans', fanKey(i, 'src'), 'gpu');
const fanScale = s => FAN_SCALE[s === 'gpu' || s === 'cpu' ? s : 't'];
// the rules fans.c reads a curve by: 2..8 points "°C:%", temperature 0..110 rising strictly, speed 0..100; null if it does not read
const fanParse = s => { const r = [];
  for (const q of String(s).split(',')) { const m = /^(\d+):(\d+)$/.exec(q); if (!m) return null;
    const x = +m[1], y = +m[2]; if (x > 110 || y > 100 || (r.length && x <= r[r.length - 1][0])) return null; r.push([x, y]); }
  return r.length >= 2 && r.length <= 8 ? r : null; };
const fanSer = pts => pts.map(p => p.join(':')).join(',');
// a curve per source: the one in use is fanN_curve, the others wait under fanN_curve_<src>; empty is the source's default
const fanCurveOf = (i, s) => cv('fans', fanKey(i, s === fanSrc(i) ? 'curve' : 'curve_' + s), '') || fanScale(s).def;
const fanCurveRaw = i => fanCurveOf(i, fanSrc(i));
const fanCurve = i => fanParse(fanCurveRaw(i)) || fanParse(fanScale(fanSrc(i)).def);
// fans.c's curve_at: flat outside the points, straight between them, rounded; then the channel's floor and 100 on top
const fanAt = (pts, x) => { if (x <= pts[0][0]) return pts[0][1];
  for (let k = 1; k < pts.length; k++) if (x <= pts[k][0]) return Math.floor(pts[k - 1][1] + (pts[k][1] - pts[k - 1][1]) * (x - pts[k - 1][0]) / (pts[k][0] - pts[k - 1][0]) + .5);
  return pts[pts.length - 1][1]; };
// the curve over the chart's range as fans.c runs it: its value at both edges and the points between; a point past the
// range stays in the curve where it is (drawn as a mark at the edge), never pulled onto the chart
const fanVis = (pts, sc) => [[sc.lo, fanAt(pts, sc.lo)], ...pts.filter(p => p[0] > sc.lo && p[0] < sc.hi), [sc.hi, fanAt(pts, sc.hi)]];
const fanOut = (pts, x, lo) => Math.max(lo, Math.min(100, fanAt(pts, x)));
const fanTemp = s => { const F = S.fans || {}; return s === 'cpu' ? F.cpu ?? null : s === 'gpu' ? F.gpu ?? S.gpu_temp ?? null : (F.t || [])[+s.slice(1) - 1] ?? null; };
const fanDeg = v => v.toLocaleString(LANG, { maximumFractionDigits: 1 }) + '°C';
const fanMin = i => Math.min(100, Math.max(fanCh(i).lk && fanCh(i).pump ? 50 : 20, +cv('fans', fanKey(i, 'min'), '') || 20));   // fans.c: a LINK pump never under 50
const fanDown = i => Math.min(100, Math.max(1, +cv('fans', fanKey(i, 'down'), '') || 3));
const clamp20 = v => Math.min(100, Math.max(20, v));
const fanAll = () => ((S.fans || {}).f || []).flatMap((f, i) => f ? [i] : []);   // the channels there are
const fanName = i => cv('fans', fanKey(i, 'name'), '') || (i >= FAN_LK ? `${t(fanPump(i) ? 'fans.lpump' : 'fans.lch')} ${i - FAN_LK + 1}`
  : `${t(fanPump(i) ? 'fans.kind.pump' : i >= FAN_Q ? 'fans.bch' : 'fans.ch')} ${i >= FAN_Q ? i - FAN_Q + 1 : i + 1}`);
const fanHidden = i => cv('fans', fanKey(i, 'hide'), '') === '1';
const fanGroups = () => Array.from({ length: FAN_GROUPS }, (_, k) => k + 1).filter(n => cv('fans', `g${n}_name`, ''))
  .map(n => ({ n, name: cv('fans', `g${n}_name`, ''), m: cv('fans', `g${n}`, '').split(',').map(x => +x - 1).filter(i => fanAll().includes(i) && !fanHidden(i)) }));
// what the open settings change: the one fan (alone or opened inside its group), or every fan of the group
const fanTargets = () => { const o = FAN.open; if (!o) return []; if (o.i != null) return [o.i]; return (fanGroups().find(g => g.n === o.g) || { m: [] }).m; };
const fanSetAll = (k, v, soon) => soon ? fanTargets().forEach(i => setCfgSoon('fans', fanKey(i, k), v)) : setFans(fanTargets().map(i => [fanKey(i, k), v]));
const fanSrcName = s => s === 'cpu' ? t('fans.cpu') : s === 'gpu' ? t('fans.gpu') : cv('fans', s + '_name', '') || t('fans.t', s.slice(1));
const fanMode = i => { const m = cv('fans', fanKey(i), '');
  return m === 'fix' ? `${t('fans.fix')} · ${cv('fans', fanKey(i, 'pct'), '50')}%` : m === 'curve' ? `${t('fans.curve')} · ${fanSrcName(fanSrc(i))}` : t(fanAsis(i)); };
const fanCh = i => ((S.fans || {}).f || [])[i] || {};
// “as is”: the QUADRO's own setting, the board's BIOS curve, or haku's default on the LINK hub (fans 50%, pumps 100%)
const fanAsis = i => i >= FAN_LK ? 'fans.asis.lk' : i >= FAN_Q ? 'fans.asis.b' : 'fans.asis';
// a channel the QUADRO drives (its readings can be old; the board's and the LINK hub's are read on their own)
const fanQd = f => !f.b && !f.lk;
// what a fan looks like on its card (fans/fan<N>_look, picked by hand: a header only reports rpm): an outline in the
// page's ink, the parts its lighting sits on brighter. Corsair iCUE LINK and Lian Li wireless groups get theirs on their own
const FAN_LOOKS = ['', 'ring', 'hub', 'qx', 'sl', 'inf'];
// the pump: a block with its fittings, the chamber and the impeller (it turns on Home: drawPump)
const PUMP_BLADE = 'M50 53Q56 39 67 41A19 19 0 0 1 69 50Q58 48 50 53Z';
const PUMP_SVG = '<rect x="8" y="8" width="84" height="84" rx="18" fill="#141414" stroke="#757575" stroke-width="2.5"/>' +
  '<rect x="31" y="14" width="12" height="8" rx="2" fill="#6e6e6e"/><rect x="57" y="14" width="12" height="8" rx="2" fill="#6e6e6e"/>' +
  '<circle cx="50" cy="53" r="31" fill="#1a1a1a" stroke="#666" stroke-width="2"/>' +
  '<path d="M24 53A26 26 0 0 1 45 27.5M76 53A26 26 0 0 1 55 78.5" stroke="#8c8c8c" stroke-width="2" stroke-linecap="round"/>' +
  '<circle cx="50" cy="53" r="21" fill="#242424" stroke="#8c8c8c" stroke-width="1.8"/><g fill="#8c8c8c">' +
  [0, 60, 120, 180, 240, 300].map(a => `<path d="${PUMP_BLADE}" transform="rotate(${a} 50 53)"/>`).join('') +
  '</g><circle cx="50" cy="53" r="9.5" fill="#b5b5b5" stroke="#d4d4d4" stroke-width="1.8"/>';
const FAN_BLADES = Array.from({ length: 7 }, (_, k) => `<path transform="rotate(${(k * 360 / 7).toFixed(1)})" d="M-1.5-4.8C-2-10 1-15 6-16.8C9-15.8 11-14.3 12.3-12.3C8.5-9.5 5.5-6.5 3.8-3.3Z"/>`).join('');
// the looks are pictures (ui/fan-<name>.svg): their gradients and filters go once into a hidden <svg> (the ids are
// unique per file), the rest is drawn inline; .on lights them in --live and turns the blades (.fb)
const FAN_ART = {};
// drawn inline until its picture has loaded, and for pumps; sl: Lian Li SL has its LEDs down the sides
function fanIcon(look, pump, on) {
  if (!pump && FAN_ART[look]) return `<svg class="fanic fan-art${on ? ' on' : ''}" data-look="${look}" viewBox="72 34.5 146 146" aria-hidden="true">${FAN_ART[look]}</svg>`;
  const ring = (r, o) => `<circle r="${r}"${o ? ` opacity="${o}"` : ''}/>`, hub = '<circle class="f" r="5"/>', bar = y => `<rect class="f" x="-14" y="${y}" width="28" height="2.4" rx="1.2"/>`;
  const lit = pump ? '' : { ring: ring(19), hub, qx: ring(18.5) + hub, sl: `<g transform="rotate(90)">${bar(-19.6) + bar(17.2)}</g>`, inf: ring(19) + ring(16.4, .55) + ring(13.8, .3) }[look] || '';
  const body = pump ? `<g transform="translate(-25 -25) scale(.5)">${PUMP_SVG}</g>`
    : `<rect class="fi-fr" x="-21" y="-21" width="42" height="42" rx="7"/><g class="fi-bl"${look === 'inf' ? ' transform="scale(.78)"' : ''}>${FAN_BLADES}</g><circle class="fi-hub" r="5"/>`;
  return `<svg class="fanic" viewBox="-24 -24 48 48" aria-hidden="true">${body}${lit && `<g class="fi-glow">${lit}</g><g class="fi-lit">${lit}</g>`}</svg>`;
}
const fanLook = i => cv('fans', fanKey(i, 'look'), '');
const fanPump = i => !!fanCh(i).pump || cv('fans', fanKey(i, 'kind'), '') === 'pump';   // the LINK hub says which is its pump
// a tile's: the fan's, or what every fan of the group shares (plain when they differ)
const fanTileIcon = m => !m.length ? '' : fanIcon(new Set(m.map(fanLook)).size === 1 ? fanLook(m[0]) : '', m.every(fanPump));
// a tile's fans turning (lit and spinning on its card)
const fanRuns = m => m.some(i => !fanOld(i) && fanCh(i).rpm > 0);
const fanDKey = o => !o ? '' : o.i != null ? 'f' + o.i : 'g' + o.g;
// fans as widgets on Home: a group, or a fan in none ([ui] hero_fans lists the ones added: fang<n>, fan<i>)
const heroFans = () => cv('ui', 'hero_fans', '').split(',').filter(Boolean);
function fanWidgets() {
  if (!(S.fans || {}).on || document.documentElement.classList.contains('remote')) return [];
  const gs = fanGroups(), inG = new Set(gs.flatMap(g => g.m));
  return [...gs.map(g => ({ key: 'fang' + g.n, g: g.n, m: g.m, name: g.name })),
    ...fanAll().filter(i => !fanHidden(i) && !inG.has(i)).map(i => ({ key: 'fan' + i, i, m: [i], name: fanName(i) }))];
}
// the fan turning with its rpm, and rpm · % under it (a group: its first fan's); a problem on any of its fans says so instead
const FAN_STALE = 5000;   // as fans.c STALE_MS: older QUADRO readings are not current
// the QUADRO's readings are not now: the link is down, or its last report is older than fans.c trusts (the board reads on its own)
const fanOld = i => { const F = S.fans || {}; return fanQd(fanCh(i)) && (F.link === 0 || (F.age || 0) > FAN_STALE); };
// a Home tile's status from all its fans, the worst first: [short text for the tile, full text for a screen reader,
// only a warning (no rpm read while driven, as the fan's own line says), its rank in FAN_TILE] or null
const FAN_TILE = ['fans.tile.nolink', 'fans.tile.old', 'fans.tile.em', 'fans.tile.err', 'fans.tile.norpm'];
function fanTileSt(m) {
  let w = null;
  for (const i of m) {
    const [st, c] = fanState(i), k = fanOld(i) ? ((S.fans || {}).link === 0 ? 0 : 1) : fanCh(i).em ? 2 : c === 'bad' ? 3 : fanNoRpm(i) ? 4 : -1;
    if (k >= 0 && (!w || k < w.k)) w = { k, i, st: k === 4 ? t('fans.norpm') : st };
  }
  return w && [t(FAN_TILE[w.k]), (m.length > 1 ? fanName(w.i) + ': ' : '') + w.st, w.k === 4, w.k];
}
const FAN_BLADE = new Path2D('M-1.5-4.8C-2-10 1-15 6-16.8C9-15.8 11-14.3 12.3-12.3C8.5-9.5 5.5-6.5 3.8-3.3Z'), FAN_SPIN = {};
const PUMP_BL = new Path2D(PUMP_BLADE), PUMP_FLOW = new Path2D('M24 53A26 26 0 0 1 45 27.5M76 53A26 26 0 0 1 55 78.5');
// the pump on the canvas, the impeller at angle a
function drawPump(c, a) {
  const disc = (r, fill, stroke, lw) => { c.beginPath(); c.arc(50, 53, r, 0, 7); c.fillStyle = fill; c.fill(); c.strokeStyle = stroke; c.lineWidth = lw; c.stroke(); };
  c.save(); c.scale(.5, .5); c.translate(-50, -50);   // its frame (84) as wide as the fan's (42)
  rr(c, 8, 8, 84, 84, 18); c.fillStyle = '#141414'; c.fill(); c.strokeStyle = '#757575'; c.lineWidth = 2.5; c.stroke();
  c.fillStyle = '#6e6e6e'; rr(c, 31, 14, 12, 8, 2); c.fill(); rr(c, 57, 14, 12, 8, 2); c.fill();
  disc(31, '#1a1a1a', '#666', 2);
  c.strokeStyle = '#8c8c8c'; c.lineCap = 'round'; c.stroke(PUMP_FLOW);
  c.translate(50, 53); c.rotate(a); c.translate(-50, -53);
  disc(21, '#242424', '#8c8c8c', 1.8);
  c.fillStyle = '#8c8c8c';
  for (let b = 0; b < 6; b++) { c.fill(PUMP_BL); c.translate(50, 53); c.rotate(Math.PI / 3); c.translate(-50, -53); }
  disc(9.5, '#b5b5b5', '#d4d4d4', 1.8);
  c.restore();
}
function drawFanTile(c, x, y, w, h, fw) {
  const d = devicePixelRatio, i = fw.m[0], f = i == null ? {} : fanCh(i), pump = fw.m.length > 0 && fw.m.every(fanPump), st = fanTileSt(fw.m);
  const rpm = i != null && !fanOld(i) && f.rpm > 0 ? f.rpm : 0;   // old readings never turn it
  const now = performance.now(), sp = FAN_SPIN[fw.key] = FAN_SPIN[fw.key] || { a: 0, t: now }, calm = document.body.classList.contains('calm');
  if (rpm && !calm) { sp.a += Math.min(100, now - sp.t) / 1000 * rpm / 1500 * Math.PI * 2; HERO.moving = !document.body.classList.contains('idle') || HERO.moving; }
  sp.t = now;
  // the line under it: rpm · %, or what is wrong, on two lines when one is too narrow for it
  c.font = `${(st ? 9 : 10) * d}px ${WIDE}`;
  const fit = s => { while (s.length > 1 && c.measureText(s).width > w + 12 * d) s = s.slice(0, -2) + '…'; return s; };
  const s = i == null ? t('fans.empty') : st ? st[0] : `${f.rpm == null || f.rpm < 0 ? '—' : f.rpm} ${t('fans.rpmu')} · ${f.pct == null ? '—' : Math.round(f.pct) + '%'}`;
  const sp2 = s.indexOf(' '), ls = (st && sp2 > 0 && c.measureText(s).width > w + 12 * d ? [s.slice(0, sp2), s.slice(sp2 + 1)] : [s]).map(fit);
  const lh = 8 * d + ls.length * 10 * d, R = Math.max(8 * d, Math.min(w, h - lh) * .4), cx = x + w / 2, cy = y + (h - lh) / 2, k = R / 21;
  c.save(); c.translate(cx, cy); c.scale(k, k); c.lineWidth = 1.3 * d / k; c.strokeStyle = inkA(.32);
  if (pump) drawPump(c, sp.a);
  else {
    rr(c, -21, -21, 42, 42, 7); c.stroke();
    c.save(); c.rotate(sp.a); c.fillStyle = inkA(rpm ? .55 : .25);
    for (let b = 0; b < 7; b++) { c.rotate(Math.PI * 2 / 7); c.fill(FAN_BLADE); }
    c.restore();
    c.beginPath(); c.arc(0, 0, 5, 0, 7); c.fillStyle = inkA(.7); c.fill();
  }
  c.restore();
  c.textAlign = 'center'; c.fillStyle = st ? (st[2] ? '#f0b44a' : '#d0605a') : THEME.hi;
  ls.forEach((l, n) => c.fillText(l, cx, y + h - 2 * d - (ls.length - 1 - n) * 10 * d));
}
const fanValid = o => !!o && (o.g ? fanGroups().some(g => g.n === o.g && (o.i == null || g.m.includes(o.i))) : !fanHidden(o.i));
// who drives the channel, and only what the QUADRO read back counts as done: [text, '' | 'warn' | 'bad']
function fanState(i) {
  const F = S.fans || {}, f = fanCh(i), m = cv('fans', fanKey(i), ''), why = f.err ? ' · ' + t('fans.err.' + f.err) : '';
  if (fanOld(i)) return [t(F.link === 0 ? 'fans.st.stale' : 'fans.st.old'), 'bad'];
  // the LINK hub: link.c sends what fans.c works out every 2 s, an emergency too; the hub does not answer what it took, so nothing says confirmed
  if (f.lk) return f.em ? [t('fans.em.' + f.em) + '. ' + t('fans.lk.em', f.out), 'bad'] : m && f.want === -2 ? [t('fans.st.wait'), 'warn'] : [t(m ? 'fans.lk.sent' : 'fans.lk.def'), ''];
  // an emergency: why it was needed, then the speed asked and whether the QUADRO took it
  if (f.em) { const ep = f.want >= 0 ? f.want : Math.max(fanMin(i), Math.min(100, +cv('fans', fanKey(i, 'fail'), '') || 100));
    return [t('fans.em.' + f.em) + '. ' + t(f.ok === -1 ? 'fans.em.fail' : !f.own || f.want == null || f.want < 0 ? 'fans.em.wait' : f.ok === 0 ? 'fans.em.unconf' : 'fans.em.ok', ep) + why, 'bad']; }
  if (!m) return !f.own ? ['', ''] : F.rfail ? [t(f.b ? 'fans.st.back.b' : 'fans.st.back'), 'bad'] : [t(f.b ? 'fans.back.wait.b' : 'fans.back.wait'), 'warn'];   // the tile already says “By the QUADRO”
  if (f.err === 'save') return [t('fans.st.save'), 'bad'];   // not taken: fans.c never drives a channel whose own setting it could not keep
  if (f.want === -2) return [t('fans.st.wait'), 'warn'];
  if (f.ok === -1) return [t('fans.st.fail') + why, 'bad'];
  if (!f.own || f.want == null || f.want < 0) return [t('fans.st.pend'), 'warn'];
  if (f.ok === 0) return [t('fans.st.unconf') + why, 'warn'];
  return [t('fans.st.ok'), ''];
}
const fanNoRpm = i => FAN.zero[i] >= 3 && cv('fans', fanKey(i, 'notach'), '') !== '1';
// a channel's live line: rpm first, then the speed, who drives it, what haku asked for
function fanRow(i, named) {
  const f = fanCh(i), [st, cls] = fanState(i), m = cv('fans', fanKey(i), '');
  const ask = m && !f.em && f.want >= 0 ? (st ? ' · ' : '') + t('fans.asked', f.want) + (f.out >= 0 && f.out !== f.want ? ' · ' + t('fans.out', f.out) : '') : '';
  return (named ? `<i>${esc(fanName(i))}</i>` : '') + `<em>${f.rpm < 0 ? '—' : f.rpm ?? 0}</em> ${esc(t('fans.rpmu'))} · ${f.pct == null && f.b ? '—' : Math.round(f.pct ?? 0)}%` +
    (st + ask ? `<span class="fan-st ${cls}">${esc(st + ask)}</span>` : '') + (fanNoRpm(i) ? `<span class="fan-st warn">${esc(t('fans.norpm'))}</span>` : '');
}
// a panel line: name and rpm, a bar of the speed read back, then a status only when it says more than “confirmed” (a ✓ then);
// a channel on its QUADRO's own setting gets no bar: the QUADRO reports no speed for it
function fanLine(i, named) {
  const f = fanCh(i), [st, cls] = fanState(i), m = cv('fans', fanKey(i), ''), p = f.pct == null || f.pct < 0 ? null : Math.round(f.pct);
  const okd = st === t('fans.st.ok'), ask = m && !f.em && f.want >= 0 && f.want !== p ? ' · ' + t('fans.asked', f.want) : '';
  return (named ? `<i>${esc(fanName(i))}</i>` : '') + `<span class="fl-rpm"><em>${f.rpm == null || f.rpm < 0 ? '—' : f.rpm}</em> ${esc(t('fans.rpmu'))}</span>` +
    (!m && !f.b && !f.lk ? '' : `<span class="fl-bar"><b style="width:${p ?? 0}%"></b></span><span class="fl-pct">${p == null ? '—' : p + '%'}${esc(ask)}` +
      (okd ? `<span class="fl-ok" title="${esc(st)}">✓<span class="fl-vh">${esc(st)}</span></span>` : '') + '</span>') +
    (st && !okd ? `<span class="fan-st ${cls}">${esc(st)}</span>` : '') + (fanNoRpm(i) ? `<span class="fan-st warn">${esc(t('fans.norpm'))}</span>` : '');
}
// the chosen fan now: the curve's value at the temperature, what the limits make of it, and the speed while it slows down
function fanNow(i) {
  const f = fanCh(i), T = fanTemp(fanSrc(i));
  if (cv('fans', fanKey(i), '') !== 'curve' || fanOld(i) || f.em || T == null || f.want == null || f.want < 0) return '';
  const raw = fanAt(fanCurve(i), T), r = [t('fans.now', fanDeg(T), raw)];
  if (f.want !== raw) r.push(t('fans.now.lim', f.want));
  if (f.out >= 0 && f.out > f.want) r.push(t('fans.now.slow', f.out, fanDown(i)));
  return r.join(' · ');
}
// what the group's fans do not share; only what the mode they share uses
function fanDiff(tg) {
  const d = k => new Set(tg.map(i => k === 'curve' ? fanCurveRaw(i) : k === 'src' ? fanSrc(i) : cv('fans', fanKey(i, k), k === 'pct' ? '50' : ''))).size > 1;
  if (tg.length < 2) return [];
  if (d('')) return ['mode'];
  const m = cv('fans', fanKey(tg[0]), '');
  return m === 'fix' ? (d('pct') ? ['pct'] : []) : m === 'curve' ? ['src', 'curve'].filter(d) : [];
}
// the chart: the source's range across, 20..100 % up
function fanGeo(cvs, sc) {
  const W = cvs.clientWidth, H = cvs.clientHeight, l = 38, r = 12, tp = 12, b = 24, w = sc.hi - sc.lo;
  return { W, H, px: x => l + (Math.min(sc.hi, Math.max(sc.lo, x)) - sc.lo) / w * (W - l - r), py: y => H - b - (y - 20) / 80 * (H - tp - b),
    ix: x => sc.lo + (x - l) / (W - l - r) * w, iy: y => 20 + (H - b - y) / (H - tp - b) * 80 };
}
const fanFit = cvs => { const d = devicePixelRatio || 1, W = Math.round(cvs.clientWidth * d), H = Math.round(cvs.clientHeight * d);
  if (cvs.width !== W || cvs.height !== H) { cvs.width = W; cvs.height = H; }
  const c = cvs.getContext('2d'); c.setTransform(d, 0, 0, d, 0, 0); c.clearRect(0, 0, cvs.clientWidth, cvs.clientHeight); return c; };
// the curve being edited (the applied one until something changes); telemetry never touches it
function fanDraft() {
  const o = FAN.open, i = fanTargets()[0], key = fanDKey(o);
  if (i == null || (o.g && o.i != null)) return null;   // a fan inside its group takes the group's curve
  const D = FAN.draft, now = fanSrc(i) + '|' + fanSer(fanCurve(i));
  // kept while it is being edited; one not touched follows the settings when they change elsewhere (another window, a profile)
  if (D && D.key === key && (D.b === now || D.src + '|' + fanSer(D.pts) !== D.b || Object.keys(D.by).length)) return D;
  return FAN.draft = { key, src: fanSrc(i), pts: fanCurve(i), by: {}, b: now };   // by: the curves of the other sources picked meanwhile
}
const fanDirty = D => { const i = D && fanTargets()[0]; if (i == null || D.key !== fanDKey(FAN.open)) return !!D && D.dirty;
  return D.dirty = D.src !== fanSrc(i) || fanSer(D.pts) !== fanCurveRaw(i) || Object.keys(D.by).some(s => s !== D.src && D.by[s] !== fanCurveOf(i, s)); };
function drawFanCurve() {
  const cvs = $('#fs-cv'), tg = fanTargets(), D = fanDraft(); if (!D || !cvs.offsetParent) return;
  const sc = fanScale(D.src), g = fanGeo(cvs, sc), c = fanFit(cvs), step = sc.hi - sc.lo > 50 ? 20 : 10, x0 = g.px(sc.lo), x1 = g.px(sc.hi);
  c.font = '11px system-ui'; c.strokeStyle = THEME.mid; c.lineWidth = 1; c.globalAlpha = .35;
  c.beginPath();
  for (let v = 20; v <= 100; v += 20) { c.moveTo(x0, Math.round(g.py(v)) + .5); c.lineTo(x1, Math.round(g.py(v)) + .5); }
  for (let v = Math.ceil(sc.lo / step) * step; v <= sc.hi; v += step) { c.moveTo(Math.round(g.px(v)) + .5, g.py(20)); c.lineTo(Math.round(g.px(v)) + .5, g.py(100)); }
  c.stroke(); c.globalAlpha = 1; c.fillStyle = THEME.mid;
  c.textAlign = 'right'; c.textBaseline = 'middle';
  for (let v = 20; v <= 100; v += 20) c.fillText(v + '%', x0 - 6, g.py(v));
  c.textAlign = 'center'; c.textBaseline = 'top';
  for (let v = Math.ceil(sc.lo / step) * step; v <= sc.hi; v += step) c.fillText(v + '°', g.px(v), g.py(20) + 7);
  // under each fan's floor: fans.c never sets less (a group's higher floors show darker)
  const pts = D.pts, mins = [...new Set(tg.map(fanMin))].sort((a, b) => a - b);
  mins.forEach(lo => { if (lo > 20) { c.fillStyle = `rgba(${THEME.ink}, .07)`; c.fillRect(x0, g.py(lo), x1 - x0, g.py(20) - g.py(lo)); } });
  // what fans.c sets: the curve first, then the floor (fanOut); its corners are the points and where a slope crosses the floor
  const vis = fanVis(pts, sc), out = lo => { const v = vis, p = [];
    v.forEach(([x, y], k) => { const [a, b] = v[k - 1] || [];
      if (k && (b - lo) * (y - lo) < 0) p.push([g.px(a + (lo - b) * (x - a) / (y - b)), g.py(lo)]);
      p.push([g.px(x), g.py(Math.max(lo, clamp20(y)))]); }); return p; };
  const o0 = out(mins[0]);
  c.beginPath(); o0.forEach(([x, y], k) => k ? c.lineTo(x, y) : c.moveTo(x, y)); c.lineTo(x1, g.py(20)); c.lineTo(x0, g.py(20)); c.closePath();
  c.fillStyle = `rgba(${THEME.ink}, .06)`; c.fill();
  mins.forEach((lo, n) => { const p = n ? out(lo) : o0;
    c.beginPath(); p.forEach(([x, y], k) => k ? c.lineTo(x, y) : c.moveTo(x, y)); c.strokeStyle = THEME.hi; c.lineWidth = 2.5; c.stroke();
    if (mins.length > 1) { c.fillStyle = THEME.hi; c.textAlign = 'left'; c.textBaseline = 'bottom'; c.fillText(t('fans.cv.min', lo), x0 + 4, g.py(lo) - 3); } });
  // the curve as set, before the limits: thin and dashed, the points on it
  const RY = y => g.py(clamp20(y));
  c.beginPath(); vis.forEach(([x, y], k) => k ? c.lineTo(g.px(x), RY(y)) : c.moveTo(g.px(x), RY(y)));
  c.setLineDash([5, 4]); c.strokeStyle = `rgb(${THEME.ink})`; c.globalAlpha = .55; c.lineWidth = 1.25; c.stroke(); c.setLineDash([]); c.globalAlpha = 1;
  const focus = document.activeElement === cvs, k0 = Math.min(pts.length - 1, FAN.sel);
  pts.forEach((p, k) => { const s = k === k0, off = p[0] < sc.lo ? -1 : p[0] > sc.hi ? 1 : 0, X = off < 0 ? x0 : off ? x1 : g.px(p[0]), Y = RY(p[1]);
    c.fillStyle = s && focus ? `rgb(${THEME.ink})` : THEME.hi; c.beginPath();
    // past the range: an arrow inside the chart pointing at the edge, and its °C (clear of the % labels on the left)
    if (off) { c.moveTo(X, Y); c.lineTo(X - off * 8, Y - 5); c.lineTo(X - off * 8, Y + 5); c.fill();
      c.textAlign = off > 0 ? 'right' : 'left'; const up = p[1] < 85; c.textBaseline = up ? 'bottom' : 'top'; c.fillText(p[0] + '°', X - off * 2, up ? Y - 6 : Y + 6); }
    else { c.arc(X, Y, s ? 7 : 5, 0, 6.283); c.fill(); }
    if (s) { c.strokeStyle = `rgb(${THEME.ink})`; c.lineWidth = 1.5; c.beginPath(); c.arc(X - off * 6, Y, 10, 0, 6.283); c.stroke(); } });
  // where each fan on this source is now: the temperature and the speed the QUADRO reports
  const T = fanTemp(D.src);
  if (T != null) tg.forEach(j => { const f = fanCh(j); if (f.pct == null || fanSrc(j) !== D.src || ((S.fans || {}).link === 0 && fanQd(f))) return;
    c.strokeStyle = `rgb(${THEME.ink})`; c.setLineDash([3, 3]); c.lineWidth = 1; c.beginPath(); c.moveTo(g.px(T) + .5, g.py(20)); c.lineTo(g.px(T) + .5, g.py(100)); c.stroke(); c.setLineDash([]);
    c.lineWidth = 2; c.beginPath(); c.arc(g.px(T), g.py(clamp20(f.pct)), 7, 0, 6.283); c.stroke(); });
  fanPoint(pts);
}
// the chosen point's exact values, bounded by its neighbours; the buttons that add and take away points
function fanPoint(pts) {
  const k = Math.min(pts.length - 1, FAN.sel), x = $('#fs-pt-x'), y = $('#fs-pt-y');
  x.min = k ? pts[k - 1][0] + 1 : 0; x.max = k < pts.length - 1 ? pts[k + 1][0] - 1 : 110;
  if (document.activeElement !== x) x.value = pts[k][0];
  if (document.activeElement !== y) y.value = pts[k][1];
  $('#fs-pt-n').textContent = t('fans.pt', k + 1, pts.length);
  $('#fs-pt-add').disabled = pts.length >= 8 || !fanGap(pts, k);
  $('#fs-pt-del').disabled = pts.length <= 2;
}
// the last five minutes of one channel, each line on or off (FAN.hl): asked (dashed), read back (solid), rpm (grey), the
// temperature of the source in use now (dotted, broken where the source changed); marks: source changed, no link, emergency
function drawFanHist(i) {
  const cvs = $('#fs-hist'); if (!cvs.offsetParent) return;
  const c = fanFit(cvs), W = cvs.clientWidth, H = cvs.clientHeight, h = FAN.hist[i] || [], now = Date.now(), src = fanSrc(i), sc = fanScale(src);
  const L = 32, R = W - 30, T = 6, B = H - 16, X = at => R - (now - at) / FAN_HIST * (R - L), Y = v => B - Math.min(1, Math.max(0, v)) * (B - T);
  const rmax = Math.max(500, ...h.map(p => p.r || 0));
  // the axes: % on the left, the source's °C on the right, minutes along the bottom
  c.font = '10px ' + getComputedStyle(cvs).fontFamily; c.fillStyle = THEME.mid; c.strokeStyle = THEME.lo; c.lineWidth = 1;
  c.globalAlpha = .4; [0, .5, 1].forEach(v => { c.beginPath(); c.moveTo(L, Math.round(Y(v)) + .5); c.lineTo(R, Math.round(Y(v)) + .5); c.stroke(); }); c.globalAlpha = 1;
  c.textBaseline = 'middle'; c.textAlign = 'right'; [0, 50, 100].forEach(v => c.fillText(v + '%', L - 4, Y(v / 100)));
  c.textAlign = 'left'; [sc.lo, (sc.lo + sc.hi) / 2, sc.hi].forEach((v, k) => c.fillText(v + '°', R + 4, Y(k / 2)));
  c.textBaseline = 'alphabetic';
  for (let m = 5; m >= 0; m--) { if (W < 420 && (m === 4 || m === 2 || m === 1)) continue;
    c.textAlign = m === 5 ? 'left' : m ? 'center' : 'right'; c.fillText(m ? t('fans.hist.ago', m) : t('fans.hist.now'), X(now - m * 60000), H - 3); }
  // the marks: no link (grey to the next reading), emergency (red along the bottom), another source (a dotted line and its name)
  let was = null;
  h.forEach((p, k) => { const n = h[k + 1], x = X(p.at), x2 = n && n.at - p.at <= 6000 ? X(n.at) : x + 2;
    if (p.lost) { c.fillStyle = `rgba(${THEME.ink}, .1)`; c.fillRect(x, T, Math.max(2, (n ? X(n.at) : R) - x), B - T); return; }
    if (p.em) { c.fillStyle = '#ff8a8a'; c.fillRect(x, B - 3, Math.max(2, x2 - x), 3); }
    if (was && was !== p.src) { c.strokeStyle = THEME.mid; c.setLineDash([2, 3]); c.beginPath(); c.moveTo(Math.round(x) + .5, T); c.lineTo(Math.round(x) + .5, B); c.stroke(); c.setLineDash([]);
      c.fillStyle = THEME.mid; c.textAlign = x > R - 60 ? 'right' : 'left'; c.fillText(fanSrcName(p.src), x + (x > R - 60 ? -3 : 3), T + 9); }
    was = p.src; });
  const line = (k, val, col, dash, w) => { if (!FAN.hl[k]) return; c.strokeStyle = col; c.setLineDash(dash); c.lineWidth = w; c.beginPath(); let up = 0, prev = null;
    h.forEach(p => { const v = p.lost ? null : val(p); if (v == null || !prev || p.at - prev.at > 6000 || p.src !== prev.src) up = 0; prev = p; if (v == null) return;
      up ? c.lineTo(X(p.at), Y(v)) : c.moveTo(X(p.at), Y(v)); up = 1; });
    c.stroke(); c.setLineDash([]); };
  line('r', p => p.r == null ? null : p.r / rmax, THEME.mid, [], 1.5);
  line('t', p => p.src === src && p.t != null ? (p.t - sc.lo) / (sc.hi - sc.lo) : null, `rgb(${THEME.ink})`, [1.5, 2.5], 1.5);
  line('w', p => p.w == null ? null : p.w / 100, THEME.hi, [5, 3], 1.5);
  line('p', p => p.p == null ? null : p.p / 100, THEME.hi, [], 2);
  // under the pointer: the nearest reading and how long ago; else the last one
  const hv = FAN.hov != null && h.length ? h.reduce((a, p) => Math.abs(X(p.at) - FAN.hov) < Math.abs(X(a.at) - FAN.hov) ? p : a) : null;
  if (hv) { c.strokeStyle = `rgb(${THEME.ink})`; c.globalAlpha = .5; c.beginPath(); c.moveTo(Math.round(X(hv.at)) + .5, T); c.lineTo(Math.round(X(hv.at)) + .5, B); c.stroke(); c.globalAlpha = 1; }
  $('#fs-legend').textContent = fanHistText(hv || h[h.length - 1], hv ? now - hv.at : 0, src);
}
const fanHistText = (p, ago, src) => { if (!p) return '';
  const s = Math.round(ago / 1000), when = s ? t('fans.hist.at', Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0')) : '';
  if (p.lost) return [when, t('fans.hist.lost')].filter(Boolean).join(' · ');
  return [when, p.t != null ? (p.src !== src ? fanSrcName(p.src) + ' ' : '') + fanDeg(p.t) : '', (p.r ?? '—') + ' ' + t('fans.rpmu'),
    p.w != null ? t('fans.hist.w', p.w) : '', t('fans.hist.p', p.p ?? '—'), p.em ? t('fans.em.' + p.em) : ''].filter(Boolean).join(' · '); };
const fanHistNow = () => { const tg = fanTargets(); if (tg.length === 1) drawFanHist(tg[0]); };
// what a status adds: one sample per channel at most every 2 s, and how long a driven fan has shown no rpm; a reading
// older than 5 s is not drawn as now, a lost link leaves one mark, a GPU temperature older than 5 s none
function fanSample(F) {
  if (F === FAN.last) return; FAN.last = F;
  const now = Date.now();
  (F.f || []).forEach((f, i) => { if (!f) return;
    const lost = F.link === 0 && fanQd(f);
    FAN.zero[i] = !lost && f.own && f.pct >= 20 && !f.rpm ? (FAN.zero[i] || 0) + 1 : 0;
    const h = FAN.hist[i] = FAN.hist[i] || [];
    while (h.length && now - h[0].at > FAN_HIST) h.shift();
    const l = h[h.length - 1];
    if (lost) { if (!l || !l.lost) h.push({ at: now, lost: 1 }); return; }
    if ((fanQd(f) && (F.age || 0) > 5000) || (l && !l.lost && now - l.at < 2000)) return;
    const src = fanSrc(i);
    h.push({ at: now, src, t: src === 'gpu' && (F.gage || 0) > 5000 ? null : fanTemp(src), r: f.rpm == null || f.rpm < 0 ? null : f.rpm, p: f.pct == null || f.pct < 0 ? null : Math.round(f.pct),
      w: f.own && f.want >= 0 ? f.want : null, em: f.em || '' });
  });
}
// the panel: a tile per group (its fans open on their own inside it) and per fan not in one
function renderFanPanel() {
  const gs = fanGroups(), inG = new Set(gs.flatMap(g => g.m)), solo = fanAll().filter(i => !fanHidden(i) && !inG.has(i));
  const o = FAN_WIDE.matches && FAN.open || {}, on = x => x ? ' on' : '';
  const grp = g => { const fold = !!FAN.fold[g.n];
    return `<div class="card fan-tile${on(o.g === g.n && o.i == null)}" data-g="${g.n}"><div class="fan-gh"><button type="button" class="fan-hd">${fanTileIcon(g.m)}<b title="${esc(g.name)}">${esc(g.name)}</b><small>${esc(g.m.length ? fanMode(g.m[0]) : t('fans.empty'))}${g.m.length ? `<span data-live-t="${g.m[0]}"></span>` : ''}</small></button>` +
      (g.m.length ? `<button type="button" class="fan-fold" data-fold="${g.n}" aria-expanded="${!fold}" title="${esc(t(fold ? 'fans.unfold' : 'fans.fold'))}" aria-label="${esc(t(fold ? 'fans.unfold' : 'fans.fold'))}">${fold ? '▸' : '▾'}</button>` : '') + '</div>' +
      (fold ? '' : g.m.map(i => `<button type="button" class="fan-row fan-ln fan-mem${on(o.g === g.n && o.i === i)}" data-gm="${i}" data-live-f="${i}"></button>`).join('')) + '</div>'; };
  const html = gs.map(grp).join('') + solo.map(i => `<div class="card fan-tile${on(!o.g && o.i === i)}" data-f="${i}"><button type="button" class="fan-hd">${fanTileIcon([i])}<b title="${esc(fanName(i))}">${esc(fanName(i))}</b><small>${esc(fanMode(i))}<span data-live-t="${i}"></span></small><span class="fan-row fan-ln" data-live-f="${i}"></span></button></div>`).join('');
  const sig = html + LANG;
  if (FAN.sig !== sig) {   // rebuilt only when something on it changes, so a status keeps focus where it is
    const a = document.activeElement, fk = a && $('#fan-panel').contains(a) && (a.dataset.gm != null ? `[data-gm="${a.dataset.gm}"]` : a.dataset.fold ? `[data-fold="${a.dataset.fold}"]` : a.closest('[data-g]') ? `[data-g="${a.closest('[data-g]').dataset.g}"] .fan-hd` : a.closest('[data-f]') ? `[data-f="${a.closest('[data-f]').dataset.f}"] .fan-hd` : '');
    FAN.sig = sig; $('#fan-panel').innerHTML = html;
    if (fk) { const b = $('#fan-panel').querySelector(fk); if (b) b.focus(); }
  }
  $$('#fan-panel [data-live-f]').forEach(el => { el.innerHTML = fanLine(+el.dataset.liveF, !!el.closest('[data-g]')); });
  // a curve's source temperature beside its name in the tile's header
  $$('#fan-panel [data-live-t]').forEach(el => { const i = +el.dataset.liveT, T = cv('fans', fanKey(i), '') === 'curve' ? fanTemp(fanSrc(i)) : null;
    el.textContent = T == null ? '' : ' ' + fanDeg(T); });
  $$('#fan-panel .fan-tile').forEach(el => { const ic = el.querySelector('.fan-hd > .fanic'), g = gs.find(x => String(x.n) === el.dataset.g);
    if (ic) ic.classList.toggle('on', fanRuns(g ? g.m : [+el.dataset.f])); });
  $('#fan-add').classList.toggle('hidden', gs.length >= FAN_GROUPS);
  const off = fanAll().filter(fanHidden);
  $('#fan-removed').classList.toggle('hidden', !off.length);
  $('#fan-removed').innerHTML = `<span>${esc(t('fans.removed'))}</span> ` + off.map(i => `<button class="btn small ghost" data-back="${i}" title="${esc(t('fans.back'))}">${esc(fanName(i))} ↩</button>`).join(' ');
}
// Apply / Cancel under the curve, and after Apply what the QUADRO made of it
function fanDraftBar() {
  const D = FAN.draft, dirty = !!D && D.key === fanDKey(FAN.open) && fanDirty(D), d = FAN.done, msg = $('#fs-applied');
  $('#fs-draft').classList.toggle('hidden', !dirty);
  if (dirty) { $('#fs-draft-msg').textContent = t('fans.draft.dirty'); msg.textContent = ''; return; }
  if (!d || d.key !== fanDKey(FAN.open)) { msg.textContent = ''; msg.className = 'pair-msg'; return; }
  // a status from before Apply says nothing about it: wait for one at least a control round later
  const tg = fanTargets(), fresh = S.fans !== d.F && Date.now() - d.at > 2500, st = tg.map(fanState), bad = st.find(s => s[1] === 'bad') || st.find(s => s[1] === 'warn');
  msg.textContent = !fresh && !(bad && bad[1] === 'bad') ? t('fans.draft.sent') : bad ? t('fans.draft.done') + ' · ' + bad[0] : t('fans.draft.ok');
  msg.className = 'pair-msg' + (bad && fresh || bad && bad[1] === 'bad' ? ' ' + bad[1] : '');
}
function fanFlash() {
  const b = $('#fs-draft'); b.classList.remove('flash'); void b.offsetWidth; b.classList.add('flash');
  $('#fs-draft-msg').textContent = t('fans.draft.first'); b.scrollIntoView({ block: 'nearest' }); $('#fs-apply').focus();
}
// the settings for the open fan or group: beside the panel, or in the sheet
function fanSheet() {
  const o = FAN.open; if (!o) return;
  const g = o.g && fanGroups().find(x => x.n === o.g), mem = !!g && o.i != null;
  if (!fanValid(o)) { FAN.open = null; if (SHEET.kind === 'fan') closeSheet(); return; }
  const tg = fanTargets(), i = tg[0], F = S.fans || {}, has = i != null;
  $('#fs-crumb').classList.toggle('hidden', !mem);
  if (mem) $('#fs-up').textContent = g.name;
  $('#fs-title').textContent = g && !mem ? g.name : fanName(o.i);
  // a group lists its fans; each opens on its own, still inside the group
  $('#fs-live').innerHTML = !tg.length ? `<div class="fan-row">${esc(t('fans.empty'))}</div>` : g && !mem
    ? tg.map(j => `<button type="button" class="fan-row fan-mem" data-gm="${j}">${fanRow(j, true)}</button>`).join('')
    : `<div class="fan-row">${fanRow(i, false)}</div>`;
  $('#fs-live').classList.toggle('stale', F.link === 0 && tg.some(j => fanQd(fanCh(j)))); $('#fs-live').classList.toggle('g', !!g && !mem);
  const now = tg.map(j => { const s = fanNow(j); return s && g && !mem ? fanName(j) + ': ' + s : s; }).filter(Boolean);
  $('#fs-now').textContent = now.join('\n'); $('#fs-now').classList.toggle('hidden', !now.length);
  const nm = $('#fs-name');
  if (document.activeElement !== nm) { nm.value = g && !mem ? g.name : cv('fans', fanKey(o.i, 'name'), ''); nm.placeholder = g && !mem ? t('group.n', g.n) : fanName(o.i); }
  $('#fs-lim-s').textContent = t(g && !mem ? 'fans.lim.g' : 'fans.lim');
  $('#fs-own').classList.toggle('hidden', !!g && !mem);
  $('#fs-mem-f').classList.toggle('hidden', !g || mem);
  if (g && !mem && !g.m.length) $('#fs-lim').open = true;   // an empty group: show where its fans are picked
  $('#fs-shared').classList.toggle('hidden', !mem);
  if (mem) $('#fs-shared-t').textContent = t('fans.shared', g.name, fanMode(o.i));
  if (g && !mem) {   // rebuilt only when something on them changes, so a status keeps focus and the keyboard where they are
    const other = j => fanGroups().find(x => x.n !== g.n && x.m.includes(j));
    const gets = j => g.m.length && !g.m.includes(j) ? t('fans.join', fanMode(g.m[0])) + (fanMin(j) > 20 ? ' · ' + t('fans.join.min', fanMin(j)) : '') : '';
    const ms = $('#fs-members'), list = fanAll().filter(j => !fanHidden(j));
    const html = list.map(j => { const ot = other(j), on = g.m.includes(j), sub = [ot ? t('group.in', ot.name) : '', gets(j)].filter(Boolean).join(' · ');
      return `<button type="button" data-m="${j}" class="${on ? 'on' : ''}" aria-pressed="${on}"><i></i><b>${esc(fanName(j))}</b>${sub ? `<small>${esc(sub)}</small>` : ''}</button>`; }).join('');
    if (ms.dataset.sig !== html + LANG) {
      const fj = document.activeElement && document.activeElement.closest && document.activeElement.closest('#fs-members [data-m]');
      ms.dataset.sig = html + LANG; ms.innerHTML = html;
      if (fj) { const b = ms.querySelector(`[data-m="${fj.dataset.m}"]`); if (b) b.focus(); }
    }
  }
  $('#fs-ctl').classList.toggle('hidden', !has || mem);
  $('#fs-del span').textContent = t(g && !mem ? 'group.delete' : 'fans.remove');
  $('#fs-del-note').classList.toggle('hidden', !!g && !mem);
  $('#fs-hist-f').classList.toggle('hidden', !!g && !mem);
  if (!has) return;
  if (!g || mem) {   // a fan's own: kind, limits, how gently it slows, its history
    const kind = cv('fans', fanKey(i, 'kind'), '');
    $$('#fs-kind button').forEach(b => { b.classList.toggle('on', b.dataset.v === kind); b.setAttribute('aria-pressed', b.dataset.v === kind); });
    const lk = $('#fs-look'), look = fanLook(i), lsig = LANG + look;
    $('#fs-look-f').classList.toggle('hidden', kind === 'pump');
    if (lk.dataset.sig !== lsig) {
      const fl = document.activeElement && lk.contains(document.activeElement) && document.activeElement.dataset.v;
      lk.dataset.sig = lsig;
      lk.innerHTML = FAN_LOOKS.map(v => { const n = t('fans.look.' + (v || 'plain'));
        return `<button type="button" data-v="${v}" class="${v === look ? 'on' : ''}" aria-pressed="${v === look}" title="${esc(n)}">${fanIcon(v, false, true)}<small>${esc(n.split(' (')[0])}</small></button>`; }).join('');
      if (fl != null) { const b = lk.querySelector(`[data-v="${fl}"]`); if (b) b.focus(); }
    }
    ['min', 'fail', 'down'].forEach(k => { const el = $('#fs-' + k); if (document.activeElement !== el) el.value = cv('fans', fanKey(i, k), ''); });
    $('#fs-notach').checked = cv('fans', fanKey(i, 'notach'), '') === '1';
    drawFanHist(i);
  }
  if (mem) return;
  // shared by the group: shown as the group's only where its fans agree
  const diff = fanDiff(tg), mode = diff.includes('mode') ? null : cv('fans', fanKey(i), ''), D = fanDraft();
  $('#fs-diff').classList.toggle('hidden', !diff.length);
  $('#fs-diff').textContent = diff.length ? t('fans.diff', diff.map(k => t('fans.diff.' + k)).join(', ')) : '';
  $$('#fs-mode button').forEach(b => { b.classList.toggle('on', b.dataset.v === mode); b.setAttribute('aria-pressed', b.dataset.v === mode); });
  fanI18n('#fs-mode [data-v=""]', fanAsis(i)); fanI18n('#fs-del-note', i >= FAN_LK ? 'fans.remove.note.lk' : i >= FAN_Q ? 'fans.remove.note.b' : 'fans.remove.note');
  $('#fs-fix').classList.toggle('hidden', mode !== 'fix');
  $('#fs-curve').classList.toggle('hidden', mode !== 'curve');
  const lo = tg.some(j => fanCh(j).lk && fanCh(j).pump) ? 50 : 20, pct = Math.max(lo, +cv('fans', fanKey(i, 'pct'), '50') || 50);   // fans.c: a LINK pump never under 50
  $('#fs-pct').min = lo; setRange($('#fs-pct'), pct); $('#fs-pct-val').textContent = diff.includes('pct') ? '—' : pct + '%';
  const src = D.src, srcs = ['gpu', ...(F.cpuok ? ['cpu'] : []), ...(F.t || []).map((v, k) => v == null ? '' : 't' + (k + 1)).filter(Boolean)];
  const list = srcs.includes(src) ? srcs : [...srcs, src], box = $('#fs-src'), sig = list.map(fanSrcName).join() + list.join() + LANG;
  if (box.dataset.sig !== sig) { box.dataset.sig = sig; box.innerHTML = list.map(s => `<button type="button" data-v="${esc(s)}">${esc(fanSrcName(s))}<span data-tv></span></button>`).join(''); }
  $$('button', box).forEach(b => { const v = fanTemp(b.dataset.v); b.classList.toggle('on', b.dataset.v === src); b.setAttribute('aria-pressed', b.dataset.v === src);
    b.querySelector('[data-tv]').textContent = v != null ? ' · ' + fanDeg(v) : ''; });
  const sn = $('#fs-src-name'), sens = src !== 'gpu' && src !== 'cpu';
  $('#fs-src-nm').classList.toggle('hidden', !sens);
  if (sens && document.activeElement !== sn) { sn.value = cv('fans', src + '_name', ''); sn.placeholder = t('fans.t', src.slice(1)); }
  $('#fs-bad').classList.toggle('hidden', mode !== 'curve' || tg.every(j => fanParse(fanCurveRaw(j))));
  fanDraftBar(); fanSavedList(D);
  if (mode === 'curve' && !FAN.drag) drawFanCurve();
}
// the board's wording where no QUADRO is connected: data-i18n too, so a language switch keeps it
const fanI18n = (sel, k) => { const el = $(sel); el.dataset.i18n = k; el.textContent = t(k); };
function updateFans() {
  const F = S.fans || {};
  $('#fans-none').classList.toggle('hidden', !!F.on || llOn());   // nothing under haku yet: an iCUE LINK hub not driven shows its card below
  $('#tab-fans .fans-top').classList.toggle('hidden', !F.on);   // a Lian Li receiver alone: its card is the page
  if (!F.on) { $('#fans-status').textContent = t('fans.off'); $('#fans-sensors').textContent = ''; $('#fan-panel').innerHTML = ''; FAN.sig = ''; $('#fan-removed').classList.add('hidden'); $('#fans-back-msg').textContent = ''; $('#fan-main').classList.add('hidden'); if (SHEET.kind === 'fan') closeSheet(); return; }
  $('#fan-main').classList.remove('hidden');
  fanSample(F);
  const age = Math.round((F.age || 0) / 1000);
  // the link down: the page stays, with the last readings marked old
  const lk = (F.f || []).some(f => f && f.lk);
  $('#fans-h').textContent = [F.q ? 'QUADRO' : '', F.chip || '', lk ? 'iCUE LINK' : ''].filter(Boolean).join(' · ');
  const qb = F.q ? '' : !F.chip && lk ? '.lk' : '.b';
  fanI18n('#fans-back', 'fans.back.all' + qb); fanI18n('#fans-note', 'fans.note' + qb);
  $('#fans-status').textContent = F.link === 0 ? t('fans.nolink', age) : F.paused ? t('fans.paused' + qb) : t('fans.on') + (age > 3 ? ' · ' + t('fans.age', age) : '');
  $('#fans-status').classList.toggle('bad', F.link === 0);
  $('#fan-panel').classList.toggle('stale', (F.link === 0 || (F.age || 0) > FAN_STALE) && !F.chip && !lk);   // the board's tiles still read
  const ts = (F.t || []).map((v, k) => v == null ? '' : `${fanSrcName('t' + (k + 1))}: ${fanDeg(v)}`).filter(Boolean);
  $('#fans-sensors').textContent = !F.q ? '' : (ts.length ? t('fans.sensors', ts.join(', ')) : t('fans.nosensors')) + (F.flow > 0 ? ' · ' + t('fans.flow', F.flow) : '');
  $('#fans-dev-t').textContent = [F.q ? 'QUADRO · ' + t('fans.dev.t', F.serial, F.fw) : '', F.chip ? t('fans.dev.b', F.chip) : '', lk ? t('fans.dev.lk', (S.link || {}).fw || '?') : ''].filter(Boolean).join(' · ');
  const own = (F.f || []).filter(f => f && f.own && !f.lk).length;   // the LINK hub is never given back by this button: it keeps haku's default
  // a control loop that stalled (fans.c wd_check): stays said until it runs again; one that could not give back is worse
  const wd = F.wd || [], wk = wd.indexOf(3) >= 0 ? 3 : wd.indexOf(2) >= 0 ? 2 : wd.indexOf(1) >= 0 ? 1 : 0;
  const wdn = [0, 1].filter(k => wd[k] === wk).map(k => k ? t('fans.wd.board') : 'QUADRO').join(', ');
  $('#fans-back-msg').textContent = wk ? t(['', 'fans.wd.back', 'fans.wd.busy', 'fans.wd.fail'][wk], wdn, F.rfail || 1) : !FAN.back ? '' : F.link === 0 ? t('fans.back.nolink') : F.rfail ? t('fans.back.fail', F.rfail) : own ? t('fans.back.wait' + qb) : t('fans.back.ok' + qb);
  $('#fans-back-msg').classList.toggle('bad', wk === 3); $('#fans-back-msg').classList.toggle('warn', wk === 1 || wk === 2);
  const lg = F.log || [];
  $('#fans-log-d').classList.toggle('hidden', !lg.length);
  $('#fans-log-s').textContent = t('fans.log', lg.length);
  $('#fans-log').textContent = fanLogText();
  // a wide window always shows one: the last opened while it is still there, else the first on the panel
  if (FAN_WIDE.matches && !fanValid(FAN.open)) {
    const g = fanGroups()[0], i = fanAll().find(j => !fanHidden(j) && !fanGroups().some(x => x.m.includes(j)));
    FAN.open = g ? { g: g.n } : i != null ? { i } : null; FAN.sel = 0;
  }
  renderFanPanel();
  $('#fan-set').classList.toggle('hidden', !FAN.open);
  if (SHEET.kind === 'fan' || FAN_WIDE.matches) fanSheet();
  if (tab === 'home' && heroFans().length) drawHero();   // the fan widgets' numbers
}
const fanLogText = () => ((S.fans || {}).log || []).map(l => `${l.t} ${l.m}${l.n > 1 ? ' ×' + l.n : ''}`).join('\n');
// open a fan or group; with curve changes not applied, back to them first: switching never drops them
function fanOpen(o, el) {
  const D = FAN.draft;
  if (D && D.key !== fanDKey(o) && fanDirty(D)) { fanShow(el); fanFlash(); return false; }
  if (fanDKey(o) !== fanDKey(FAN.open) || (o.i != null) !== (FAN.open && FAN.open.i != null)) { FAN.sel = 0; FAN.draft = null; FAN.done = null; }
  FAN.open = o; if (o.g && o.i != null) $('#fs-lim').open = true;
  fanShow(el);
  return true;
}
function fanShow(el) {
  if (FAN_WIDE.matches && tab === 'fans') { if (SHEET.kind === 'fan') closeSheet(); }   // beside the panel; from Home always the sheet
  else if (SHEET.kind !== 'fan') { const r = (el || $('#fan-panel')).getBoundingClientRect(); openSheet({ k: 'fan' }, r.left + r.width / 2, r.top + r.height / 2); }
  FAN.sig = ''; updateFans();
  requestAnimationFrame(() => { drawFanCurve(); const tg = fanTargets(); if (tg.length === 1) drawFanHist(tg[0]); });
}
const fanOpenAt = (el, from) => {
  if (el.dataset.gm != null) return fanOpen({ g: +(from.dataset.g || FAN.open.g), i: +el.dataset.gm }, el);
  if (el.dataset.fold) { FAN.fold[el.dataset.fold] = !FAN.fold[el.dataset.fold]; FAN.sig = ''; updateFans(); return; }
  fanOpen(el.dataset.g ? { g: +el.dataset.g } : { i: +el.dataset.f }, el);
};
const fanMemKV = (n, m) => [`g${n}`, [...new Set(m)].sort((a, b) => a - b).map(i => i + 1).join(',')];
const fanMembers = (n, m) => setFans([fanMemKV(n, m)]);
// Apply: the curve and source to every fan it is for; the curves of the sources left are kept under their names
function fanApply() {
  const D = FAN.draft, tg = fanTargets(), cur = D && fanSer(D.pts); if (!D || !tg.length || !fanParse(cur)) return;
  const kv = [];
  tg.forEach(i => {
    const was = fanSrc(i);
    Object.keys(D.by).forEach(s => { if (s !== D.src && s !== was) kv.push([fanKey(i, 'curve_' + s), D.by[s]]); });
    if (was !== D.src) kv.push([fanKey(i, 'curve_' + was), D.by[was] ?? cv('fans', fanKey(i, 'curve'), '')]);
    kv.push([fanKey(i, 'curve'), cur], [fanKey(i, 'src'), D.src]);
  });
  setFans(kv);
  FAN.draft = null; FAN.done = { key: D.key, at: Date.now(), F: S.fans };
  FAN.sig = ''; updateFans();
}
const fanText = v => v.replace(/[\x00-\x1f;]/g, '').trim().slice(0, 24);   // ; starts a comment in haku.ini
// an edit goes to the draft only, and only when it reads by fans.c's rules
const fanEdit = pts => { const D = fanDraft(); if (!D || !pts || !fanParse(fanSer(pts))) return false; D.pts = pts; FAN.done = null; fanDraftBar(); drawFanCurve(); return true; };
// where a point can go next to point k: the middle of the wider free gap beside it
const fanGap = (pts, k) => [[k, k + 1], [k - 1, k]].find(([a, b]) => a >= 0 && b < pts.length && pts[b][0] - pts[a][0] >= 2) ||
  (k === pts.length - 1 && pts[k][0] <= 108 ? [k, k + 1] : k === 0 && pts[0][0] >= 2 ? [-1, 0] : null);
function fanPts(d) {
  const D = fanDraft(); if (!D) return;
  const pts = D.pts.map(p => [...p]), k = Math.min(pts.length - 1, FAN.sel);
  if (d < 0) { if (pts.length <= 2) return; pts.splice(k, 1); FAN.sel = Math.min(k, pts.length - 1); }
  else {
    const gp = pts.length < 8 && fanGap(pts, k); if (!gp) return;
    const [a, b] = gp, x = a < 0 ? Math.floor(pts[0][0] / 2) : b >= pts.length ? Math.min(110, pts[a][0] + 10) : Math.round((pts[a][0] + pts[b][0]) / 2);
    pts.splice(a + 1, 0, [x, Math.max(0, fanAt(pts, x))]); FAN.sel = a + 1;
  }
  fanEdit(pts);
}
// curves kept by name for any channel: [fans] cN_name, cN (the points), cN_k (cpu, gpu or liq: a GPU curve is no use on
// water); taking one changes the draft's points only, never a channel's limits. Before, the CPU and the water shared 't'
// and an entry may have no kind: those are listed apart, can be taken as they are and get their kind by hand, never a guess
const FAN_SAVED = 8, fanSlots = Array.from({ length: FAN_SAVED }, (_, j) => j + 1), FAN_SV_K = ['cpu', 'gpu', 'liq'];
const fanKind = s => s === 'gpu' || s === 'cpu' ? s : 'liq';
const fanSvK = n => { const k = cv('fans', `c${n}_k`, ''); return FAN_SV_K.includes(k) ? k : ''; };
const fanSaved = k => fanSlots.filter(n => cv('fans', `c${n}_name`, '') && fanSvK(n) === k && fanParse(cv('fans', `c${n}`, '')));
function fanSavedList(D) {
  const s = $('#fs-sv'), k = fanKind(D.src), list = fanSaved(k), old = fanSaved(''), all = [...list, ...old];
  const grp = (l, ns) => ns.length ? `<optgroup label="${esc(l)}">${ns.map(n => `<option value="${n}">${esc(cv('fans', `c${n}_name`, ''))}</option>`).join('')}</optgroup>` : '';
  const html = `<option value="">${esc(t(all.length ? 'fans.sv.pick' : 'fans.sv.none'))}</option>` + grp(t('fans.sv.k.' + k), list) + grp(t('fans.sv.k.none'), old);
  if (s.dataset.sig !== html + LANG) { const v = s.value; s.dataset.sig = html + LANG; s.innerHTML = html; s.value = all.includes(+v) ? v : ''; }
  s.disabled = !all.length; s.setAttribute('aria-label', t('fans.sv.pick'));
  $('#fs-sv-name').placeholder = t('fans.sv.ph'); $('#fs-sv-name').setAttribute('aria-label', t('fans.sv.ph'));
  $('#fs-sv-del').disabled = !s.value;
  fanSvKind();
}
// an entry without a kind picked: the choice of one beside it
function fanSvKind() {
  const n = $('#fs-sv').value, e = $('#fs-sv-k'), html = `<option value="">${esc(t('fans.sv.k.set'))}</option>` + FAN_SV_K.map(k => `<option value="${k}">${esc(t('fans.sv.k.' + k))}</option>`).join('');
  e.classList.toggle('hidden', !n || !!fanSvK(n));
  if (e.dataset.sig !== html) { e.dataset.sig = html; e.innerHTML = html; e.setAttribute('aria-label', t('fans.sv.k.set')); }
}

// what acts at load: called by app.js where this part of it once was, after the page and app.js's helpers are there
function fanBind() {
  Object.entries({ '': 't30', ring: 'halo', hub: 'hub', qx: 'qx', sl: 'sl', inf: 'slinf' }).forEach(([look, n]) =>
    fetch(`fan-${n}.svg`, { cache: 'no-cache' }).then(r => r.text()).then(t => {
      const defs = (t.match(/<defs>([\s\S]*?)<\/defs>/) || [])[1] || '';
      let d = $('svg.fan-defs');
      if (!d) { d = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); d.setAttribute('class', 'fan-defs'); d.setAttribute('aria-hidden', 'true'); document.body.append(d); }
      d.insertAdjacentHTML('beforeend', defs);
      FAN_ART[look] = t.replace(/^[\s\S]*?<svg[^>]*>|<\/svg>\s*$/g, '').replace(/<defs>[\s\S]*?<\/defs>/, '').replace(/\sid="[^"]*"/g, '');
      FAN.sig = ''; $('#fs-look').dataset.sig = ''; updateFans();
    }).catch(() => {}));
  FAN_WIDE.addEventListener('change', () => { if (FAN_WIDE.matches && SHEET.kind === 'fan' && tab === 'fans') closeSheet(); FAN.sig = ''; updateFans(); requestAnimationFrame(drawFanCurve); });
  $('#fan-panel').addEventListener('click', e => {
    const el = e.target.closest('[data-gm], [data-fold], .fan-tile'); if (!el) return;
    fanOpenAt(el, el.closest('.fan-tile'));
  });
  // a new group, empty: its fans are picked in its settings
  $('#fan-add').addEventListener('click', e => {
    const n = Array.from({ length: FAN_GROUPS }, (_, k) => k + 1).find(k => !cv('fans', `g${k}_name`, '')); if (!n) return;
    if (FAN.draft && fanDirty(FAN.draft)) { fanShow(e.currentTarget); fanFlash(); return; }
    setCfg('fans', `g${n}`, ''); setCfg('fans', `g${n}_name`, t('group.n', n));
    fanOpen({ g: n }, e.currentTarget);
  });
  $('#fan-removed').addEventListener('click', e => {
    const b = e.target.closest('[data-back]'); if (!b) return;
    setCfg('fans', fanKey(+b.dataset.back, 'hide'), ''); updateFans();
  });
  // every channel back to the QUADRO's own setting; the result is what the core then reports
  $('#fans-back').addEventListener('click', () => {
    setFans(fanAll().filter(i => cv('fans', fanKey(i), '')).map(i => [fanKey(i), '']));
    FAN.back = Date.now(); updateFans();
  });
  $('#fans-log-copy').addEventListener('click', async e => {
    const F = S.fans || {}, b = e.currentTarget, txt = [F.q ? `QUADRO ${F.serial} · fw ${F.fw}` : '', F.chip ? 'board ' + F.chip : ''].filter(Boolean).join(' · ') + '\n' + fanLogText();
    let ok = false; try { await navigator.clipboard.writeText(txt); ok = true; } catch (x) { }
    b.textContent = ok ? '✓' : t('share.copyhand'); setTimeout(() => { b.textContent = t('share.copy'); }, 1500);
  });
  $('#fan-set').addEventListener('click', e => {
    const o = FAN.open; if (!o) return;
    const b = e.target.closest('#fs-mode button, #fs-src button, #fs-kind button, #fs-look button, #fs-members button, #fs-live [data-gm], #fs-up, #fs-shared button, #fs-del, #fs-apply, #fs-cancel, #fs-pt-add, #fs-pt-del, #fs-reset');
    if (!b) return;
    const D = FAN.draft;
    if (b.id === 'fs-apply') { fanApply(); return; }
    if (b.id === 'fs-cancel') { FAN.draft = null; FAN.done = null; FAN.sel = 0; updateFans(); $('#fs-cv').focus(); return; }   // nothing was sent
    if (b.id === 'fs-pt-add' || b.id === 'fs-pt-del') { fanPts(b.id === 'fs-pt-add' ? 1 : -1); return; }
    if (b.id === 'fs-reset') { const P = fanDraft(); if (P) { FAN.sel = 0; fanEdit(fanParse(fanScale(P.src).def)); } return; }
    if (b.dataset.gm != null) { fanOpen({ g: o.g, i: +b.dataset.gm }, b); return; }
    if (b.id === 'fs-up' || b.closest('#fs-shared')) { fanOpen({ g: o.g }, b); return; }
    if (b.id === 'fs-del') {
      if (o.i == null) { setCfg('fans', `g${o.g}`, ''); setCfg('fans', `g${o.g}_name`, ''); }
      else {   // off the panel: back to the QUADRO's own setting (the button says so) and out of its group
        setFans([[fanKey(o.i), ''], [fanKey(o.i, 'hide'), '1'], ...fanGroups().filter(g => g.m.includes(o.i)).map(g => fanMemKV(g.n, g.m.filter(j => j !== o.i)))]);
      }
      if (D && D.key === fanDKey(o)) FAN.draft = null;
      FAN.open = null; if (SHEET.kind === 'fan') closeSheet(); FAN.sig = ''; updateFans(); return;
    }
    if (b.dataset.m != null) {
      const j = +b.dataset.m, gs = fanGroups(), g = gs.find(x => x.n === o.g); if (!g) return;
      if (g.m.includes(j)) fanMembers(g.n, g.m.filter(x => x !== j));
      else {   // a fan joins: out of any other group, and it takes the group's mode, speed and curve; its kind and limits stay its own.
        // One change (setFans): fans.c never runs it in the group's mode with its old source or curve
        setFans([...gs.filter(x => x.n !== g.n && x.m.includes(j)).map(x => fanMemKV(x.n, x.m.filter(y => y !== j))),
          ...(g.m.length ? ['src', 'curve', 'pct', ''].map(k => [fanKey(j, k), cv('fans', fanKey(g.m[0], k), '')]) : []), fanMemKV(g.n, [...g.m, j])]);
      }
    } else if (b.closest('#fs-kind')) setCfg('fans', fanKey(o.i, 'kind'), b.dataset.v);
    else if (b.closest('#fs-look')) setCfg('fans', fanKey(o.i, 'look'), b.dataset.v);
    else if (b.closest('#fs-mode')) { fanSetAll('', b.dataset.v); FAN.back = 0; }
    else {   // another source, in the draft: this one's curve is kept for it, the other's comes back (or its default the first time)
      const P = fanDraft(), i = fanTargets()[0], s = b.dataset.v; if (!P || P.src === s) return;
      P.by[P.src] = fanSer(P.pts); P.pts = fanParse(P.by[s] ?? fanCurveOf(i, s)) || fanParse(fanScale(s).def); P.src = s; FAN.sel = 0; FAN.done = null;
    }
    FAN.sig = ''; updateFans();
  });
  $('#fs-name').addEventListener('change', e => {
    const o = FAN.open; if (!o) return;
    const v = fanText(e.target.value);
    if (o.i == null) setCfg('fans', `g${o.g}_name`, v || t('group.n', o.g));   // an empty name would delete the group
    else setCfg('fans', fanKey(o.i, 'name'), v);
    FAN.sig = ''; updateFans();
  });
  $('#fs-src-name').addEventListener('change', e => {
    const D = fanDraft(); if (!D || D.src === 'gpu') return;
    setCfg('fans', D.src + '_name', fanText(e.target.value)); $('#fs-src').dataset.sig = ''; FAN.sig = ''; updateFans();
  });
  ['#fs-name', '#fs-src-name', '#fs-pt-x', '#fs-pt-y', '#fs-min', '#fs-fail', '#fs-down'].forEach(s => $(s).addEventListener('keydown', e => { if (e.key === 'Enter') e.target.blur(); }));
  $('#fs-pct').addEventListener('input', e => { fanSetAll('pct', e.target.value, true); $('#fs-pct-val').textContent = e.target.value + '%'; });
  // a limit: in its range or empty for the default (20 for the floor, 100 for the emergency speed, 3 for slowing down)
  [['min', 20], ['fail', 20], ['down', 1]].forEach(([k, lo]) => $('#fs-' + k).addEventListener('change', e => {
    const o = FAN.open; if (!o || o.i == null) return;
    const v = e.target.value.trim() === '' || !Number.isFinite(+e.target.value) ? '' : String(Math.min(100, Math.max(lo, Math.round(+e.target.value))));
    e.target.value = v; setCfg('fans', fanKey(o.i, k), v); updateFans();
  }));
  $('#fs-notach').addEventListener('change', e => { const o = FAN.open; if (o && o.i != null) { setCfg('fans', fanKey(o.i, 'notach'), e.target.checked ? '1' : ''); updateFans(); } });
  // the chosen point, typed: kept between its neighbours and within 20..100 %, and said so when it was moved
  ['x', 'y'].forEach((a, ax) => $('#fs-pt-' + a).addEventListener('change', e => {
    const D = fanDraft(); if (!D) return;
    const pts = D.pts.map(p => [...p]), k = Math.min(pts.length - 1, FAN.sel), v = Math.round(+e.target.value), lo = ax ? 20 : +e.target.min, hi = ax ? 100 : +e.target.max;
    let m = '';
    if (e.target.value.trim() !== '' && Number.isFinite(v)) { pts[k][ax] = Math.min(hi, Math.max(lo, v)); if (pts[k][ax] !== v) m = t(ax ? 'fans.pt.ylim' : 'fans.pt.xlim', lo, hi); fanEdit(pts); }
    $('#fs-pt-msg').textContent = m;
    e.target.value = D.pts[k][ax]; drawFanCurve();
  }));
  $('#fs-sv').addEventListener('change', e => { const n = e.target.value; $('#fs-sv-msg').textContent = '';
    if (n) { FAN.sel = 0; fanEdit(fanParse(cv('fans', `c${n}`, ''))); } $('#fs-sv-del').disabled = !n; $('#fs-sv-k').value = ''; fanSvKind(); });
  $('#fs-sv-k').addEventListener('change', e => {
    const n = $('#fs-sv').value, k = e.target.value, D = fanDraft(), m = $('#fs-sv-msg'), name = cv('fans', `c${n}_name`, ''), kn = t('fans.sv.k.' + k); if (!n || !k) return;
    e.value = '';
    if (fanSaved(k).some(j => cv('fans', `c${j}_name`, '') === name)) { m.textContent = t('fans.sv.k.taken', name, kn); return; }
    setCfg('fans', `c${n}_k`, k); m.textContent = t('fans.sv.k.done', name, kn);
    if (D) fanSavedList(D); $('#fs-sv').focus();
  });
  $('#fs-sv-save').addEventListener('click', () => {
    const D = fanDraft(), name = fanText($('#fs-sv-name').value), m = $('#fs-sv-msg'); if (!D) return;
    if (!name) { m.textContent = t('fans.sv.noname'); $('#fs-sv-name').focus(); return; }
    const k = fanKind(D.src), n = fanSlots.find(j => cv('fans', `c${j}_name`, '') === name && fanSvK(j) === k) || fanSlots.find(j => !cv('fans', `c${j}_name`, ''));
    if (!n) { m.textContent = t('fans.sv.full', FAN_SAVED); return; }
    setCfg('fans', `c${n}`, fanSer(D.pts)); setCfg('fans', `c${n}_k`, k); setCfg('fans', `c${n}_name`, name);
    $('#fs-sv-name').value = ''; m.textContent = t('fans.sv.done', name); fanSavedList(D); $('#fs-sv').value = n; $('#fs-sv-del').disabled = false;
  });
  $('#fs-sv-name').addEventListener('keydown', e => { if (e.key === 'Enter') $('#fs-sv-save').click(); });
  $('#fs-sv-del').addEventListener('click', () => { const n = $('#fs-sv').value, D = fanDraft(); if (!n) return;
    const name = cv('fans', `c${n}_name`, ''); ['', '_k', '_name'].forEach(x => setCfg('fans', `c${n}${x}`, ''));
    $('#fs-sv-msg').textContent = t('fans.sv.gone', name); $('#fs-sv').value = ''; if (D) fanSavedList(D); $('#fs-sv').focus(); });
  // the history: lines on and off, the reading under the pointer
  $('#fs-hl').addEventListener('click', e => { const b = e.target.closest('[data-l]'); if (!b) return;
    const on = FAN.hl[b.dataset.l] = !FAN.hl[b.dataset.l]; b.classList.toggle('on', on); b.setAttribute('aria-pressed', on); fanHistNow(); });
  $('#fs-hist').addEventListener('pointermove', e => { FAN.hov = e.offsetX; fanHistNow(); });
  $('#fs-hist').addEventListener('pointerleave', () => { FAN.hov = null; fanHistNow(); });
  // drag a point: x stays between its neighbours and on the chart, y within 20..100 %
  const fcv = $('#fs-cv');
  fcv.addEventListener('pointerdown', e => {
    const D = fanDraft(); if (!D) return;
    const sc = fanScale(D.src), g = fanGeo(fcv, sc), r = fcv.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top, pts = D.pts.map(p => [...p]);
    let k = -1, best = 18 * 18;
    pts.forEach((p, j) => { if (p[0] < sc.lo || p[0] > sc.hi) return;   // past the range: the keys or the fields move it
      const d2 = (g.px(p[0]) - x) ** 2 + (g.py(clamp20(p[1])) - y) ** 2; if (d2 < best) { best = d2; k = j; } });
    if (k < 0) return;
    e.preventDefault(); fcv.focus({ preventScroll: true }); fcv.setPointerCapture(e.pointerId);
    FAN.drag = { k, pts, sc }; FAN.sel = k; drawFanCurve();
  });
  fcv.addEventListener('pointermove', e => {
    const D = FAN.drag; if (!D) return;
    const g = fanGeo(fcv, D.sc), r = fcv.getBoundingClientRect(), p = D.pts;
    const lo = D.k ? p[D.k - 1][0] + 1 : 0, hi = D.k < p.length - 1 ? p[D.k + 1][0] - 1 : 110;
    const x = Math.min(D.sc.hi, Math.max(D.sc.lo, g.ix(e.clientX - r.left)));
    p[D.k] = [Math.round(Math.min(hi, Math.max(lo, x))), Math.round(clamp20(g.iy(e.clientY - r.top)))];
    fanEdit(p.map(q => [...q]));
  });
  ['pointerup', 'pointercancel'].forEach(n => fcv.addEventListener(n, () => { FAN.drag = null; }));
  // keyboard: ←/→ pick a point, ↑/↓ its speed (Shift: by 1), Insert/+ add one, Delete/− take it away
  fcv.addEventListener('keydown', e => {
    const D = fanDraft(); if (!D) return;
    if (e.key === 'Insert' || e.key === '+') { e.preventDefault(); fanPts(1); return; }
    if (e.key === 'Delete' || e.key === '-') { e.preventDefault(); fanPts(-1); return; }
    if (!/^Arrow/.test(e.key)) return;
    e.preventDefault();
    const pts = D.pts.map(p => [...p]), k = Math.min(pts.length - 1, Math.max(0, FAN.sel));
    if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') { FAN.sel = Math.min(pts.length - 1, Math.max(0, k + (e.key === 'ArrowRight' ? 1 : -1))); drawFanCurve(); return; }
    FAN.sel = k; pts[k][1] = clamp20(pts[k][1] + (e.key === 'ArrowUp' ? 1 : -1) * (e.shiftKey ? 1 : 5));
    fanEdit(pts);
  });
  ['focus', 'blur'].forEach(n => fcv.addEventListener(n, drawFanCurve));
}
