// SPDX-License-Identifier: GPL-3.0-only
'use strict';
// haku control settings page. Talks to the core through WebView2 messages:
//   page -> core: JSON strings {cmd: ...}
//   core -> page: {type: 'state' | 'status' | 'frame'}

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const wv = window.chrome && window.chrome.webview;
const send = o => wv && wv.postMessage(JSON.stringify(o));

// effect texts come from i18n.js: fx.<id>, short.<id>, desc.<id>
const fxName = id => t('fx.' + id);
// the ARGB header zone can be given its own name (e.g. "Water block")
const stripName = () => cv('layout', 'strip_name', '') || t('pc.block');
const TABS = ['effects', 'pc', 'nano', 'bulbs', 'devices', 'settings'];
const HOTKEYS = ['next', 'prev', 'off', 'brighter', 'dimmer', 'profile'];
const PRESETS = ['#FF0000', '#FF5A00', '#FFA000', '#FFE000', '#9DFF00', '#00FF6A', '#00FFD5', '#00C8FF',
  '#0068FF', '#2B2BFF', '#7A3CFF', '#B400FF', '#FF00D4', '#FF2D95', '#FFB070', '#FFFFFF'];
const DEFAULT_PAL = ['#00C8FF', '#7A3CFF', '#FF2D95'];
const OFF = '#1c1c1c';
const WIDE = '"Segoe UI Variable Text", "Segoe UI", sans-serif';

let S = { cfg: {}, effects: [], effect: 'flow', brightness: 100, bulbs: [], nano: { ctls: [] }, ext: { devs: [], found: [], kinds: [], scanning: 0 }, autostart: -1 };
let F = { ram: [[], []], gpu: [], board: null, bulbs: [], nano: {}, ext: {} };   // nano: by slot - 1

// Nanoleaf: every paired controller is its own device in a fixed slot (1..8): the first uses [nanoleaf] and
// zone.nanoleaf, the others [nanoleafN] / zone.nanoleafN; the Nanoleaf page shows one of them at a time
const nanoCtls = () => (S.nano && S.nano.ctls) || [];
const nanoOn = () => nanoCtls().length > 0;
let nanoSlot = 0;
const curNano = () => nanoCtls().find(c => c.slot === nanoSlot) || nanoCtls()[0] || null;
const nanoSec = slot => slot > 1 ? 'nanoleaf' + slot : 'nanoleaf';
const nanoZone = slot => 'zone.' + nanoSec(slot);
const nanoKey = slot => nanoSec(slot) + '_enabled';
const nanoName = c => (c && c.name) || 'Nanoleaf';
let tab = 'effects';
const SHEET = { kind: null, moved: [], page: '' };   // the open device sheet (see openSheet)
let dragging = false;

// ------------------------------------------------------------------ config helpers
const cv = (s, k, d) => { const x = S.cfg[s]; return x && x[k] != null && x[k] !== '' ? x[k] : d; };
function setCfg(s, k, v) {
  v = String(v);
  (S.cfg[s] = S.cfg[s] || {})[k] = v;
  send({ cmd: 'set', s, k, v });
  groupSpread(s, k, v, setCfg);
}
const parsePal = str => (str || '').match(/#[0-9a-fA-F]{6}/g)?.map(x => x.toUpperCase()) || [];
const palStr = a => a.join(', ');
// the newer effects' own colours when none are set (as effects.c FX_DEFAULT); the others take the general palette
const FX_DEFAULT_PAL = {
  rainbow: ['#FF0000', '#FFFF00', '#00FF00', '#00FFFF', '#0000FF', '#FF00FF'], wave: ['#FF0050', '#FF9000', '#00FF80', '#0090FF', '#9030FF'], fire: ['#200000', '#C01000', '#FF5000', '#FFB020', '#FFF0A0'],
  ocean: ['#000820', '#003070', '#0070B0', '#00B0D0', '#A0F0FF'], twinkle: ['#FFFFFF', '#FFE8A0', '#A0C8FF'], meteor: ['#FFFFFF', '#80C0FF', '#3040FF'],
  plasma: ['#FF0080', '#8000FF', '#0080FF', '#00FFC0'], aurora: ['#00FF90', '#00C0FF', '#7040FF', '#FF40C0'], ripple: ['#40E0FF', '#4060FF', '#C040FF'],
  matrix: ['#00FF40', '#C0FFC0'], candle: ['#FF7A1A', '#FFB347', '#FF5500'],
};
const effPal = id => { const p = parsePal(cv(id, 'palette')); if (p.length) return p; if (FX_DEFAULT_PAL[id]) return FX_DEFAULT_PAL[id]; const g = parsePal(cv('general', 'palette')); return g.length ? g : DEFAULT_PAL; };

// throttled sender for continuous edits (colour dragging)
const pendingSets = new Map();
let flushTimer = 0;
function setCfgSoon(s, k, v) {
  (S.cfg[s] = S.cfg[s] || {})[k] = String(v);
  groupSpread(s, k, String(v), setCfgSoon);
  pendingSets.set(s + '\u0001' + k, [s, k, String(v)]);
  if (!flushTimer) flushTimer = setTimeout(() => {
    flushTimer = 0;
    for (const [s2, k2, v2] of pendingSets.values()) send({ cmd: 'set', s: s2, k: k2, v: v2 });
    pendingSets.clear();
  }, 40);
}

// ---- groups of devices (the preview on the Effects tab): [group.N] name, members = the devices' zone sections
// (zone.dev3, zone.nanoleaf2, zone.light1, ...). The look keys of [group.N] are also written to every member's
// [zone.*], so changing the group's look changes all of its devices; the core knows nothing about groups.
const GROUP_MAX = 16, GROUP_KEYS = ['mode', 'palette', 'kelvin', 'brightness', 'effect'];
const groupSec = n => 'group.' + n;
function groups() {
  return Object.keys(S.cfg).map(k => /^group\.(\d+)$/.exec(k)).filter(Boolean)
    .map(m => ({ n: +m[1], name: S.cfg[m[0]].name || '', members: (S.cfg[m[0]].members || '').split(',').filter(Boolean) }))
    .filter(g => g.members.length).sort((a, b) => a.n - b.n);
}
const groupOf = zone => groups().find(g => g.members.includes(zone));
const groupTitle = g => g.name || t('group.n', g.n);
function groupSpread(s, k, v, set) {
  const m = /^group\.(\d+)$/.exec(s);
  if (!m || !GROUP_KEYS.includes(k)) return;
  const g = groups().find(x => x.n === +m[1]);
  if (g) g.members.forEach(z => { if (zoneTakes(z, k)) set(z, k, v); });
}
const setMembers = (n, list) => setCfg(groupSec(n), 'members', list.join(','));
// into group n (out of any other); the device takes the group's look
function groupAdd(n, zone) {
  groups().forEach(g => { if (g.n !== n && g.members.includes(zone)) setMembers(g.n, g.members.filter(z => z !== zone)); });
  const g = groups().find(x => x.n === n), list = g ? g.members : [];
  if (!list.includes(zone)) setMembers(n, list.concat(zone));
  GROUP_KEYS.forEach(k => { const v = cv(groupSec(n), k, ''); if (v !== '' && zoneTakes(zone, k)) setCfg(zone, k, v); });
}
function groupRemove(zone) { const g = groupOf(zone); if (g) setMembers(g.n, g.members.filter(z => z !== zone)); }
// a number for a new group, its section emptied (a group without members is gone, its section may be left over)
// Times Frame: the dials Divoom lists for it (haku keeps them in [dev.N] clocks = "id:name|..."), else a number
function frameDials(sec) {
  const cur = cv(sec, 'clock', '');
  const list = cv(sec, 'clocks', '').split('|').map(x => x.split(':')).filter(x => +x[0] > 0);
  if (!list.length) return `<label class="num"><span>${t('dv.dial.id')}</span><input type="text" class="dev-dial" inputmode="numeric" maxlength="8" spellcheck="false" value="${esc(cur)}"></label>`;
  return `<label class="num"><span>${t('dv.dial')}</span><select class="select dev-dial">${cur && !list.some(x => x[0] === cur) ? `<option value="${esc(cur)}" selected>#${esc(cur)}</option>` : ''}${list.map(([id, name]) =>
    `<option value="${esc(id)}"${id === cur ? ' selected' : ''}>${esc(name || '#' + id)}</option>`).join('')}</select></label>`;
}

function groupNew() {
  let n = 0;
  for (let i = 1; i <= GROUP_MAX && !n; i++) if (!groups().some(g => g.n === i)) n = i;
  if (!n) return 0;
  ['name'].concat(GROUP_KEYS).forEach(k => { if (cv(groupSec(n), k, '') !== '') setCfg(groupSec(n), k, ''); });
  return n;
}

// ---- themes: [general] theme = dark (default) | grey | light, as html[data-theme]; the canvas takes the page's
// colours for its frames, labels and tooltip (the hardware it draws stays dark, like the real thing).
// Hidden themes are opened with a code typed next to the switch ([general] unlocked = their names); the codes
// are kept as hashes so the source does not give them away.
const THEMES = ['dark', 'grey', 'light'];
const CODES = { '9kr8dk': 'verity', '1se99dm': 'bubblegum', '4eco40': 'kyoka' };
const THEME = { ink: '255, 255, 255', hi: '#d8d8d8', mid: '#8a8a8a', lo: '#4e4e4e', tip: '#121212' };
const inkA = a => `rgba(${THEME.ink}, ${a})`;
const unlocked = () => cv('general', 'unlocked', '').split(',').filter(v => Object.values(CODES).includes(v));
function codeHash(s) {
  let h = 5381;
  for (const c of s) h = (h * 33 + c.charCodeAt(0)) >>> 0;
  return h.toString(36);
}
function applyTheme() {
  const open = THEMES.concat(unlocked()), want = cv('general', 'theme', 'dark');
  const th = open.includes(want) ? want : 'dark';
  $$('#theme button').forEach(b => b.classList.toggle('hidden', !open.includes(b.dataset.v)));
  $('#code-in').placeholder = t('code.ph');
  if (th === 'dark') delete document.documentElement.dataset.theme; else document.documentElement.dataset.theme = th;
  const cs = getComputedStyle(document.documentElement), g = n => cs.getPropertyValue(n).trim();
  Object.assign(THEME, { ink: g('--ink'), hi: g('--text'), mid: g('--muted'), lo: g('--faint'), tip: g('--menu') });
  $$('#theme button').forEach(b => b.classList.toggle('on', b.dataset.v === th));
  if (th === 'kyoka' && !kyTimer) kyTimer = setTimeout(kyWrite, 0); else if (th !== 'kyoka') { clearTimeout(kyTimer); kyTimer = 0; }
}

// verity (the hidden smiley theme) says something in game-chat style now and then: first a hello, later every few
// minutes a line, friendly at first and less so the longer it stays
const VERITY = {
  en: ["hi! i'm Verity :)", "it's going to rain soon", 'there are diamonds 11 blocks below you', 'your lights look nice today :)',
    'i like it when the lights are on', "don't turn off the lights :)", 'i know which room you are in',
    'why did you close the window yesterday?', "i'm always here", 'look behind you :)'],
  ru: ['привет! я Верити :)', 'скоро пойдёт дождь', 'в 11 блоках под тобой алмазы', 'красивая сегодня подсветка :)',
    'мне нравится, когда свет включён', 'не выключай свет :)', 'я знаю, в какой ты комнате',
    'почему вчера окно было закрыто?', 'я всегда здесь', 'обернись :)'],
  fr: ["salut ! moi c'est Verity :)", 'il va bientôt pleuvoir', 'il y a des diamants 11 blocs sous toi', 'tes lumières sont jolies aujourd\'hui :)',
    "j'aime quand les lumières sont allumées", "n'éteins pas les lumières :)", 'je sais dans quelle pièce tu es',
    'pourquoi as-tu fermé la fenêtre hier ?', 'je suis toujours là', 'regarde derrière toi :)'],
};
let verityNext = 0, verityN = 0;
function veritySays(i) {
  const L = VERITY[LANG] || VERITY.en;
  if (i === undefined) { verityN = Math.min(verityN + 1, L.length - 1); i = 1 + Math.floor(Math.random() * verityN); }
  $$('.vchat').forEach(e => e.remove());
  const d = document.createElement('div');
  d.className = 'vchat'; d.innerHTML = '<b>&lt;Verity&gt;</b> '; d.append(L[i]);
  document.body.appendChild(d);
  setTimeout(() => d.remove(), 7200);
  verityNext = Date.now() + (3 + Math.random() * 4) * 60000;
}
// only haku's own Animations switch stops them, like everything else here (not Windows' animation effects,
// which many turn off for speed)
const stillMotion = () => document.body.classList.contains('calm');

// verity's code: for about three seconds the window breaks down (it shakes, tears, flips colours and themes, jumps
// between tabs, the ball shows both its faces), then the theme is on and verity says hello. Without animations
// the theme just comes on.
let verityBusy = false;
function verityArrives() {
  if (verityBusy) return;
  const done = () => { showTab(back); setTheme('verity'); veritySays(0); };
  const back = tab;
  if (stillMotion()) { done(); return; }
  verityBusy = true;
  const tabs = $$('#nav button').filter(b => b.offsetParent).map(b => b.dataset.tab);
  const looks = ['', 'light', 'grey', 'bubblegum', 'verity'];
  const words = ['h̷̢e̸l̵l̶o̷?', 'V E R I T Y', 'ERR 0x5M1L3', ':)', 'i̸ ̷s̵e̶e̷ ̸y̵o̶u̸', 'l̶e̸t̷ ̵m̶e̴ ̷i̵n̶', '☺☺☺☺☺☺'];
  const pick = a => a[Math.floor(Math.random() * a.length)];
  const root = document.documentElement, body = document.body;
  const ov = document.createElement('div'); ov.className = 'vglitch';
  body.appendChild(ov); body.classList.add('glitching');
  const t0 = Date.now(), len = 3200;
  (function step() {
    const k = (Date.now() - t0) / len;
    if (k >= 1) {
      ov.remove(); body.classList.remove('glitching');
      ['--gx', '--gy', '--sk', '--hue', '--v-rgb2'].forEach(p => body.style.removeProperty(p));
      verityBusy = false; done(); return;
    }
    const hard = k > .55;   // it gets worse towards the end
    const r = (n) => (Math.random() * 2 - 1) * n;
    body.style.setProperty('--gx', r(hard ? 18 : 8).toFixed(1) + 'px');
    body.style.setProperty('--gy', r(hard ? 8 : 3).toFixed(1) + 'px');
    body.style.setProperty('--sk', r(hard ? 6 : 2).toFixed(1) + 'deg');
    body.style.setProperty('--hue', (Math.random() < .3 ? r(180) : 0).toFixed(0) + 'deg');
    body.style.setProperty('--v-rgb2', Math.random() < .6 ? 'drop-shadow(4px 0 0 rgba(255,0,60,.7)) drop-shadow(-4px 0 0 rgba(0,240,255,.7))' : 'hue-rotate(0deg)');
    if (Math.random() < .45) showTab(pick(tabs));
    root.dataset.theme = pick(looks); if (!root.dataset.theme) delete root.dataset.theme;
    if (Math.random() < .5) $('#title').textContent = pick(words);
    ov.innerHTML = '';
    for (let n = 0; n < 2 + Math.floor(Math.random() * (hard ? 6 : 3)); n++) {
      const b = document.createElement('i');
      b.style.top = Math.random() * 100 + '%'; b.style.height = 2 + Math.random() * (hard ? 90 : 40) + 'px';
      b.style.setProperty('--h', Math.floor(Math.random() * 360) + 'deg');
      ov.appendChild(b);
    }
    if (k > .3 && Math.random() < (hard ? .5 : .2)) {
      const f = document.createElement('div');
      f.className = 'vg-face' + (Math.random() < (hard ? .7 : .3) ? ' evil' : '');
      ov.appendChild(f);
    }
    if (Math.random() < .35) { const w = document.createElement('div'); w.className = 'vg-text'; w.textContent = '<Verity> ' + pick(words); ov.appendChild(w); }
    setTimeout(step, 60 + Math.random() * (hard ? 70 : 140));
  })();
}

// the ball in the corner: every half a minute to a minute and a half it lags and turns evil for a moment
(function ballGlitch() {
  setTimeout(() => {
    const w = $('.watermark');
    if (w && document.documentElement.dataset.theme === 'verity' && !document.hidden && !stillMotion()) {
      w.classList.remove('vg'); void w.offsetWidth; w.classList.add('vg');
      setTimeout(() => w.classList.remove('vg'), 2700);
    }
    ballGlitch();
  }, 30000 + Math.random() * 60000);
})();

setInterval(() => {
  if (document.documentElement.dataset.theme !== 'verity' || document.hidden) return;
  if (!verityNext) verityNext = Date.now() + 2 * 60000;
  else if (Date.now() > verityNext) veritySays();
}, 20000);

// kyōka suigetsu (the hidden Bleach theme): entering its code, the spiritual pressure rises, a blade cuts across the
// window, the window cracks like a mirror and the illusion breaks into shards that fall away, showing the theme.
// Afterwards hell butterflies cross the window now and then. Without animations the theme just comes on.
let kyokaBusy = false;
function kyokaArrives() {
  if (kyokaBusy) return;
  if (stillMotion()) { setTheme('kyoka'); return; }
  kyokaBusy = true;
  const body = document.body, rnd = (a, b) => a + Math.random() * (b - a);
  // cracks: jagged lines from the middle out past the edges, and broken rings round the point of impact (pixels)
  const W = innerWidth, H = innerHeight, cx = W / 2, cy = H / 2, R = Math.hypot(W, H) / 2;
  let paths = '';
  for (let i = 0; i < 16; i++) {
    let a = i / 16 * Math.PI * 2 + rnd(-.15, .15), d = `M${cx} ${cy}`, r = 0;
    while (r < R) { r += rnd(.07, .16) * R; a += rnd(-.2, .2); d += ` L${(cx + Math.cos(a) * r).toFixed(0)} ${(cy + Math.sin(a) * r).toFixed(0)}`; }
    paths += `<path pathLength="1" d="${d}" style="--d:${(i % 5) * 60}ms"/>`;
  }
  for (let k = 1; k <= 3; k++) {
    let d = '';
    for (let i = 0; i <= 18; i++) { const a = i / 18 * Math.PI * 2, r = (k * .14 + rnd(-.025, .025)) * R; d += `${i ? 'L' : 'M'}${(cx + Math.cos(a) * r).toFixed(0)} ${(cy + Math.sin(a) * r).toFixed(0)} `; }
    paths += `<path class="ring" pathLength="1" d="${d}" style="--d:${200 + k * 90}ms"/>`;
  }
  const ov = document.createElement('div');
  ov.className = 'kyoka-ov';
  ov.innerHTML = `<div class="ky-press"></div><div class="ky-slash"></div><svg class="ky-cracks" viewBox="0 0 ${W} ${H}">${paths}</svg>
    <div class="ky-name"><b>鏡花水月</b><small>kyōka suigetsu · complete hypnosis</small></div>`;
  body.appendChild(ov); body.classList.add('ky-pressing');
  setTimeout(() => ov.classList.add('cut'), 650);
  setTimeout(() => ov.classList.add('crack'), 900);
  setTimeout(() => {
    // the illusion breaks: shards of dark glass (a jittered grid cut into triangles) fly out from the middle and fall
    setTheme('kyoka');
    body.classList.remove('ky-pressing');
    ov.classList.add('break');
    const N = 7, M = 5, P = [];
    for (let j = 0; j <= M; j++) for (let i = 0; i <= N; i++)
      P.push([i / N * 100 + (i % N ? rnd(-5, 5) : 0), j / M * 100 + (j % M ? rnd(-6, 6) : 0)]);
    const at = (i, j) => P[j * (N + 1) + i];
    const tri = [];
    for (let j = 0; j < M; j++) for (let i = 0; i < N; i++) {
      const a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
      if ((i + j) % 2) tri.push([a, b, c], [a, c, d]); else tri.push([a, b, d], [b, c, d]);
    }
    tri.forEach(t => {
      const s = document.createElement('i');
      s.className = 'ky-shard';
      s.style.clipPath = `polygon(${t.map(p => p[0].toFixed(1) + '% ' + p[1].toFixed(1) + '%').join(',')})`;
      const mx = (t[0][0] + t[1][0] + t[2][0]) / 3 - 50, my = (t[0][1] + t[1][1] + t[2][1]) / 3 - 50, dist = Math.hypot(mx, my);
      s.style.transformOrigin = `${(mx + 50).toFixed(1)}% ${(my + 50).toFixed(1)}%`;
      ov.appendChild(s);
      s.animate([{ transform: 'none', opacity: 1 },
                 { transform: `translate(${(mx * rnd(.5, 1.1)).toFixed(1)}vw, ${(my * rnd(.4, .9) + rnd(40, 90)).toFixed(1)}vh) rotate(${rnd(-160, 160).toFixed(0)}deg) scale(${rnd(.6, .95).toFixed(2)})`, opacity: 0 }],
                { duration: rnd(900, 1500), delay: dist * 9 + rnd(0, 120), easing: 'cubic-bezier(.4,0,.8,.6)', fill: 'forwards' });
    });
  }, 1900);
  setTimeout(() => { ov.remove(); kyokaBusy = false; hellButterflies(4); }, 3900);
}
// hell butterflies (jigokuchō): black, with a violet glow, crossing the window on a wavering path, head first,
// each wing beating on its own
function hellButterflies(n) {
  for (let k = 0; k < n; k++) {
    const f = document.createElement('i');
    f.className = 'ky-fly';
    f.innerHTML = '<b class="l"></b><b class="r"></b>';
    f.style.setProperty('--beat', (.18 + Math.random() * .1).toFixed(2) + 's');
    document.body.appendChild(f);
    const W = innerWidth, H = innerHeight, fromLeft = Math.random() < .5;
    const x0 = fromLeft ? -50 : W + 50, x1 = fromLeft ? W + 50 : -50;
    const y0 = H * (.45 + Math.random() * .5), y1 = H * (.05 + Math.random() * .45);
    const amp = 30 + Math.random() * 50, waves = 2 + Math.random() * 2, ph = Math.random() * 6.3;
    const at = t => [x0 + (x1 - x0) * t, y0 + (y1 - y0) * t + Math.sin(t * waves * 6.283 + ph) * amp];
    const frames = [];
    for (let s = 0; s <= 48; s++) {
      const t = s / 48, [x, y] = at(t), [nx, ny] = at(Math.min(1, t + .01)), [px, py] = at(Math.max(0, t - .01));
      const head = Math.atan2(ny - py, nx - px) * 180 / Math.PI + 90;   // the drawing faces up
      frames.push({ transform: `translate(${x.toFixed(0)}px, ${y.toFixed(0)}px) rotate(${head.toFixed(0)}deg)` });
    }
    const dur = 10000 + Math.random() * 5000, delay = k * 800 + Math.random() * 600;
    f.animate(frames, { duration: dur, delay, easing: 'linear', fill: 'both' });
    setTimeout(() => f.remove(), dur + delay + 300);
  }
}
(function butterflyTimer() {
  setTimeout(() => {
    if (document.documentElement.dataset.theme === 'kyoka' && !document.hidden && !stillMotion()) hellButterflies(1 + Math.floor(Math.random() * 2));
    butterflyTimer();
  }, 45000 + Math.random() * 60000);
})();

// kyōka suigetsu: 鏡花水月 down the right edge, written with a brush. Each sign is the font's own glyph, uncovered
// by a mask of its strokes drawn one after another in the usual stroke order (rough centre lines in a 100 x 100
// box, as wide as a brush); then the name stays, fades and is written again.
const KY_STROKES = {
  '鏡': ['M24 6 L7 30', 'M24 6 L40 26', 'M12 36 L36 36', 'M10 50 L38 50', 'M24 36 L24 88', 'M13 62 L18 73', 'M35 60 L30 73', 'M6 91 L40 82',
         'M69 3 L71 12', 'M50 17 L90 17', 'M58 23 L62 32', 'M83 22 L77 32', 'M46 37 L95 37',
         'M54 43 L54 65', 'M54 43 L84 43 L84 65', 'M56 54 L82 54', 'M56 65 L82 65', 'M64 67 Q62 86 46 95', 'M76 67 L76 88 Q78 95 95 92'],
  '花': ['M8 22 L92 22', 'M32 7 L35 37', 'M67 7 L64 37', 'M42 40 L16 72', 'M28 56 L28 95', 'M82 44 L54 66', 'M58 40 L58 82 Q60 93 90 90'],
  '水': ['M50 6 L50 90 L41 83', 'M14 36 L40 36 L14 76', 'M80 26 L57 48', 'M55 46 L90 88'],
  '月': ['M30 12 L30 68 Q28 86 13 94', 'M30 12 L72 12 L72 88 L61 82', 'M32 38 L70 38', 'M32 61 L70 61'],
};
let kyTimer = 0;
function kyScript() {
  const box = $('.ky-script');
  if (!box || box.dataset.built) return box;
  box.dataset.built = '1';
  const defs = `<defs><linearGradient id="ky-grad" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#f6f0ff"/><stop offset=".45" stop-color="#c3a2ff"/><stop offset="1" stop-color="#7344e0"/></linearGradient>
    <filter id="ky-ink" x="-10%" y="-10%" width="120%" height="120%"><feTurbulence type="fractalNoise" baseFrequency="1.1" numOctaves="2" seed="7" result="n"/>
    <feDisplacementMap in="SourceGraphic" in2="n" scale="3.5" xChannelSelector="R" yChannelSelector="G" result="d"/>
    <feColorMatrix in="n" type="matrix" values="0 0 0 0 1  0 0 0 0 1  0 0 0 0 1  0 0 0 -.9 1.4" result="g"/><feComposite in="d" in2="g" operator="in"/></filter></defs>`;
  box.innerHTML = [...'鏡花水月'].map((c, n) => `<svg viewBox="0 0 100 100">${n ? '' : defs}<mask id="ky-m${n}" maskUnits="userSpaceOnUse" x="-10" y="-10" width="120" height="120">${
    KY_STROKES[c].map(d => `<path pathLength="1" d="${d}"/>`).join('')}</mask><text x="50" y="52" fill="url(#ky-grad)" filter="url(#ky-ink)" mask="url(#ky-m${n})">${c}</text></svg>`).join('');
  return box;
}
function kyWrite() {
  clearTimeout(kyTimer); kyTimer = 0;
  const box = $('.ky-script');
  if (!box || document.documentElement.dataset.theme !== 'kyoka') return;
  kyScript();
  box.getAnimations({ subtree: true }).forEach(a => a.cancel());
  const paths = $$('.ky-script mask path');
  if (stillMotion()) { paths.forEach(p => { p.style.strokeDashoffset = 0; }); return; }
  paths.forEach(p => { p.style.strokeDashoffset = ''; });
  let t = 400, last = null;
  paths.forEach(p => {
    const svg = p.closest('svg');
    if (last && svg !== last) t += 350;   // a breath between the signs
    last = svg;
    const dur = 160 + p.getTotalLength() * 5.5;
    p.animate([{ strokeDashoffset: 1 }, { strokeDashoffset: 0 }], { duration: dur, delay: t, easing: 'cubic-bezier(.45, .05, .3, 1)', fill: 'both' });
    t += dur + 60;
  });
  const hold = 4500, fade = 1600;
  box.animate([{ opacity: .5 }, { opacity: .5, offset: (t + hold) / (t + hold + fade) }, { opacity: 0 }], { duration: t + hold + fade, fill: 'forwards' });
  kyTimer = setTimeout(kyWrite, t + hold + fade + 900);
}
document.addEventListener('visibilitychange', () => { if (!document.hidden && document.documentElement.dataset.theme === 'kyoka' && !kyTimer) kyWrite(); });

// ------------------------------------------------------------------ colour helpers
const hex2rgb = h => [parseInt(h.slice(1, 3), 16), parseInt(h.slice(3, 5), 16), parseInt(h.slice(5, 7), 16)];
const rgb2hex = (r, g, b) => '#' + [r, g, b].map(v => Math.round(Math.max(0, Math.min(255, v))).toString(16).padStart(2, '0')).join('').toUpperCase();
const mix = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
const scale = (a, k) => [a[0] * k, a[1] * k, a[2] * k];
const css = a => `rgb(${a[0] | 0},${a[1] | 0},${a[2] | 0})`;
function hsv2rgb(h, s, v) {
  const f = n => { const k = (n + h / 60) % 6; return v - v * s * Math.max(0, Math.min(k, 4 - k, 1)); };
  return [f(5) * 255, f(3) * 255, f(1) * 255];
}
function rgb2hsv(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
  let h = 0;
  if (d) h = mx === r ? ((g - b) / d) % 6 : mx === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return [(h * 60 + 360) % 360, mx ? d / mx : 0, mx];
}
function kelvinRgb(k) {
  const t = k / 100;
  let r, g, b;
  if (t <= 66) { r = 255; g = 99.47 * Math.log(t) - 161.12; } else { r = 329.7 * Math.pow(t - 60, -0.1332); g = 288.12 * Math.pow(t - 60, -0.0755); }
  b = t >= 66 ? 255 : t <= 19 ? 0 : 138.52 * Math.log(t - 10) - 305.04;
  return [r, g, b].map(v => Math.max(0, Math.min(255, v)));
}
const lum = h => { const [r, g, b] = hex2rgb(h); return (r * 0.3 + g * 0.59 + b * 0.11) / 255; };

// ------------------------------------------------------------------ ranges
function fill(el) { const p = (el.value - el.min) / (el.max - el.min) * 100; el.style.setProperty('--p', p + '%'); }
function setRange(el, v) { if (!el || (dragging && document.activeElement === el)) return; el.value = v; fill(el); }
document.addEventListener('input', e => { if (e.target.classList.contains('range')) fill(e.target); });
document.addEventListener('pointerdown', e => { if (e.target.type === 'range') dragging = true; });
document.addEventListener('pointerup', () => { dragging = false; });

// ------------------------------------------------------------------ navigation
// Sidebar: only the tabs for hardware this PC has (Effects, PC, Devices and Settings always; the open tab stays too).
// PC stays because its hardware list and OpenRGB set-up are for every PC, not only MSI boards and ENE memory.
// Nanoleaf and AiDot are reached from Devices until they are set up.
const NAV_NEED = { nano: nanoOn, bulbs: () => S.bulbs.length > 0 };

// ---- a tab for every brand of lights added (as Nanoleaf has its own): their device cards move there. PC hardware
// (OpenRGB, Wooting, the Nanoleaf desk dock) and everything to find and add devices stay under Devices.
const BRAND_ICON = {
  panel: '<rect x="5" y="3" width="14" height="10" rx="1.5"/><path d="M12 13v6M8 21h8"/>',
  bar: '<rect x="3" y="10" width="18" height="4" rx="2"/><path d="M6 17v2M18 17v2"/>',
  strip: '<path d="M3 15c3-6 6 6 9 0s6 6 9 0"/>',
  grid: '<rect x="4" y="4" width="16" height="16" rx="2"/><path d="M9.3 4v16M14.7 4v16M4 9.3h16M4 14.7h16"/>',
  bulb: '<path d="M9 18h6M10 21h4M12 3a6 6 0 0 0-3.5 10.9c.6.5 1 1.2 1 2V16h5v-.1c0-.8.4-1.5 1-2A6 6 0 0 0 12 3z"/>',
  plug: '<path d="M9 3v5M15 3v5M7 8h10v4a5 5 0 0 1-10 0zM12 17v4"/>',
};
const BRANDS = {
  wled: ['wled', 'WLED', 'strip'], govee: ['govee', 'Govee', 'bar'], goveecloud: ['govee', 'Govee', 'bar'], lifx: ['lifx', 'LIFX', 'bulb'],
  yeelight: ['yeelight', 'Yeelight', 'bulb'], hue: ['hue', 'Philips Hue', 'bulb'], wiz: ['wiz', 'WiZ', 'bulb'], tuya: ['tuya', 'Tuya', 'plug'],
  elgato: ['elgato', 'Elgato', 'panel'], divoom: ['divoom', 'Divoom', 'grid'],
};
const brandOf = kind => BRANDS[kind] || null;   // null: PC hardware, under Devices
const brandTab = d => { const b = brandOf(d.kind); return b ? 'brand-' + b[0] : 'devices'; };
let wantTab = '';   // a brand tab remembered from last time, shown once its devices are there
function buildBrandTabs() {
  const seen = new Map();
  S.ext.devs.forEach(d => { const b = brandOf(d.kind); if (b && !seen.has(b[0])) seen.set(b[0], b); });
  const keys = [...seen.keys()].join(',');
  if ($('#nav').dataset.brands === keys) return;
  $('#nav').dataset.brands = keys;
  $$('#nav [data-brand], .tab[data-brand]').forEach(e => e.remove());
  let after = $('#nav [data-tab="bulbs"]');
  const host = $('#tab-devices').parentElement;
  for (const [key, [, name, icon]] of seen) {
    const b = document.createElement('button');
    b.dataset.tab = 'brand-' + key; b.dataset.brand = key;
    b.innerHTML = `<svg viewBox="0 0 24 24">${BRAND_ICON[icon]}</svg><span>${esc(name)}</span><i></i>`;
    b.addEventListener('click', () => showTab(b.dataset.tab));
    after.after(b); after = b;
    const sec = document.createElement('section');
    sec.className = 'tab'; sec.id = 'tab-brand-' + key; sec.dataset.brand = key;
    sec.innerHTML = `<div class="bulb-grid dev-grid" data-brand-grid="${key}"></div>`;
    host.appendChild(sec);
  }
  if (tab.startsWith('brand-') && !seen.has(tab.slice(6))) showTab('devices');
  else if (wantTab && seen.has(wantTab.slice(6))) { const w = wantTab; wantTab = ''; showTab(w); }
  else updateNav();
}
const brandName = tb => { const b = Object.values(BRANDS).find(x => 'brand-' + x[0] === tb); return b ? b[1] : ''; };
function updateNav() {
  let n = 0;
  $$('#nav button').forEach(b => {
    const need = NAV_NEED[b.dataset.tab], show = !need || !!need() || b.dataset.tab === tab;
    b.classList.toggle('gone', !show);
    b.tabIndex = show ? 0 : -1;
    if (show) b.querySelector('i').textContent = String(++n).padStart(2, '0');
  });
  updateAccounts();
}

// ---- sign-ins and pairing (Devices tab): AiDot hands out its bulbs' local keys only to its own account
const ACC = { shown: false, pending: false };
function updateAccounts() {
  const A = (S.accounts || {}).aidot || {}, nb = S.bulbs.length;
  const nc = nanoCtls(), non = nc.filter(c => c.online).length;
  $('#acc-nano').textContent = !nc.length ? t('acc.nano.none') : nc.length > 1 ? `${nc.map(nanoName).join(', ')} · ${non}/${nc.length}` : t(non ? 'acc.nano.on' : 'acc.nano.off');
  $('[data-go="nano"]').classList.toggle('hidden', nc.length > 0);
  $('#acc-aidot').textContent = nb ? t('acc.aidot.ok', nb) : t('acc.aidot.none');
  $('#aidot-open-t').textContent = t(nb ? 'acc.signin.again' : 'acc.signin');
  const cc = $('#aidot-cc');
  if (!cc.options.length && A.countries) {
    cc.innerHTML = A.countries.map(([c, n]) => `<option value="${c}">${n}</option>`).join('');
    cc.value = cv('aidot', 'country', 'FR');
  }
  const busy = A.state === 1;
  $('#aidot-go').disabled = busy;
  if (ACC.pending && !busy && (A.state === 2 || A.state === 3)) {
    ACC.pending = false;
    if (A.state === 2) { ACC.shown = false; $('#aidot-email').value = ''; }
  }
  $('#aidot-form').classList.toggle('hidden', !ACC.shown);
  $('#aidot-open').classList.toggle('hidden', ACC.shown);
  const m = $('#aidot-msg'), known = A.msg && t('acc.err.' + A.msg) !== 'acc.err.' + A.msg;
  m.textContent = busy ? t('acc.busy') : A.state === 2 ? t('acc.aidot.done', A.found) :
    A.state === 3 ? (known ? t('acc.err.' + A.msg) : t('acc.err.vendor', A.msg)) : '';
  m.classList.toggle('bad', A.state === 3);
  updateTuya(); updateGovee();
}
// Tuya: a cloud project's Access ID / Secret (iot.tuya.com) reads the linked app's lights and their local keys
const TACC = { shown: false, pending: false };
function updateTuya() {
  const T = (S.accounts || {}).tuya || {}, nt = S.ext.devs.filter(d => d.kind === 'tuya').length;
  $('#acc-tuya').textContent = T.state === 2 ? t('acc.tuya.ok', T.found) : nt ? t('acc.tuya.devs', nt) : t('acc.tuya.none');
  $('#tuya-open-t').textContent = t(T.state === 2 || nt ? 'acc.signin.again' : 'acc.signin');
  const rg = $('#tuya-region');
  if (!rg.options.length && T.regions) {
    rg.innerHTML = T.regions.map(([c, n]) => `<option value="${c}">${n}</option>`).join('');
    rg.value = cv('tuya', 'region', 'eu');
  }
  const busy = T.state === 1;
  $('#tuya-go').disabled = busy;
  if (TACC.pending && !busy && (T.state === 2 || T.state === 3)) {
    TACC.pending = false;
    if (T.state === 2) TACC.shown = false;
  }
  $('#tuya-form').classList.toggle('hidden', !TACC.shown);
  $('#tuya-open').classList.toggle('hidden', TACC.shown);
  const m = $('#tuya-msg'), known = T.msg && t('acc.err.' + T.msg) !== 'acc.err.' + T.msg;
  m.textContent = busy ? t('acc.busy') : T.state === 2 ? t('acc.tuya.done', T.found) :
    T.state === 3 ? (known ? t('acc.err.' + T.msg) : t('acc.err.tvendor', T.msg)) : '';
  m.classList.toggle('bad', T.state === 3);
}
// Govee cloud: an API key from the Govee Home app, for devices without LAN Control (AI Sync Box 2...)
const GACC = { shown: false, pending: false };
function updateGovee() {
  const G = (S.accounts || {}).govee || {}, ng = S.ext.devs.filter(d => d.kind === 'goveecloud').length;
  $('#acc-govee').textContent = G.state === 2 ? t('acc.govee.ok', G.found) : ng ? t('acc.tuya.devs', ng) : G.saved ? t('acc.govee.saved') : t('acc.govee.none');
  $('#govee-open-t').textContent = t(G.state === 2 || ng || G.saved ? 'acc.govee.again' : 'acc.govee.go');
  const busy = G.state === 1;
  $('#govee-go').disabled = busy;
  if (GACC.pending && !busy && (G.state === 2 || G.state === 3)) { GACC.pending = false; if (G.state === 2) GACC.shown = false; }
  $('#govee-form').classList.toggle('hidden', !GACC.shown);
  $('#govee-open').classList.toggle('hidden', GACC.shown);
  const m = $('#govee-msg');
  m.textContent = busy ? t('acc.busy') : G.state === 2 ? t('acc.govee.done', G.found) : G.state === 3 ? t('acc.err.' + G.msg) : '';
  m.classList.toggle('bad', G.state === 3);
}
$('#govee-open').addEventListener('click', () => { GACC.shown = true; updateGovee(); setTimeout(() => $('#govee-key').focus(), 30); });
$('#govee-cancel').addEventListener('click', () => { GACC.shown = false; $('#govee-key').value = ''; updateGovee(); });
$('#govee-form').addEventListener('submit', e => {
  e.preventDefault();
  const key = $('#govee-key').value.trim();
  if (!key) return;
  $('#govee-key').value = '';
  GACC.pending = true;
  send({ cmd: 'govee_login', key });
  updateGovee();
});
$('#tuya-open').addEventListener('click', () => { TACC.shown = true; updateTuya(); setTimeout(() => $('#tuya-id').focus(), 30); });
$('#tuya-cancel').addEventListener('click', () => { TACC.shown = false; $('#tuya-secret').value = ''; updateTuya(); });
$('#tuya-form').addEventListener('submit', e => {
  e.preventDefault();
  const id = $('#tuya-id').value.trim(), secret = $('#tuya-secret').value.trim(), region = $('#tuya-region').value;
  if (!id || !secret) return;
  $('#tuya-secret').value = '';
  setCfg('tuya', 'region', region);
  TACC.pending = true;
  send({ cmd: 'tuya_login', region, id, secret });
  updateTuya();
});
$('#aidot-open').addEventListener('click', () => { ACC.shown = true; updateAccounts(); setTimeout(() => $('#aidot-email').focus(), 30); });
$('#aidot-cancel').addEventListener('click', () => { ACC.shown = false; $('#aidot-pass').value = ''; updateAccounts(); });
$('#aidot-form').addEventListener('submit', e => {
  e.preventDefault();
  const email = $('#aidot-email').value.trim(), password = $('#aidot-pass').value, country = $('#aidot-cc').value;
  if (!email || !password) return;
  $('#aidot-pass').value = '';   // the password is not kept anywhere in the page either
  setCfg('aidot', 'country', country);
  ACC.pending = true;
  send({ cmd: 'aidot_login', country, email, password });
  updateAccounts();
});

function showTab(t) {
  closeSheet();
  tab = t;
  $('main').scrollTop = 0;
  $$('#nav button').forEach(b => b.classList.toggle('active', b.dataset.tab === t));
  $$('.tab').forEach(s => s.classList.toggle('active', s.id === 'tab-' + t));
  updateNav();
  $('#nav button.active')?.scrollIntoView({ block: 'nearest', inline: 'nearest' });   // the bottom bar of a narrow window scrolls
  const bn = brandName(t);
  $('#title').textContent = bn || window.t('nav.' + t);
  $('#subtitle').textContent = bn ? window.t('sub.brand', S.ext.devs.filter(d => brandTab(d) === t).length) : window.t('sub.' + t);
  requestAnimationFrame(sizeCanvases);
  $$(`#tab-${t} .card, #tab-${t} .fx`).forEach((c, i) => c.style.setProperty('--i', Math.min(i, 14)));
  try { localStorage.setItem('tab', t); } catch (e) { }
}

// cursor spotlight on cards and effect tiles
document.addEventListener('pointermove', e => {
  const c = e.target.closest && e.target.closest('.card, .fx');
  if (!c) return;
  const r = c.getBoundingClientRect();
  c.style.setProperty('--mx', (e.clientX - r.left) + 'px');
  c.style.setProperty('--my', (e.clientY - r.top) + 'px');
}, { passive: true });
$$('#nav button').forEach(b => b.addEventListener('click', () => showTab(b.dataset.tab)));
$$('[data-go]').forEach(b => b.addEventListener('click', () => showTab(b.dataset.go)));

// ------------------------------------------------------------------ colour picker
const PK = { h: 0, s: 1, v: 1, onChange: null, onDelete: null, anchor: null };
const pk = $('#picker'), pkSv = $('#pk-sv'), pkHue = $('#pk-hue'), pkHex = $('#pk-hex');
function pkColor() { const [r, g, b] = hsv2rgb(PK.h, PK.s, PK.v); return rgb2hex(r, g, b); }
function pkDraw() {
  const c = pkSv.getContext('2d'), w = pkSv.width, h = pkSv.height;
  c.fillStyle = css(hsv2rgb(PK.h, 1, 1)); c.fillRect(0, 0, w, h);
  let g = c.createLinearGradient(0, 0, w, 0); g.addColorStop(0, '#fff'); g.addColorStop(1, 'rgba(255,255,255,0)');
  c.fillStyle = g; c.fillRect(0, 0, w, h);
  g = c.createLinearGradient(0, 0, 0, h); g.addColorStop(0, 'rgba(0,0,0,0)'); g.addColorStop(1, '#000');
  c.fillStyle = g; c.fillRect(0, 0, w, h);
  const x = PK.s * w, y = (1 - PK.v) * h;
  c.beginPath(); c.arc(x, y, 7, 0, 7); c.lineWidth = 2.5; c.strokeStyle = '#fff'; c.stroke();
  c.beginPath(); c.arc(x, y, 8.5, 0, 7); c.lineWidth = 1; c.strokeStyle = 'rgba(0,0,0,.4)'; c.stroke();
  const hex = pkColor();
  $('#pk-cur').style.background = hex;
  if (document.activeElement !== pkHex) pkHex.value = hex;
}
function pkEmit() { pkDraw(); PK.onChange && PK.onChange(pkColor()); }
function openPicker(anchor, hex, onChange, onDelete) {
  [PK.h, PK.s, PK.v] = rgb2hsv(...hex2rgb(hex));
  PK.onChange = onChange; PK.onDelete = onDelete; PK.anchor = anchor;
  pkHue.value = PK.h;
  $('#pk-del').classList.toggle('hidden', !onDelete);
  $('#pk-presets').innerHTML = PRESETS.map(c => `<button style="background:${c}" data-c="${c}"></button>`).join('');
  pk.classList.remove('hidden');
  const r = anchor.getBoundingClientRect(), pw = pk.offsetWidth, ph = pk.offsetHeight;
  let x = Math.min(r.left, innerWidth - pw - 12), y = r.bottom + 10;
  if (y + ph > innerHeight - 12) y = Math.max(12, r.top - ph - 10);
  pk.style.left = x + 'px'; pk.style.top = y + 'px';
  pkDraw();
}
function closePicker() { pk.classList.add('hidden'); PK.onChange = PK.onDelete = null; $$('.sw.sel').forEach(s => s.classList.remove('sel')); }
function svAt(e) {
  const r = pkSv.getBoundingClientRect();
  PK.s = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
  PK.v = 1 - Math.max(0, Math.min(1, (e.clientY - r.top) / r.height));
  pkEmit();
}
pkSv.addEventListener('pointerdown', e => { pkSv.setPointerCapture(e.pointerId); svAt(e); });
pkSv.addEventListener('pointermove', e => { if (e.buttons) svAt(e); });
pkHue.addEventListener('input', () => { PK.h = +pkHue.value; pkEmit(); });
pkHex.addEventListener('input', () => {
  const m = pkHex.value.trim().replace(/^#?/, '#');
  if (/^#[0-9a-fA-F]{6}$/.test(m)) { [PK.h, PK.s, PK.v] = rgb2hsv(...hex2rgb(m)); pkHue.value = PK.h; pkEmit(); }
});
$('#pk-presets').addEventListener('click', e => {
  const c = e.target.dataset.c; if (!c) return;
  [PK.h, PK.s, PK.v] = rgb2hsv(...hex2rgb(c)); pkHue.value = PK.h; pkEmit();
});
$('#pk-ok').addEventListener('click', closePicker);
$('#pk-del').addEventListener('click', () => { const d = PK.onDelete; closePicker(); d && d(); });
document.addEventListener('pointerdown', e => {
  if (pk.classList.contains('hidden') || pk.contains(e.target) || (PK.anchor && PK.anchor.contains(e.target))) return;
  closePicker();
}, true);

// ------------------------------------------------------------------ palette editor
// Swatches for `section`.palette; single = only the first colour (a zone's own colour).
function renderPalette(box, section, single, fallback) {
  const pal = parsePal(cv(section, 'palette'));
  const list = pal.length ? pal : (fallback || DEFAULT_PAL).slice();
  const shown = single ? list.slice(0, 1) : list;
  box.innerHTML = '';
  shown.forEach((c, i) => {
    const b = document.createElement('button');
    b.className = 'sw'; b.style.setProperty('--c', c); b.title = c;
    b.addEventListener('click', () => {
      $$('.sw.sel').forEach(s => s.classList.remove('sel'));
      b.classList.add('sel');
      openPicker(b, list[i], hex => {
        list[i] = hex; b.style.setProperty('--c', hex); b.title = hex;
        setCfgSoon(section, 'palette', palStr(list));
        onPaletteChanged(section);
      }, !single && list.length > 1 ? () => {
        list.splice(i, 1);
        setCfg(section, 'palette', palStr(list));
        renderPalette(box, section, single, fallback);
        onPaletteChanged(section);
      } : null);
    });
    box.appendChild(b);
  });
  if (!single && list.length < 8) {
    const a = document.createElement('button');
    a.className = 'sw add'; a.textContent = '+'; a.title = t('add.colour');
    a.addEventListener('click', () => {
      list.push(list[list.length - 1] || '#FFFFFF');
      setCfg(section, 'palette', palStr(list));
      renderPalette(box, section, single, fallback);
      box.querySelectorAll('.sw:not(.add)')[list.length - 1].click();
    });
    box.appendChild(a);
  }
}
function onPaletteChanged(section) {
  if (section === S.effect) updateBrand();
}

// ------------------------------------------------------------------ zone editor
const MODES = ['effect', 'palette', 'static', 'white'];
function zoneMode(section) {
  const m = cv(section, 'mode', 'effect');
  if (m === 'white') return 'white';
  if (!parsePal(cv(section, 'palette')).length) return 'effect';
  return m === 'static' || m === 'palette' ? m : 'effect';
}
// What a device's zone can do: 'white' (white light only: Elgato Key Light, Ring Light; its range in K from the
// driver, [dev.N] caps / kmin / kmax), 'onoff' (only switched on and off here: a Govee sync box in its own mode),
// or '' (everything). Only what applies is shown, and a group does not push the rest onto it.
function zoneCaps(zone) {
  const m = /^zone\.dev(\d+)$/.exec(zone || ''); if (!m) return { kind: '' };
  const sec = 'dev.' + m[1], d = (S.ext.devs || []).find(x => x.id === +m[1]);
  if (d && d.kind === 'goveecloud' && gcSync(d, sec)) return { kind: 'onoff' };
  if (cv(sec, 'caps', '') === 'white') return { kind: 'white', kmin: +cv(sec, 'kmin', 2700), kmax: +cv(sec, 'kmax', 6500) };
  return { kind: '' };
}
const zoneTakes = (zone, k) => { const c = zoneCaps(zone).kind; return !c || (c === 'white' && (k === 'brightness' || k === 'kelvin')); };

function renderZone(el) {
  const section = el.dataset.zone, caps = zoneCaps(section);
  if (caps.kind === 'onoff') { el.innerHTML = `<p class="note">${t('zone.onoff')}</p>`; return; }
  if (caps.kind === 'white') { renderWhiteZone(el, section, caps); return; }
  const mode = zoneMode(section);
  const k = +cv(section, 'kelvin', 4000), br = +cv(section, 'brightness', 100);
  const own = cv(section, 'effect', '');
  const opts = [`<option value="">${t('zone.global')} — ${fxName(S.effect)}</option>`]
    .concat(S.effects.map(e => `<option value="${e.id}" ${e.id === own ? 'selected' : ''}>${e.id === 'off' ? t('zone.offfx') : fxName(e.id)}</option>`));
  el.innerHTML = `
    <div class="field">
      <div class="lbl"><span>${t('zone.effect')}</span>${own ? `<b>${t('zone.own')}</b>` : ''}</div>
      <select class="select zfx ${own ? 'own' : ''}">${opts.join('')}</select>
    </div>
    <div class="field"><div class="lbl"><span>${t('zone.mode')}</span></div></div>
    <div class="seg">${MODES.map(m => `<button data-m="${m}" class="${m === mode ? 'on' : ''}">${t('mode.' + m)}</button>`).join('')}</div>
    <div class="zbody"></div>
    <div class="field">
      <div class="lbl"><span>${t('zone.bright')}</span><b>${br}%</b></div>
      <input type="range" class="range zb" min="1" max="100" value="${br}">
    </div>`;
  const body = el.querySelector('.zbody');
  el.querySelector('.zfx').addEventListener('change', e => { setCfg(section, 'effect', e.target.value); renderZone(el); renderOwnList(); });
  if ((own || S.effect) === 'screen' && mode === 'effect') {
    const sa = cv(section, 'screen', 'auto'), f = document.createElement('div');
    f.className = 'field';
    f.innerHTML = `<div class="lbl"><span>${t('zone.screen')}</span></div><select class="select zscr">${['auto', 'whole', 'border', 'left', 'right', 'top', 'bottom'].map(v =>
      `<option value="${v}"${sa === v ? ' selected' : ''}>${t('scr.' + v)}</option>`).join('')}</select>`;
    body.after(f);
    f.querySelector('.zscr').addEventListener('change', e => setCfg(section, 'screen', e.target.value));
  }
  const fxT = fxName(own || S.effect);
  if (mode === 'effect') body.innerHTML = `<p class="note">${t('zone.note', fxT)}</p>`;
  else if (mode === 'white') {
    body.innerHTML = `<div class="field"><div class="lbl"><span>${t('kelvin')}</span><b>${k} K</b></div>
      <input type="range" class="range kelvin" min="2700" max="6500" step="100" value="${k}"></div>
      ${section.startsWith('zone.light') ? `<p class="note">${t('bulb.white.note')}</p>` : ''}`;
    const r = body.querySelector('.kelvin'); fill(r);
    r.addEventListener('input', () => { r.parentElement.querySelector('b').textContent = r.value + ' K'; setCfgSoon(section, 'kelvin', r.value); });
  } else {
    body.innerHTML = `<div class="swatches"></div>`;
    renderPalette(body.firstChild, section, mode === 'static', effPal(own || S.effect));
  }
  const zb = el.querySelector('.zb'); fill(zb);
  zb.addEventListener('input', () => { zb.parentElement.querySelector('b').textContent = zb.value + '%'; setCfgSoon(section, 'brightness', zb.value); });
  el.querySelectorAll('.seg button').forEach(b => b.addEventListener('click', () => {
    const m = b.dataset.m;
    if ((m === 'palette' || m === 'static') && !parsePal(cv(section, 'palette')).length) setCfg(section, 'palette', palStr(effPal(own || S.effect)));
    setCfg(section, 'mode', m);
    renderZone(el);
  }));
}
// a white-only light: its colour temperature (in its own range) and brightness, nothing else
function renderWhiteZone(el, section, caps) {
  if (cv(section, 'mode', '') !== 'white') setCfg(section, 'mode', 'white');
  const k = Math.min(caps.kmax, Math.max(caps.kmin, +cv(section, 'kelvin', 4500))), br = +cv(section, 'brightness', 100);
  el.innerHTML = `
    <div class="field"><div class="lbl"><span>${t('kelvin')}</span><b>${k} K</b></div>
      <input type="range" class="range kelvin" min="${caps.kmin}" max="${caps.kmax}" step="100" value="${k}"></div>
    <div class="field"><div class="lbl"><span>${t('zone.bright')}</span><b>${br}%</b></div>
      <input type="range" class="range zb" min="3" max="100" value="${Math.max(3, br)}"></div>
    <p class="note">${t('zone.white')}</p>`;
  const r = el.querySelector('.kelvin'), zb = el.querySelector('.zb'); fill(r); fill(zb);
  r.addEventListener('input', () => { r.parentElement.querySelector('b').textContent = r.value + ' K'; setCfgSoon(section, 'kelvin', r.value); });
  zb.addEventListener('input', () => { zb.parentElement.querySelector('b').textContent = zb.value + '%'; setCfgSoon(section, 'brightness', zb.value); });
}
const renderZones = () => $$('.zone').forEach(renderZone);

// ------------------------------------------------------------------ effects tab
// the grid shows one group at a time: the built-in effects, or the user's own presets (and the + tile)
let FX_TAB = null;
try { FX_TAB = localStorage.getItem('fxTab'); } catch (e) { }
function fxTab(v) {
  FX_TAB = v;
  try { localStorage.setItem('fxTab', v); } catch (e) { }
  buildEffects();
}
$('#fx-tabs').addEventListener('click', e => { const b = e.target.closest('button'); if (b && b.dataset.v !== FX_TAB) fxTab(b.dataset.v); });
function buildEffects() {
  const grid = $('#fx-grid');
  grid.innerHTML = '';
  if (FX_TAB !== 'fx' && FX_TAB !== 'presets' && FX_TAB !== 'community') FX_TAB = activePreset() ? 'presets' : 'fx';
  const tabs = $$('#fx-tabs button');
  tabs.forEach(b => b.classList.toggle('on', b.dataset.v === FX_TAB));
  tabs[0].querySelector('i').textContent = S.effects.length;
  tabs[1].querySelector('i').textContent = presets().length || '';
  if (FX_TAB === 'community') { buildCommunity(grid); return; }
  if (FX_TAB === 'fx') S.effects.forEach((e, n) => {
    const b = document.createElement('button');
    b.className = 'fx'; b.dataset.id = e.id;
    b.innerHTML = `<div class="top"><b>${fxName(e.id)}</b><span class="num-i">${String(n + 1).padStart(2, '0')}</span></div><small>${t('short.' + e.id)}</small><canvas></canvas>`;
    b.addEventListener('click', () => { S.effect = e.id; setCfgLocal('general', 'preset', ''); send({ cmd: 'effect', id: e.id }); renderEffectSide(); markEffect(); });
    grid.appendChild(b);
  });
  // own presets, then the + tile that makes a new one
  if (FX_TAB === 'presets') presets().forEach(P => {
    const b = document.createElement('button');
    b.className = 'fx preset'; b.dataset.preset = P.id;
    const sub = [fxName(P.effect)].concat(P.bri ? [P.bri + '%'] : [], P.zones ? [t('preset.sub.zones')] : []).join(' · ');
    b.innerHTML = `<div class="top"><b>${esc(P.name)}</b><span class="num-i">P${P.id}</span><i class="pmenu" title="${t('preset.edit')}">···</i></div><small>${esc(sub)}</small><canvas></canvas>`;
    b.addEventListener('click', e => {
      if (e.target.closest('.pmenu')) { presetDialog(P.id); return; }
      S.effect = P.effect; setCfgLocal('general', 'preset', String(P.id));
      if (P.palette) setCfgLocal(P.effect, 'palette', P.palette);
      if (P.speed) setCfgLocal(P.effect, 'speed', P.speed);
      send({ cmd: 'preset', id: String(P.id) });
      renderEffectSide(); markEffect();
    });
    b.addEventListener('contextmenu', e => { e.preventDefault(); presetDialog(P.id); });
    grid.appendChild(b);
  });
  if (FX_TAB === 'presets' && presets().length < 32) {
    const add = document.createElement('button');
    add.className = 'fx add'; add.title = t('preset.new');
    add.innerHTML = `<span class="plus">+</span><b>${t('preset.add')}</b><small>${t('preset.add.sub')}</small>`;
    add.addEventListener('click', () => presetDialog(0));
    grid.appendChild(add);
    const imp = document.createElement('button');
    imp.className = 'fx add'; imp.title = t('preset.import');
    imp.innerHTML = `<span class="plus">⇩</span><b>${t('preset.import')}</b><small>${t('preset.import.sub')}</small>`;
    imp.addEventListener('click', () => shareDialog(null));
    grid.appendChild(imp);
  }
  markEffect();
}
// [preset.N] sections of the settings (the core saves and applies them, see main.c)
function presets() {
  return Object.keys(S.cfg).map(k => /^preset\.(\d+)$/.exec(k)).filter(m => m && S.cfg[m[0]].effect).map(m => {
    const c = S.cfg[m[0]];
    return { id: +m[1], name: c.name || 'Preset ' + m[1], effect: c.effect, palette: c.palette || '', speed: c.speed || '', bri: c.brightness || '', zones: c.zones === '1' };
  }).sort((a, b) => a.id - b.id);
}
// the preset that is showing: [general] preset, while its effect is the current one
function activePreset() {
  const id = +cv('general', 'preset', 0), P = id && presets().find(p => p.id === id);
  return P && P.effect === S.effect ? P : null;
}
function markEffect() {
  const P = activePreset();
  $$('.fx').forEach(b => b.classList.toggle('on', b.dataset.preset ? !!P && +b.dataset.preset === P.id : !P && b.dataset.id === S.effect));
  // a dot on the other group's switch when what is showing lives there
  $$('#fx-tabs button').forEach(b => b.classList.toggle('lit', b.dataset.v === 'presets' ? !!P : !P && S.effect !== 'off'));
  const off = S.effect === 'off';
  $('#power').classList.toggle('off', off);
  $('#power span').textContent = off ? t('power.on') : t('power.off');
  updateBrand();
}
// ---- profiles: a whole setup per scenario (gaming, work, night...): the effect and colours, which devices are on,
// the preview's widgets. [profile.N] name / hotkey; the core keeps each setup and switches them (see main.c), so
// the page only lists them. The switcher sits above the preview: a click on a profile switches to it, the pencil
// renames it (and sets its hotkey or deletes it), "Save profile" makes a new one from what is set up now.
const PROFILE_MAX = 16;
const PROF = { edit: 0, sure: false };
const profSec = id => 'profile.' + id;
function profiles() {
  return Object.keys(S.cfg).map(k => /^profile\.(\d+)$/.exec(k)).filter(m => m && S.cfg[m[0]].name)
    .map(m => ({ id: +m[1], name: S.cfg[m[0]].name, hotkey: S.cfg[m[0]].hotkey || '' })).sort((a, b) => a.id - b.id);
}
const profCur = () => profiles().find(p => p.id === +cv('general', 'profile', 0)) || null;
const PENCIL = '<svg viewBox="0 0 24 24"><path d="M4 20h4L19 9l-4-4L4 16z"/><path d="M13 7l4 4"/></svg>';
function renderProfiles() {
  const P = profiles(), cur = profCur();
  $('#prof-cur').textContent = cur ? cur.name : t('prof.none');
  $('#prof-btn').classList.toggle('none', !cur);
  if (PROF.edit && !P.some(p => p.id === PROF.edit)) PROF.edit = 0;
  $('#prof-list').innerHTML = P.map(p => p.id === PROF.edit ? `
    <div class="prof-edit" data-id="${p.id}">
      <input type="text" class="prof-name" maxlength="40" spellcheck="false" value="${esc(p.name)}">
      <div class="hk"><span>${t('prof.hotkey')}</span><button class="kbd" type="button"></button></div>
      <div class="btns">
        <button class="btn danger small prof-del" type="button"><span>${t(PROF.sure ? 'prof.delete.sure' : 'prof.delete')}</span></button>
        <span class="grow"></span>
        <button class="btn primary small prof-ok" type="button"><span>${t('prof.done')}</span></button>
      </div>
    </div>` : `
    <div class="prof-row${cur && cur.id === p.id ? ' on' : ''}" data-id="${p.id}">
      <button class="prof-item" type="button"><i></i><b>${esc(p.name)}</b>${p.hotkey ? `<small>${esc(p.hotkey.replace(/\+/g, ' + '))}</small>` : ''}</button>
      <button class="prof-ren" type="button" title="${t('prof.rename')}">${PENCIL}</button>
    </div>`).join('') || `<p class="hint">${t('prof.empty')}</p>`;
  const ed = $('#prof-list .prof-edit');
  if (ed) hotkeyButton($('.kbd', ed), profSec(PROF.edit), 'hotkey');
  const full = P.length >= PROFILE_MAX;
  $('#prof-save').classList.toggle('hidden', full || !$('#prof-new').classList.contains('hidden'));
  $('#prof-new-name').placeholder = t('prof.name.ph');
}
function profMenu(open) {
  const m = $('#prof-menu');
  if (open === undefined) open = m.classList.contains('hidden');
  m.classList.toggle('hidden', !open);
  $('#prof-btn').setAttribute('aria-expanded', open ? 'true' : 'false');
  if (!open) { profRenameDone(); $('#prof-new').classList.add('hidden'); PROF.edit = 0; PROF.sure = false; }
  renderProfiles();
}
// the name typed in the pencil's box goes in when it is left
function profRenameDone() {
  const ed = $('#prof-list .prof-edit'); if (!ed) return;
  const id = +ed.dataset.id, v = $('.prof-name', ed).value.replace(/[;#\[\]=]/g, ' ').trim();
  if (v && v !== cv(profSec(id), 'name', '')) setCfg(profSec(id), 'name', v);
}
$('#prof-btn').addEventListener('click', e => { e.stopPropagation(); profMenu(); });
$('#prof-list').addEventListener('click', e => {
  const row = e.target.closest('[data-id]'); if (!row) return;
  const id = +row.dataset.id;
  if (e.target.closest('.prof-ren')) { profRenameDone(); PROF.edit = id; PROF.sure = false; renderProfiles(); const i = $('#prof-list .prof-name'); i.focus(); i.select(); return; }
  if (e.target.closest('.prof-item')) { profMenu(false); if (id !== +cv('general', 'profile', 0)) { setCfgLocal('general', 'profile', String(id)); send({ cmd: 'profile', id: String(id) }); renderProfiles(); } return; }
  if (e.target.closest('.prof-ok')) { profRenameDone(); PROF.edit = 0; renderProfiles(); return; }
  if (e.target.closest('.prof-del')) {
    if (!PROF.sure) { PROF.sure = true; profRenameDone(); renderProfiles(); return; }
    send({ cmd: 'profile_delete', id: String(id) }); PROF.edit = 0; PROF.sure = false;
  }
});
$('#prof-list').addEventListener('keydown', e => {
  if (!e.target.classList.contains('prof-name')) return;
  if (e.key === 'Enter') { e.preventDefault(); profRenameDone(); PROF.edit = 0; renderProfiles(); }
  if (e.key === 'Escape') { e.stopPropagation(); PROF.edit = 0; renderProfiles(); }
});
$('#prof-save').addEventListener('click', () => {
  profRenameDone(); PROF.edit = 0;
  $('#prof-new').classList.remove('hidden'); $('#prof-new-name').value = '';
  renderProfiles(); $('#prof-new-name').focus();
});
$('#prof-new').addEventListener('submit', e => {
  e.preventDefault();
  const name = $('#prof-new-name').value.replace(/[;#\[\]=]/g, ' ').trim() || t('prof.n', profiles().length + 1);
  send({ cmd: 'profile_save', id: '0', name });
  $('#prof-new').classList.add('hidden'); renderProfiles();
});
$('#prof-new-name').addEventListener('keydown', e => { if (e.key === 'Escape') { e.stopPropagation(); $('#prof-new').classList.add('hidden'); renderProfiles(); } });
document.addEventListener('pointerdown', e => { if (!e.target.closest('#prof') && !$('#prof-menu').classList.contains('hidden')) profMenu(false); });
document.addEventListener('keydown', e => { if (e.key === 'Escape' && !$('#prof-menu').classList.contains('hidden') && !$('.kbd.rec')) profMenu(false); });

// ---- preset window: "+" opens it with the look that shows now as the starting point, and every part (effect,
// colours, speed, brightness, the devices' own colours) can be changed before it is added. A preset's "···" (or a
// right click) opens the same window with that preset's values. Saving shows the preset.
const PDLG = { id: 0, effect: 'flow', pal: [], palTouched: false, speed: 5, bri: 100, zonesHad: false, retake: false, raf: 0 };
const pdlgEffects = () => S.effects.filter(e => e.id !== 'off');
const pdlgSpeed = id => !['static', 'off', 'temperature'].includes(id);
// devices with a look of their own right now (own effect, own colours, white light)
const zonesCustom = () => $$('.zone').some(z => cv(z.dataset.zone, 'mode', 'effect') !== 'effect' || cv(z.dataset.zone, 'effect', ''));
function pdlgFromNow() {
  const P = activePreset(), fx = S.effect !== 'off' ? S.effect : (P ? P.effect : (pdlgEffects()[0] || {}).id || 'flow');
  PDLG.effect = fx; PDLG.pal = effPal(fx).slice(); PDLG.palTouched = false;
  PDLG.speed = +cv(fx, 'speed', cv('general', 'speed', 5)); PDLG.bri = S.brightness || 100;
}
function presetDialog(id, name) {
  const P = id ? presets().find(p => p.id === id) : null;
  PDLG.id = P ? P.id : 0; PDLG.zonesHad = !!(P && P.zones); PDLG.retake = false;
  if (P) {
    PDLG.effect = S.effects.some(e => e.id === P.effect) ? P.effect : 'flow';
    PDLG.pal = parsePal(P.palette).length ? parsePal(P.palette) : effPal(PDLG.effect).slice(); PDLG.palTouched = true;
    PDLG.speed = +P.speed || 5; PDLG.bri = +P.bri || S.brightness || 100;
  } else pdlgFromNow();
  $('#pdlg-title').textContent = t(P ? 'preset.edit' : 'preset.new');
  $('#pdlg-name').value = P ? P.name : name || '';
  $('#pdlg-name').placeholder = t('preset.name.ph');
  $('#pdlg-bri').checked = P ? !!P.bri : false;
  $('#pdlg-zones').checked = P ? P.zones : zonesCustom();
  $('#pdlg-save span').textContent = t(P ? 'preset.save' : 'preset.addbtn');
  const del = $('#pdlg-del'); del.classList.toggle('hidden', !P); del.classList.remove('confirm'); del.querySelector('span').textContent = t('preset.delete');
  $('#pdlg-share').classList.toggle('hidden', !P);
  $('#pdlg').classList.remove('hidden');
  pdlgRender();
  const cvs = $('#pdlg-prev'); cvs.width = cvs.clientWidth * devicePixelRatio; cvs.height = cvs.clientHeight * devicePixelRatio;
  if (!PDLG.raf) PDLG.raf = requestAnimationFrame(pdlgPreview);
  setTimeout(() => { $('#pdlg-name').focus(); $('#pdlg-name').select(); }, 30);
}
function pdlgRender() {
  $('#pdlg-fx').innerHTML = pdlgEffects().map(e => `<button type="button" data-fx="${e.id}" class="${e.id === PDLG.effect ? 'on' : ''}">${fxName(e.id)}</button>`).join('');
  pdlgPalette();
  $('#pdlg-speed-field').classList.toggle('hidden', !pdlgSpeed(PDLG.effect));
  setRange($('#pdlg-speed'), PDLG.speed); $('#pdlg-speed-val').textContent = PDLG.speed;
  const bri = $('#pdlg-bri').checked, br = $('#pdlg-bri-v');
  setRange(br, PDLG.bri); br.disabled = !bri; $('#pdlg-bri-val').textContent = bri ? PDLG.bri + '%' : '';
  const z = $('#pdlg-zones').checked, keep = z && PDLG.zonesHad && !PDLG.retake;
  $('#pdlg-zones-note').textContent = z ? t(keep ? 'preset.zones.keep' : 'preset.zones.take') : t('preset.zones.none');
}
// the colours as swatches: click one to change it (or remove it in the picker), "+" adds one; a single colour for static
function pdlgPalette() {
  const box = $('#pdlg-pal'), single = PDLG.effect === 'static', list = PDLG.pal;
  box.innerHTML = '';
  (single ? list.slice(0, 1) : list).forEach((c, i) => {
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'sw'; b.style.setProperty('--c', c); b.title = c;
    b.addEventListener('click', () => {
      $$('.sw.sel').forEach(s => s.classList.remove('sel')); b.classList.add('sel');
      openPicker(b, list[i], hex => { list[i] = hex; PDLG.palTouched = true; b.style.setProperty('--c', hex); b.title = hex; },
        !single && list.length > 1 ? () => { list.splice(i, 1); PDLG.palTouched = true; pdlgPalette(); } : null);
    });
    box.appendChild(b);
  });
  if (!single && list.length < 8) {
    const a = document.createElement('button');
    a.type = 'button'; a.className = 'sw add'; a.textContent = '+'; a.title = t('add.colour');
    a.addEventListener('click', () => { list.push(list[list.length - 1] || '#FFFFFF'); PDLG.palTouched = true; pdlgPalette(); box.querySelectorAll('.sw:not(.add)')[list.length - 1].click(); });
    box.appendChild(a);
  }
}
function pdlgPreview(now) {
  PDLG.raf = 0;
  if ($('#pdlg').classList.contains('hidden')) return;
  const cvs = $('#pdlg-prev'), pal = (PDLG.effect === 'static' ? PDLG.pal.slice(0, 1) : PDLG.pal).map(hex2rgb);
  if (cvs.width && pal.length) drawStrip(cvs, preview(PDLG.effect, pal, now / 1000 * PDLG.speed / 5, 40));
  PDLG.raf = requestAnimationFrame(pdlgPreview);
}
$('#pdlg-fx').addEventListener('click', e => {
  const b = e.target.closest('[data-fx]'); if (!b) return;
  PDLG.effect = b.dataset.fx;
  // colours that were not changed by hand follow the effect (its own palette); the speed too
  if (!PDLG.palTouched) PDLG.pal = effPal(PDLG.effect).slice();
  if (!PDLG.id) PDLG.speed = +cv(PDLG.effect, 'speed', cv('general', 'speed', 5));
  pdlgRender();
});
$('#pdlg-speed').addEventListener('input', e => { PDLG.speed = +e.target.value; fill(e.target); $('#pdlg-speed-val').textContent = PDLG.speed; });
$('#pdlg-bri-v').addEventListener('input', e => { PDLG.bri = +e.target.value; fill(e.target); $('#pdlg-bri-val').textContent = PDLG.bri + '%'; });
$('#pdlg-bri').addEventListener('change', pdlgRender);
$('#pdlg-zones').addEventListener('change', pdlgRender);
$('#pdlg-retake').addEventListener('click', () => {   // start over from what shows now (the devices' colours too)
  pdlgFromNow(); PDLG.retake = true;
  if (zonesCustom()) $('#pdlg-zones').checked = true;
  pdlgRender();
});
const closePresetDialog = () => { $('#pdlg').classList.add('hidden'); closePicker(); };
const pdlgName = () => $('#pdlg-name').value.replace(/[;#\[\]=]/g, ' ').trim();
$('#pdlg-form').addEventListener('submit', e => {
  e.preventDefault();
  const z = $('#pdlg-zones').checked, pal = PDLG.effect === 'static' ? PDLG.pal.slice(0, 1) : PDLG.pal;
  send({ cmd: 'preset_save', id: String(PDLG.id), name: pdlgName(), effect: PDLG.effect, palette: palStr(pal), speed: String(PDLG.speed),
    brightness: $('#pdlg-bri').checked ? String(PDLG.bri) : '', zones: z ? (PDLG.id && PDLG.zonesHad && !PDLG.retake ? 'keep' : '1') : '0', apply: '1' });
  closePresetDialog();
  if (FX_TAB !== 'presets') fxTab('presets');
});
$('#pdlg-del').addEventListener('click', () => {
  const b = $('#pdlg-del');
  if (!b.classList.contains('confirm')) { b.classList.add('confirm'); b.querySelector('span').textContent = t('preset.delete.sure'); return; }
  send({ cmd: 'preset_delete', id: String(PDLG.id) });
  closePresetDialog();
});
$('#pdlg-cancel').addEventListener('click', closePresetDialog);
$('#pdlg-share').addEventListener('click', () => { const P = presets().find(p => p.id === PDLG.id); closePresetDialog(); if (P) shareDialog(P); });

// ---- sharing a preset: a code (haku:<base64url of its JSON>) with the look only (effect, colours, speed,
// brightness), nothing of the devices; pasted in Import it becomes a preset of its own
const b64u = s => btoa(String.fromCharCode(...new TextEncoder().encode(s))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
const unb64u = s => new TextDecoder().decode(Uint8Array.from(atob(s.replace(/-/g, '+').replace(/_/g, '/') + '==='.slice((s.length + 3) % 4)), c => c.charCodeAt(0)));
function presetCode(P) {
  const o = { v: 1, n: P.name, e: P.effect, p: parsePal(P.palette), s: +P.speed || undefined, b: +P.bri || undefined };
  return 'haku:' + b64u(JSON.stringify(o));
}
function presetFromCode(code) {
  const m = /haku:([A-Za-z0-9_-]+)/.exec(code.replace(/\s+/g, ''));
  if (!m) return null;
  try {
    const o = JSON.parse(unb64u(m[1]));
    if (!o || o.v !== 1 || !S.effects.some(e => e.id === o.e)) return null;
    const pal = (Array.isArray(o.p) ? o.p : []).filter(c => /^#[0-9a-fA-F]{6}$/.test(c)).slice(0, 8);
    const speed = Math.max(1, Math.min(10, Math.round(+o.s || 5))), bri = o.b ? Math.max(5, Math.min(100, Math.round(+o.b))) : 0;
    return { name: String(o.n || '').replace(/[;#\[\]=]/g, ' ').trim().slice(0, 40) || fxName(o.e), effect: o.e, palette: pal.join(', '), speed, bri };
  } catch (e) { return null; }
}
let SHARE = null;   // the preset being shared, or null: importing
function shareDialog(P) {
  SHARE = P;
  $('#share-title').textContent = t(P ? 'share.title' : 'import.title');
  $('#share-note').textContent = t(P ? 'share.note' : 'import.note');
  const ta = $('#share-code');
  ta.value = P ? presetCode(P) : ''; ta.readOnly = !!P; ta.placeholder = P ? '' : 'haku:…';
  $('#share-ok span').textContent = t(P ? 'share.copy' : 'import.go');
  $('#share-msg').textContent = '';
  $('#pub-row').classList.toggle('hidden', !P);
  $('#pub-author').value = cv('community', 'author', ''); $('#pub-author').placeholder = t('pub.author');
  $('#share-dlg').classList.remove('hidden');
  setTimeout(() => { ta.focus(); if (P) ta.select(); }, 30);
}
const closeShare = () => $('#share-dlg').classList.add('hidden');
$('#share-close').addEventListener('click', closeShare);
$('#share-dlg').addEventListener('pointerdown', e => { if (e.target.id === 'share-dlg') closeShare(); });
document.addEventListener('keydown', e => { if (e.key === 'Escape' && !$('#share-dlg').classList.contains('hidden')) closeShare(); });
$('#share-form').addEventListener('submit', async e => {
  e.preventDefault();
  const ta = $('#share-code');
  if (SHARE) {   // copy: the clipboard, or the code stays selected for Ctrl+C
    ta.select();
    let ok = false;
    try { await navigator.clipboard.writeText(ta.value); ok = true; } catch (x) { try { ok = document.execCommand('copy'); } catch (y) { } }
    $('#share-msg').textContent = t(ok ? 'share.copied' : 'share.copyhand');
    return;
  }
  const p = presetFromCode(ta.value);
  if (!p) { $('#share-msg').textContent = t('import.bad'); ta.focus(); return; }
  if (presets().length >= 32) { $('#share-msg').textContent = t('import.full'); return; }
  send({ cmd: 'preset_save', id: '0', name: p.name, effect: p.effect, palette: p.palette, speed: String(p.speed),
    brightness: p.bri ? String(p.bri) : '', zones: '0', apply: '1' });
  closeShare();
  if (FX_TAB !== 'presets') fxTab('presets');
});

// ---- community presets (docs/community-api.md): presets people published, on hakune.blog. The list comes in
// pages of 24 (popular or new, a search, an effect); Add makes one a preset of one's own; ♥ likes it; Publish (in a
// preset's Share window) sends one for review. Only the look travels, plus a random install id (likes and
// downloads count once per install) and the author name typed in.
const COMM = { sort: 'top', q: '', effect: '', items: [], page: 0, more: false, busy: false, err: false, loaded: false, added: new Set() };
const commApi = () => cv('community', 'api', '') || 'https://hakune.blog/api/presets.php';
function commInstall() {
  let id = cv('community', 'install', '');
  if (!/^[0-9a-f]{32}$/.test(id)) {
    id = [...crypto.getRandomValues(new Uint8Array(16))].map(b => b.toString(16).padStart(2, '0')).join('');
    setCfg('community', 'install', id);
  }
  return id;
}
async function commPost(body) {
  const r = await fetch(commApi(), { method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: JSON.stringify({ ...body, install: commInstall() }) });
  return r.json();
}
async function commLoad(more) {
  if (COMM.busy) return;
  COMM.busy = true; COMM.err = false;
  if (!more) { COMM.page = 0; }
  const q = new URLSearchParams({ sort: COMM.sort, q: COMM.q, effect: COMM.effect, page: more ? COMM.page + 1 : 0, install: commInstall() });
  try {
    const r = await fetch(commApi() + '?' + q, { cache: 'no-store' }), j = await r.json();
    const items = (j.items || []).filter(x => x && x.id && S.effects.some(e => e.id === x.effect));
    COMM.items = more ? COMM.items.concat(items) : items;
    if (more) COMM.page++;
    COMM.more = !!j.more;
  } catch (e) { COMM.err = true; if (!more) COMM.items = []; }
  COMM.busy = false; COMM.loaded = true;
  if (FX_TAB === 'community') buildEffects();
}
let commSearchTimer = 0;
function buildCommunity(grid) {
  const bar = document.createElement('div');
  bar.className = 'comm-bar';
  bar.innerHTML = `<div class="seg"><button data-s="top" class="${COMM.sort === 'top' ? 'on' : ''}">${t('comm.top')}</button><button data-s="new" class="${COMM.sort === 'new' ? 'on' : ''}">${t('comm.new')}</button></div>
    <input type="text" class="comm-q" placeholder="${t('comm.search')}" value="${esc(COMM.q)}" spellcheck="false">
    <select class="select comm-fx"><option value="">${t('comm.all')}</option>${S.effects.filter(e => e.id !== 'off').map(e => `<option value="${e.id}" ${COMM.effect === e.id ? 'selected' : ''}>${esc(fxName(e.id))}</option>`).join('')}</select>`;
  bar.querySelectorAll('[data-s]').forEach(b => b.addEventListener('click', () => { COMM.sort = b.dataset.s; commLoad(false); buildEffects(); }));
  const q = bar.querySelector('.comm-q');
  q.addEventListener('input', () => { COMM.q = q.value.trim(); clearTimeout(commSearchTimer); commSearchTimer = setTimeout(() => commLoad(false), 350); });
  bar.querySelector('.comm-fx').addEventListener('change', e => { COMM.effect = e.target.value; commLoad(false); });
  grid.appendChild(bar);
  if (!COMM.loaded && !COMM.busy) commLoad(false);
  const note = text => { const n = document.createElement('div'); n.className = 'comm-note'; n.innerHTML = text; grid.appendChild(n); return n; };
  if (!COMM.loaded || (COMM.busy && !COMM.items.length)) { note(t('comm.loading')); return; }
  if (COMM.err && !COMM.items.length) {
    const n = note(`${t('comm.offline')} <button class="btn small ghost">${t('comm.retry')}</button>`);
    n.querySelector('button').addEventListener('click', () => { COMM.loaded = false; buildEffects(); });
    return;
  }
  if (!COMM.items.length) { note(t('comm.empty')); return; }
  COMM.items.forEach(C => {
    const b = document.createElement('div');
    b.className = 'fx comm'; b.dataset.comm = C.id;
    const added = COMM.added.has(C.id);
    b.innerHTML = `<div class="top"><b>${esc(C.name)}</b><button class="comm-like ${C.liked ? 'on' : ''}" title="${t('comm.like')}">♥ ${C.likes | 0}</button></div>
      <small>${esc(t('comm.by', C.author))} · ${esc(fxName(C.effect))}</small><canvas></canvas>
      <div class="comm-foot"><span>⇩ ${C.dl | 0}</span><button class="btn small ${added ? 'ghost' : 'primary'}" ${added ? 'disabled' : ''}>${t(added ? 'comm.added' : 'comm.add')}</button></div>`;
    b.querySelector('.comm-foot .btn').addEventListener('click', () => commAdd(C));
    b.querySelector('.comm-like').addEventListener('click', () => commLike(C));
    grid.appendChild(b);
  });
  if (COMM.more) {
    const m = document.createElement('button');
    m.className = 'fx add comm-more'; m.innerHTML = `<span class="plus">⋯</span><b>${t('comm.more')}</b>`;
    m.addEventListener('click', () => commLoad(true));
    grid.appendChild(m);
  }
}
function commAdd(C) {
  if (presets().length >= 32) { alertNote(t('import.full')); return; }
  send({ cmd: 'preset_save', id: '0', name: String(C.name).slice(0, 40), effect: C.effect, palette: (C.palette || []).join(', '), speed: String(C.speed || 5),
    brightness: C.bri ? String(C.bri) : '', zones: '0', apply: '1' });
  COMM.added.add(C.id);
  commPost({ action: 'download', id: C.id }).then(j => { if (j && j.dl != null) C.dl = j.dl; buildEffects(); }).catch(() => {});
  buildEffects();
}
function commLike(C) {
  C.liked = !C.liked; C.likes = (C.likes | 0) + (C.liked ? 1 : -1); buildEffects();
  commPost({ action: 'like', id: C.id }).then(j => { if (j && j.ok) { C.liked = !!j.liked; C.likes = j.likes | 0; buildEffects(); } }).catch(() => {});
}
const alertNote = s => { $('#share-msg').textContent = s; };
async function commPublish(P) {
  const author = $('#pub-author').value.replace(/[\u0000-\u001f]/g, '').trim();
  if (!author) { $('#pub-author').focus(); return; }
  setCfg('community', 'author', author);
  const btn = $('#pub-go'); btn.disabled = true;
  let msg;
  try {
    const j = await commPost({ action: 'publish', name: P.name, author, effect: P.effect, palette: parsePal(P.palette), speed: +P.speed || 5, bri: +P.bri || 0, version: S.update && S.update.version || '' });
    msg = j.ok ? t(j.status === 'approved' ? 'pub.live' : 'pub.sent') : t(j.error === 'rate' ? 'pub.rate' : j.error === 'invalid' ? 'pub.bad' : 'pub.err');
    if (j.ok && j.status === 'approved') COMM.loaded = false;
  } catch (e) { msg = t('pub.err'); }
  btn.disabled = false;
  $('#share-msg').textContent = msg;
}
$('#pub-go').addEventListener('click', () => { if (SHARE) commPublish(SHARE); });

// ---- the settings' backup (backup.c): saved / restored through the PC's own file dialogs
function updateBackup() {
  const B = S.backup || {}, note = $('#bk-note');
  note.textContent = B.res === 'saved' ? t('bk.saved', B.a, B.b) : B.res === 'restored' ? t('bk.restored', B.a, B.b) :
    B.res ? t('bk.' + B.res) : t('bk.note');
  note.classList.toggle('bad', !!B.res && B.res !== 'saved' && B.res !== 'restored');
}
$('#bk-save').addEventListener('click', () => send({ cmd: 'backup_save' }));
$('#bk-load').addEventListener('click', () => {
  const b = $('#bk-load');
  if (!b.classList.contains('confirm')) { b.classList.add('confirm'); b.querySelector('span').textContent = t('bk.load.sure'); setTimeout(() => { b.classList.remove('confirm'); b.querySelector('span').textContent = t('bk.load'); }, 5000); return; }
  b.classList.remove('confirm'); b.querySelector('span').textContent = t('bk.load');
  send({ cmd: 'backup_load' });
});
$('#pdlg').addEventListener('pointerdown', e => { if (e.target.id === 'pdlg') closePresetDialog(); });
document.addEventListener('keydown', e => { if (e.key === 'Escape' && !$('#pdlg').classList.contains('hidden') && pk.classList.contains('hidden')) closePresetDialog(); });

function renderEffectSide() {
  const id = S.effect, P = activePreset();
  $('#fx-name').textContent = P ? P.name : fxName(id);
  $('#fx-desc').textContent = (P ? t('preset.based', fxName(id)) + ' ' : '') + t('desc.' + id);
  $('#fx-pal-field').classList.toggle('hidden', id === 'off' || id === 'screen');
  renderPalette($('#fx-pal'), id, false, effPal(id));
  const hasSpeed = !['static', 'off', 'temperature'].includes(id);
  $('#fx-speed-field').classList.toggle('hidden', !hasSpeed);
  const sp = +cv(id, 'speed', cv('general', 'speed', 5));
  setRange($('#fx-speed'), sp); $('#fx-speed-val').textContent = sp;
  $('#fx-temp').classList.toggle('hidden', id !== 'temperature');
  $('#fx-screen').classList.toggle('hidden', id !== 'screen');
  $('#fx-wave').classList.toggle('hidden', id !== 'wave');
  if (id === 'wave') {
    const d = cv('wave', 'dir', 'right'), sz = cv('wave', 'size', '2');
    $$('#wave-dir button').forEach(b => b.classList.toggle('on', b.dataset.v === d));
    $$('#wave-size button').forEach(b => b.classList.toggle('on', b.dataset.v === sz));
  }
  if (id === 'screen') updateScreen();
  if (id === 'temperature') {
    const src = cv('temperature', 'source', 'gpu');
    $$('#temp-src button').forEach(b => b.classList.toggle('on', b.dataset.v === src));
    $('#temp-cold').value = cv('temperature', 'cold', 35);
    $('#temp-hot').value = cv('temperature', 'hot', 75);
  }
  renderZones();   // zones copy the effect palette when switched to their own
  const sync = cv('general', 'sync', '1') !== '0';
  $('#fx-sync').checked = sync;
  $('#fx-sync-note').textContent = sync ? t('sync.on') : t('sync.off');
  renderOwnList();
}
function zoneName(z) {
  if (z === 'zone.ram') return t('pc.memory');
  if (z === 'zone.gpu') return stripName();
  const nm = /^zone\.nanoleaf(\d*)$/.exec(z); if (nm) { const c = nanoCtls().find(x => x.slot === (+nm[1] || 1)); return nanoCtls().length > 1 ? nanoName(c) : 'Nanoleaf'; }
  const dm = /^zone\.dev(\d+)$/.exec(z); if (dm) { const d = S.ext.devs.find(x => x.id === +dm[1]); return d ? d.name : z; }
  const m = /^zone\.light(\d+)$/.exec(z); return m ? t('bulb', m[1]) : z;
}
function renderOwnList() {
  const zones = ['zone.ram', 'zone.gpu'].concat(nanoCtls().map(c => nanoZone(c.slot)), S.bulbs.map((_, i) => 'zone.light' + (i + 1)), S.ext.devs.map(d => 'zone.dev' + d.id));
  $('#fx-own').innerHTML = zones.filter(z => cv(z, 'effect', '')).map(z => {
    const id = cv(z, 'effect');
    return `<div class="own-item"><span>${zoneName(z)}</span><b>${id === 'off' ? t('zone.offfx') : fxName(id)}</b></div>`;
  }).join('');
}
$('#fx-sync').addEventListener('change', e => { setCfg('general', 'sync', e.target.checked ? 1 : 0); renderEffectSide(); });
$('#fx-speed').addEventListener('input', e => { $('#fx-speed-val').textContent = e.target.value; setCfgSoon(S.effect, 'speed', e.target.value); });
// Screen effect: which screen ([screen] monitor, 1..), and how the copy is doing
function updateScreen() {
  const C = S.screen || {}, n = C.monitors || 0, cur = +cv('screen', 'monitor', 1);
  $('#scr-mon-field').classList.toggle('hidden', n < 2);
  const html = Array.from({ length: n }, (_, i) => `<button data-v="${i + 1}" class="${cur === i + 1 ? 'on' : ''}">${t('scr.mon.n', i + 1)}</button>`).join('');
  const seg = $('#scr-mon');
  if (seg.dataset.html !== html) {
    seg.dataset.html = html; seg.innerHTML = html;
    seg.querySelectorAll('button').forEach(b => b.addEventListener('click', () => { setCfg('screen', 'monitor', b.dataset.v); updateScreen(); }));
  }
  $('#scr-note').textContent = C.err ? t('scr.err', C.err) : C.have ? t('scr.on', C.w, C.h) : t('scr.note');
}
$$('#temp-src button').forEach(b => b.addEventListener('click', () => { setCfg('temperature', 'source', b.dataset.v); renderEffectSide(); }));
$$('#wave-dir button').forEach(b => b.addEventListener('click', () => { setCfg('wave', 'dir', b.dataset.v); renderEffectSide(); }));
$$('#wave-size button').forEach(b => b.addEventListener('click', () => { setCfg('wave', 'size', b.dataset.v); renderEffectSide(); }));
['cold', 'hot'].forEach(k => $('#temp-' + k).addEventListener('change', e => { const v = +e.target.value; if (v > 0 && v < 120) setCfg('temperature', k, v); }));

function updateBrand() { }   // monochrome UI: the brand does not follow the palette

// ---- effect previews (JS versions of the core's effects, on a strip)
const fract = x => x - Math.floor(x);
const smooth01 = x => { x = Math.max(0, Math.min(1, x)); return x * x * (3 - 2 * x); };
function palc(pal, p) { p = fract(p) * pal.length; const i = Math.floor(p); return mix(pal[i % pal.length], pal[(i + 1) % pal.length], p - i); }
function grad(pal, p) {
  if (pal.length === 1) return pal[0];
  p = Math.max(0, Math.min(1, p)) * (pal.length - 1);
  const i = Math.floor(p); return i >= pal.length - 1 ? pal[pal.length - 1] : mix(pal[i], pal[i + 1], p - i);
}
// chance without state, as in effects.c: a hash of whole numbers and smooth noise over it
const hashf = n => { let x = Math.imul((n | 0) ^ 0x9E3779B9, 0x85EBCA6B); x ^= x >>> 13; x = Math.imul(x, 0xC2B2AE35); x ^= x >>> 16; return (x >>> 8) / 16777216; };
const vnoise = (x, seed) => { const i = Math.floor(x), f = x - i; const a = hashf(i * 7919 + seed), b = hashf((i + 1) * 7919 + seed); return a + (b - a) * smooth01(f); };
const hsvRgb = (h, s, v) => { h = fract(h) * 6; const i = Math.floor(h), f = h - i, p = v * (1 - s), q = v * (1 - s * f), u = v * (1 - s * (1 - f));
  return [[v, u, p], [q, v, p], [p, v, u], [p, q, v], [u, p, v], [v, p, q]][i % 6].map(k => k * 255); };
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const causticN = (x, y, t) => { const n = 0.5 + 0.5 * Math.sin(x * 13 + t * 1.7) * Math.sin(y * 9 - t * 1.3 + Math.sin(x * 5 + t)); return n * n; };
function preview(id, pal, t, n) {
  const out = [];
  for (let i = 0; i < n; i++) {
    const x = i / (n - 1);
    let c;
    switch (id) {
      case 'flow': c = palc(pal, x - t * 0.12); break;
      case 'pump': c = palc(pal, x - t * (0.12 + 0.08 * Math.sin(t * 0.5))); break;
      case 'caustic': c = scale(palc(pal, x * 0.6 + t * 0.03), 0.25 + 0.75 * causticN(x, 0.5, t)); break;
      case 'bubbles': {
        c = scale(pal[0], 0.12);
        for (let k = 0; k < 4; k++) {
          const pos = fract(t * 0.18 + k * 0.29) * 1.3 - 0.15, d = (pos - x) / 0.06;
          const bc = palc(pal, 0.33 + k * 0.21);
          c = [c[0] + bc[0] * Math.exp(-d * d), c[1] + bc[1] * Math.exp(-d * d), c[2] + bc[2] * Math.exp(-d * d)];
        }
        break;
      }
      case 'comet': {
        const head = fract(t * 0.2) * 1.35, d = head - x;
        c = scale(pal[Math.min(2, pal.length - 1)], 0.05);
        if (d >= 0 && d < 0.3) { const k = 1 - d / 0.3; const hc = mix(pal[0], pal[Math.min(1, pal.length - 1)], d / 0.3); c = [c[0] + hc[0] * k * k, c[1] + hc[1] * k * k, c[2] + hc[2] * k * k]; }
        break;
      }
      case 'lava': { const v = Math.sin(x * 6 + t * 0.8) + Math.sin(2.5 - t * 0.6) + Math.sin((x + 0.5) * 4 + t * 0.4); c = palc(pal, v / 6 + t * 0.02); break; }
      case 'breathe': { const ph = t * 0.35, k = Math.floor(ph); c = mix(pal[k % pal.length], pal[(k + 1) % pal.length], smooth01(ph - k)); break; }
      case 'temperature': c = scale(grad(pal, 0.5 + 0.5 * Math.sin(t * 0.25)), 0.8 + 0.2 * causticN(x, 0.5, t * 0.5)); break;
      case 'audio': { const lv = Math.abs(Math.sin(t * 2.3) * Math.sin(t * 0.9 + 1)); c = scale(grad(pal, x), x < lv ? 1 : 0.1); break; }
      case 'rainbow': c = hsvRgb(x * 0.9 - t * 0.15, 1, 1); break;
      case 'wave': {   // as effects.c: the palette in bands sweeping one way (the strip read left to right)
        const d = cv('wave', 'dir', 'right'), b = Math.max(1, Math.min(4, +cv('wave', 'size', 2))), u = d === 'left' ? 1 - x : d === 'out' ? Math.abs(x - 0.5) * 1.4 : d === 'in' ? 1 - Math.abs(x - 0.5) * 1.4 : x;
        c = palc(pal, u * b - t * 0.18); break;
      }
      case 'fire': {   // the strip as the middle of a flame
        const n = vnoise(x * 7 + t * 1.9, 3), h = Math.max(0, Math.min(1, 0.55 + (n - 0.5) * 0.9 + 0.12 * vnoise(t * 9 + x * 13, i)));
        c = grad(pal, h); break;
      }
      case 'ocean': {
        const u = x + 0.17, w = 0.4 * Math.sin(u * 9 + t * 0.71) + 0.3 * Math.sin(u * 13 - t * 1.13 + 1.3) + 0.2 * Math.sin(u * 21 + t * 1.7 + 2.1) + 0.1 * Math.sin(u * 31 - t * 2.3), v = 0.5 + 0.5 * w;
        c = grad(pal, v * 0.85); if (v > 0.8) c = add(c, scale(pal[pal.length - 1], (v - 0.8) * 2.5)); break;
      }
      case 'twinkle': {
        const ph = hashf(i * 31) * 6.2832, rate = 0.6 + hashf(i * 31 + 5) * 1.6, sn = Math.sin(t * rate * 1.6 + ph);
        c = add(scale(pal[pal.length - 1], 0.025), scale(palc(pal, hashf(i * 31 + 9)), sn > 0 ? Math.pow(sn, 7) : 0)); break;
      }
      case 'meteor': {
        c = scale(pal[pal.length - 1], 0.02);
        for (let m = 0; m < 2; m++) {
          const head = fract(t * 0.22 + m * 0.5) * 1.5 - 0.1, d = head - x;
          if (d < 0 || d > 0.4) continue;
          const k = 1 - d / 0.4, crumb = d < 0.03 ? 1 : 0.35 + 0.65 * hashf(i * 131 + Math.floor(t * 14));
          c = add(c, scale(mix(pal[0], pal[Math.min(1, pal.length - 1)], d / 0.4), k * k * crumb));
        }
        break;
      }
      case 'plasma': { const v = Math.sin(x * 10 + t) + Math.sin(4 - t * 1.3) + Math.sin((x + 0.5) * 7 + t * 0.7) + Math.sin(Math.abs(x - 0.5) * 14 - t * 1.6); c = palc(pal, v * 0.125 + 0.5 + t * 0.04); break; }
      case 'aurora': {
        const wave = Math.sin(x * 6 + t * 0.5 + 2 * Math.sin(x * 2.3 - t * 0.3)); let cu = smooth01((wave + 1) * 0.5); cu *= cu;
        const sh = 0.75 + 0.25 * vnoise(x * 30 + t * 3, 7), col = grad(pal, 0.5 + 0.5 * Math.sin(x * 2 + t * 0.2 + 0.4));
        c = add(scale(pal[0], 0.03), scale(col, cu * sh * 0.8)); break;
      }
      case 'ripple': {
        c = scale(pal[0], 0.03);
        for (let k = 0; k < 4; k++) {
          const tt = t * 0.45 + k * 0.25, slot = Math.floor(tt), age = tt - slot, id = slot * 4 + k;
          const cx = hashf(id * 97), d = Math.abs(x - cx), e = (d - age * 0.9) / 0.06, ring = Math.exp(-e * e) * (1 - age) * (1 - age);
          c = add(c, scale(palc(pal, hashf(id * 97 + 3)), ring));
        }
        break;
      }
      case 'matrix': {   // the strip as a row of columns: each lights up as its drop passes the middle
        const col = Math.floor(x * 24), sp = 0.35 + hashf(col * 31) * 0.5, head = fract(t * sp * 0.6 + hashf(col * 17)) * 1.6 - 0.3, dist = head - 0.5;
        c = scale(pal[0], 0.01);
        if (dist >= 0 && dist < 0.45) { const b = (1 - dist / 0.45) ** 2; c = scale(dist < 0.03 && pal.length > 1 ? pal[1] : pal[0], b * (0.7 + 0.3 * hashf(i * 13 + Math.floor(t * 15)))); }
        break;
      }
      case 'candle': {
        const f = vnoise(t * 6, 1) * 0.6 + vnoise(t * 13, 2) * 0.4, dip = vnoise(t * 1.3, 5) < 0.18 ? 0.6 : 1;
        c = scale(mix(pal[0], pal[Math.min(1, pal.length - 1)], vnoise(t * 2, 9)), (0.55 + 0.45 * f) * dip * (0.93 + 0.07 * hashf(i))); break;
      }
      case 'screen': {   // a moving picture, seen along the edge
        const v = 0.5 + 0.5 * Math.sin(x * 5 + t * 0.9) * Math.sin(t * 0.37 + x * 2);
        c = hsvRgb(0.58 + 0.25 * Math.sin(t * 0.21 + x * 1.5), 0.55, 0.25 + 0.6 * v); break;
      }
      case 'static': c = pal[0]; break;
      default: c = [14, 15, 19];
    }
    out.push(c);
  }
  return out;
}
function drawStrip(cv2, cols) {
  const c = cv2.getContext('2d'), w = cv2.width, h = cv2.height;
  const g = c.createLinearGradient(0, 0, w, 0);
  cols.forEach((col, i) => g.addColorStop(i / (cols.length - 1), css(col.map(v => Math.min(255, v)))));
  c.clearRect(0, 0, w, h);
  c.fillStyle = g; c.fillRect(0, 0, w, h);
  const sh = c.createLinearGradient(0, 0, 0, h);
  sh.addColorStop(0, 'rgba(255,255,255,.10)'); sh.addColorStop(.5, 'rgba(255,255,255,0)'); sh.addColorStop(1, 'rgba(0,0,0,.25)');
  c.fillStyle = sh; c.fillRect(0, 0, w, h);
}
let t0 = performance.now(), lastPrev = 0;
function animate(now) {
  requestAnimationFrame(animate);
  if (tab !== 'effects' || now - lastPrev < 33) return;
  lastPrev = now;
  const t = (now - t0) / 1000;
  const PR = presets();
  for (const b of $$('.fx')) {
    const cvs = b.querySelector('canvas'); if (!cvs) continue;
    const C = b.dataset.comm ? COMM.items.find(x => x.id === b.dataset.comm) : null;
    const P = C ? { effect: C.effect, palette: (C.palette || []).join(', '), speed: C.speed } : b.dataset.preset ? PR.find(p => p.id === +b.dataset.preset) : null, id = P ? P.effect : b.dataset.id;
    if (!id) continue;
    if (cvs.width !== cvs.clientWidth * devicePixelRatio) { cvs.width = cvs.clientWidth * devicePixelRatio; cvs.height = cvs.clientHeight * devicePixelRatio; }
    const pal = P && parsePal(P.palette).length ? parsePal(P.palette) : effPal(id);
    const spd = +(P && P.speed ? P.speed : cv(id, 'speed', cv('general', 'speed', 5))) / 5;
    drawStrip(cvs, preview(id, pal.map(hex2rgb), t * spd, 28));
  }
}
requestAnimationFrame(animate);

// ------------------------------------------------------------------ live drawing
function sizeCanvases() {
  for (const c of $$('canvas.live, #hero')) {
    const w = c.clientWidth, h = c.id === 'hero' ? c.parentElement.clientHeight : +c.dataset.h;
    if (!w) continue;
    c.style.height = h + 'px';
    c.width = w * devicePixelRatio; c.height = h * devicePixelRatio;
  }
  drawAll();
}
new ResizeObserver(sizeCanvases).observe($('main'));   // also follows the sidebar sliding in / out

function rr(c, x, y, w, h, r) { c.beginPath(); c.roundRect(x, y, Math.max(0, w), Math.max(0, h), Math.max(0, r)); }
const lit = col => col && col !== OFF;
function avgColor(list) {
  const on = list.filter(lit);
  if (!on.length) return null;
  const s = on.map(hex2rgb).reduce((a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]], [0, 0, 0]);
  return css(scale(s, 1 / on.length));
}

// A vertical diffuser bar: colours blend like a real light bar.
function lightBar(c, x, y, w, h, cols, vertical) {
  const g = vertical ? c.createLinearGradient(0, y + h, 0, y) : c.createLinearGradient(x, 0, x + w, 0);
  const n = cols.length;
  cols.forEach((col, i) => g.addColorStop(n > 1 ? i / (n - 1) : 0, col || OFF));
  const glow = avgColor(cols);
  c.save();
  if (glow) { c.shadowColor = glow; c.shadowBlur = 26 * devicePixelRatio; }
  rr(c, x, y, w, h, 1.5 * devicePixelRatio);
  c.fillStyle = g; c.fill();
  c.restore();
  const sh = vertical ? c.createLinearGradient(x, 0, x + w, 0) : c.createLinearGradient(0, y, 0, y + h);
  sh.addColorStop(0, 'rgba(255,255,255,.22)'); sh.addColorStop(.45, 'rgba(255,255,255,0)'); sh.addColorStop(1, 'rgba(0,0,0,.18)');
  rr(c, x, y, w, h, 1.5 * devicePixelRatio); c.fillStyle = sh; c.fill();
}

function drawRam(c, X, Y, W, H) {
  const d = devicePixelRatio, sw = Math.min(W / 4.2, 46 * d), sh = Math.min(H * 0.86, 150 * d), gap = sw * 0.55;
  const x0 = X + (W - (sw * 2 + gap)) / 2, y0 = Y + (H - sh) / 2;
  for (let s = 0; s < 2; s++) {
    const x = x0 + s * (sw + gap);
    const body = c.createLinearGradient(x, 0, x + sw, 0);
    body.addColorStop(0, '#141414'); body.addColorStop(.5, '#222'); body.addColorStop(1, '#121212');
    rr(c, x, y0, sw, sh, 1.5 * d); c.fillStyle = body; c.fill();
    c.strokeStyle = 'rgba(255,255,255,.06)'; c.lineWidth = d; c.stroke();
    const cols = [...Array(8)].map((_, i) => F.ram[s][i] || OFF);
    lightBar(c, x + sw * 0.26, y0 + sh * 0.05, sw * 0.48, sh * 0.9, cols, true);
  }
}

function drawGpu(c, X, Y, W, H) {
  const d = devicePixelRatio, bw = Math.min(W * 0.92, 520 * d), bh = Math.min(H * 0.62, 100 * d);
  const x = X + (W - bw) / 2, y = Y + (H - bh) / 2;
  const n = Math.max(F.gpu.length, +cv('layout', 'gpu_leds', 8));
  const cols = [...Array(n)].map((_, i) => F.gpu[i] || OFF);
  const glow = avgColor(cols);
  // coolant lit by the strip: the clear block glows from the top edge down
  const inner = c.createLinearGradient(0, y, 0, y + bh);
  inner.addColorStop(0, glow ? glow.replace('rgb', 'rgba').replace(')', ',.55)') : 'rgba(40,40,40,.5)');
  inner.addColorStop(1, 'rgba(14,14,14,.95)');
  rr(c, x, y, bw, bh, 2 * d); c.fillStyle = '#0e0e0e'; c.fill();
  rr(c, x, y, bw, bh, 2 * d); c.fillStyle = inner; c.fill();
  c.strokeStyle = 'rgba(255,255,255,.10)'; c.lineWidth = d; c.stroke();
  // fins / channels
  c.save(); rr(c, x, y, bw, bh, 2 * d); c.clip();
  c.strokeStyle = 'rgba(255,255,255,.035)'; c.lineWidth = 2 * d;
  for (let i = 1; i < 18; i++) { const fx = x + bw * i / 18; c.beginPath(); c.moveTo(fx, y + bh * 0.28); c.lineTo(fx, y + bh * 0.86); c.stroke(); }
  c.restore();
  lightBar(c, x + bw * 0.04, y + bh * 0.08, bw * 0.92, 7 * d, cols, false);
  // board LED
  if (F.board || cv('layout', 'board_led', '1') === '1') {
    const bx = x + bw - 10 * d, by = y + bh + 16 * d;
    c.save();
    if (lit(F.board)) { c.shadowColor = F.board; c.shadowBlur = 14 * d; }
    c.fillStyle = F.board || OFF; c.fillRect(bx - 3.5 * d, by - 3.5 * d, 7 * d, 7 * d); c.restore();
    c.fillStyle = THEME.lo; c.font = `${9.5 * d}px ${WIDE}`; c.textAlign = 'right';
    c.fillText(t('board'), bx - 10 * d, by + 4 * d);
  }
}

// Nanoleaf panels at their wall positions; panel size from the nearest neighbour.
// Nanoleaf panel shapes by shapeType (Nanoleaf OpenAPI): number of sides and edge length in layout units.
// Unknown types (e.g. Blocks) are drawn as squares sized from the distance to their neighbours.
const NANO_SHAPES = {
  0: [3, 150], 8: [3, 134], 9: [3, 67],          // Light Panels triangle, Shapes triangle, Shapes mini triangle
  2: [4, 100], 3: [4, 100], 4: [4, 100],         // Canvas squares
  33: [4, 134], 34: [4, 67],                     // Blocks square, Blocks mini square (they sit edge to edge)
  7: [6, 67], 14: [6, 134], 15: [6, 33.5],       // Shapes hexagon, Elements hexagon, Elements hexagon corner
  17: [2, 154], 18: [2, 77],                     // Lines (a bar)
};
// Every panel as a polygon: centre, circumradius r, and the angle of its first corner (canvas, radians).
// Shapes panels turn by their own angle (see below); other triangles and hexagons so that the edge they share with
// their nearest neighbour faces it, and a panel on its own by the angle from the controller.
function nanoGeometry(N) {
  const P = (N && N.panels) || [], unit = N.unit || 0, side = N.side || 0.25;
  const deg = Math.PI / 180;
  return P.map(([x, y, shape, ang = 0], i) => {
    const info = NANO_SHAPES[shape];
    let near = null, nd = 1e9, box = side;
    P.forEach(([x2, y2], j) => {
      if (i === j) return;
      const dd = Math.hypot(x2 - x, y2 - y);
      if (dd < nd) { nd = dd; near = [x2, y2]; }
      box = Math.min(box, Math.max(Math.abs(x - x2), Math.abs(y - y2)));
    });
    if (!info || !unit) return { x, y, n: 4, r: box / Math.SQRT2, a0: Math.PI / 4, bar: 0 };   // square, axis aligned
    const [n, len] = info, a = len * unit;
    if (n === 2) return { x, y, n, r: a / 2, a0: ang * deg, bar: a * 0.14 };
    const r = n === 3 ? a / Math.sqrt(3) : n === 4 ? a / Math.SQRT2 : a;
    const share = n === 3 ? a / Math.sqrt(3) : n === 4 ? a : a * Math.sqrt(3);   // centre distance of two panels sharing an edge
    let a0;
    if (shape === 7 || shape === 8 || shape === 9)
      // Shapes: the panel's own angle. Their corners sit at angle + 30 degrees (triangles) or + 0 (hexagons): with
      // these, two layouts (one mirrored) have no panel over another (found by checking every angle on them). Their
      // panels are not always edge to edge (a triangle's edge is two hexagon edges long), so no guessing from neighbours.
      a0 = ((n === 3 ? 30 : 0) + ang) * deg;
    else if (near && n !== 4 && Math.abs(nd - share) < share * 0.2) a0 = Math.atan2(near[1] - y, near[0] - x) + Math.PI / n;
    else a0 = (n === 3 ? 90 : n === 6 ? 0 : 45) * deg + ang * deg;
    return { x, y, n, r, a0, bar: 0 };
  });
}
function nanoPath(c, cx, cy, p, k, shrink) {
  c.beginPath();
  if (p.n === 2) {   // a light line: a thin bar
    const L = p.r * k * (1 - shrink), T = Math.max(p.bar * k, 2), dx = Math.cos(p.a0), dy = Math.sin(p.a0);
    c.moveTo(cx - dx * L - dy * T / 2, cy - dy * L + dx * T / 2); c.lineTo(cx + dx * L - dy * T / 2, cy + dy * L + dx * T / 2);
    c.lineTo(cx + dx * L + dy * T / 2, cy + dy * L - dx * T / 2); c.lineTo(cx - dx * L + dy * T / 2, cy - dy * L - dx * T / 2);
  } else {
    const r = p.r * k * (1 - shrink);
    for (let v = 0; v < p.n; v++) {
      const t = p.a0 + v * 2 * Math.PI / p.n;
      c[v ? 'lineTo' : 'moveTo'](cx + Math.cos(t) * r, cy + Math.sin(t) * r);
    }
  }
  c.closePath();
}
// One controller's panels (N), or with no N all controllers side by side (the preview); returns the hit areas.
function drawNano(c, X, Y, W, H, N) {
  const d = devicePixelRatio;
  if (!N) {
    const L = nanoCtls().filter(n => (n.panels || []).length), hits = [];
    if (L.length < 2) {
      const n = L[0] || curNano();
      drawNano(c, X, Y, W, H, n || {});
      return n ? [{ key: 'nano' + n.slot, k: 'nano', slot: n.slot, x: X + 6 * d, y: Y - 8 * d, w: W - 12 * d, h: H + 16 * d }] : [];
    }
    const w = W / L.length;
    L.forEach((n, i) => {
      drawNano(c, X + i * w, Y, w, H, n);
      hits.push({ key: 'nano' + n.slot, k: 'nano', slot: n.slot, x: X + i * w + 3 * d, y: Y - 8 * d, w: w - 6 * d, h: H + 16 * d });
    });
    return hits;
  }
  const G = nanoGeometry(N), cols = F.nano[N.slot - 1] || [];
  if (!G.length) {
    c.fillStyle = THEME.lo; c.font = `${12 * d}px ${WIDE}`; c.textAlign = 'center';
    c.fillText(nanoOn() ? t('nano.wait') : t('nano.none'), X + W / 2, Y + H / 2);
    return [];
  }
  let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
  G.forEach(p => { x0 = Math.min(x0, p.x - p.r); x1 = Math.max(x1, p.x + p.r); y0 = Math.min(y0, p.y - p.r); y1 = Math.max(y1, p.y + p.r); });
  const pad = 18 * d, k = Math.min((W - pad * 2) / Math.max(x1 - x0, 1e-6), (H - pad * 2) / Math.max(y1 - y0, 1e-6));
  const ox = X + (W - (x1 - x0) * k) / 2 - x0 * k, oy = Y + (H - (y1 - y0) * k) / 2 - y0 * k;
  G.forEach((p, i) => {
    const col = cols[i] || OFF, cx = ox + p.x * k, cy = oy + p.y * k, r = p.r * k;
    c.save();
    if (lit(col)) { c.shadowColor = col; c.shadowBlur = 30 * d; }
    nanoPath(c, cx, cy, p, k, 0.08); c.fillStyle = col; c.fill();
    c.restore();
    const sh = c.createLinearGradient(cx - r, cy - r, cx + r, cy + r);
    sh.addColorStop(0, 'rgba(255,255,255,.25)'); sh.addColorStop(.5, 'rgba(255,255,255,0)'); sh.addColorStop(1, 'rgba(0,0,0,.2)');
    nanoPath(c, cx, cy, p, k, 0.08); c.fillStyle = sh; c.fill();
    c.strokeStyle = 'rgba(255,255,255,.08)'; c.lineWidth = d; c.stroke();
  });
  return [];
}

// What a light is, for its picture in the preview (and, for LAN devices, how effects lay out on it; see
// device_type in devices.c). LAN devices get it from the core (chosen, or guessed from the model); bulbs: [zone.lightN] type.
const FIXTURES = ['strip', 'tv', 'bars', 'floor', 'lamp', 'panels', 'bulb', 'gpu', 'ram', 'board', 'fan', 'keyboard', 'mouse', 'keylight', 'gate', 'frame'];
const bulbType = i => { const v = cv('zone.light' + (i + 1), 'type', 'bulb'); return FIXTURES.includes(v) ? v : 'bulb'; };

// One light drawn as what it is, in the box X, Y, W, H: cols are its LEDs (index 0 = start of the strip).
function drawFixture(c, X, Y, W, H, type, cols, on) {
  const dp = devicePixelRatio, n = cols.length, cx = X + W / 2, cy = Y + H / 2;
  const col = i => (on && cols[i]) || OFF, glow = on ? avgColor(cols) : null;
  const metal = (x, y, w, h) => { rr(c, x, y, w, h, 1.5 * dp); c.fillStyle = '#1c1c1c'; c.fill(); c.strokeStyle = 'rgba(255,255,255,.07)'; c.lineWidth = dp; c.stroke(); };
  const orb = (x, y, r, k) => {
    const g = c.createRadialGradient(x, y - r * 0.1, 0, x, y, r * 1.7), on1 = on && lit(k);
    if (on1) { g.addColorStop(0, '#fff'); g.addColorStop(0.28, k); g.addColorStop(1, 'rgba(0,0,0,0)'); }
    else { g.addColorStop(0, '#262626'); g.addColorStop(0.5, '#161616'); g.addColorStop(1, 'rgba(0,0,0,0)'); }
    c.beginPath(); c.arc(x, y, r * 1.7, 0, 7); c.fillStyle = g; c.fill();
  };
  const seg = (a, b) => { const s = cols.slice(a, b).map((k, i) => col(a + i)); return s.length ? s : [col(0)]; };
  if (type === 'strip') {
    const bw = Math.min(W * 0.92, 560 * dp), bh = Math.min(H * 0.34, 14 * dp);
    lightBar(c, cx - bw / 2, cy - bh / 2, bw, bh, n > 1 ? cols.map((_, i) => col(i)) : [col(0), col(0)], false);
  } else if (type === 'bulb') {
    const m = Math.max(1, n), r = Math.min(H * 0.28, W / (m * 3.2), 24 * dp);
    for (let i = 0; i < m; i++) orb(cx + (i - (m - 1) / 2) * r * 3.2, cy, r, col(i));
  } else if (type === 'floor') {   // floor lamp: a tall glowing tube on a thin stand
    const h = H * 0.9, tw = Math.max(3 * dp, Math.min(h * 0.07, 9 * dp)), top = cy - h / 2;
    metal(cx - tw * 1.6, top + h - 3 * dp, tw * 3.2, 3 * dp);
    metal(cx - dp, top + h * 0.72, 2 * dp, h * 0.28);
    lightBar(c, cx - tw / 2, top, tw, h * 0.72, n > 1 ? cols.map((_, i) => col(i)) : [col(0), col(0)], true);
  } else if (type === 'bars') {   // two light bars side by side, the LEDs split between them
    const h = H * 0.8, bw = Math.max(3 * dp, Math.min(h * 0.08, 8 * dp)), gap = Math.min(W * 0.28, h * 0.45), half = Math.ceil(n / 2);
    [seg(0, half), seg(half, n)].forEach((s, j) => {
      const x = cx + (j ? gap / 2 : -gap / 2) - bw / 2;
      lightBar(c, x, cy - h / 2, bw, h * 0.9, s.length > 1 ? s : [s[0], s[0]], true);
      metal(x - bw * 0.6, cy + h / 2 - 2 * dp, bw * 2.2, 2.5 * dp);
    });
  } else if (type === 'tv') {   // a screen with the light around it (left edge up, top, right edge down, bottom)
    const sw = Math.min(W * 0.72, H * 0.8 * 16 / 9), sh = sw * 9 / 16, x = cx - sw / 2, y = cy - sh / 2, t = Math.max(2 * dp, sh * 0.05);
    const q = Math.max(1, Math.floor(n / 4)), edges = n >= 4 ? [seg(0, q), seg(q, 2 * q), seg(2 * q, 3 * q), seg(3 * q, n)] : [[col(0)], [col(0)], [col(0)], [col(0)]];
    const two = s => s.length > 1 ? s : [s[0], s[0]];
    lightBar(c, x - t * 1.6, y, t, sh, two(edges[0]), true);
    lightBar(c, x, y - t * 1.6, sw, t, two(edges[1]), false);
    lightBar(c, x + sw + t * 0.6, y, t, sh, two([...edges[2]].reverse()), true);
    lightBar(c, x, y + sh + t * 0.6, sw, t, two([...edges[3]].reverse()), false);
    rr(c, x, y, sw, sh, 2 * dp); c.fillStyle = '#0c0c0c'; c.fill(); c.strokeStyle = 'rgba(255,255,255,.1)'; c.lineWidth = dp; c.stroke();
  } else if (type === 'lamp') {   // table lamp: the shade glows
    const h = Math.min(H * 0.85, W * 0.9), top = cy - h / 2, sw = h * 0.62, g = glow || 'rgb(40,40,40)';
    metal(cx - h * 0.2, top + h - 3 * dp, h * 0.4, 3 * dp);
    metal(cx - dp, top + h * 0.5, 2 * dp, h * 0.5);
    c.save();
    if (glow) { c.shadowColor = glow; c.shadowBlur = 30 * dp; }
    c.beginPath(); c.moveTo(cx - sw * 0.32, top); c.lineTo(cx + sw * 0.32, top); c.lineTo(cx + sw / 2, top + h * 0.5); c.lineTo(cx - sw / 2, top + h * 0.5); c.closePath();
    const lg = c.createLinearGradient(0, top, 0, top + h * 0.5);
    lg.addColorStop(0, glow ? g : '#1a1a1a'); lg.addColorStop(1, glow ? '#fff' : '#222');
    c.fillStyle = lg; c.fill(); c.restore();
  } else if (type === 'gpu') {   // a graphics card: shroud with fans, the light along its top edge
    const w = Math.min(W * 0.92, H * 2.6), h = w * 0.36, x = cx - w / 2, y = cy - h / 2 + h * 0.08;
    rr(c, x, y, w, h, 4 * dp); c.fillStyle = '#161616'; c.fill(); c.strokeStyle = 'rgba(255,255,255,.09)'; c.lineWidth = dp; c.stroke();
    const fr = h * 0.36;
    for (let i = 0; i < 3; i++) {
      const fx = x + w * (0.2 + i * 0.3), fy = y + h * 0.56;
      c.beginPath(); c.arc(fx, fy, fr, 0, 7); c.fillStyle = '#0d0d0d'; c.fill(); c.strokeStyle = 'rgba(255,255,255,.08)'; c.stroke();
      c.beginPath(); c.arc(fx, fy, fr * 0.28, 0, 7); c.fillStyle = '#1e1e1e'; c.fill();
    }
    lightBar(c, x + w * 0.05, y - Math.max(2 * dp, h * 0.07) - dp, w * 0.9, Math.max(2 * dp, h * 0.07), n > 1 ? cols.map((_, i) => col(i)) : [col(0), col(0)], false);
  } else if (type === 'ram') {   // a memory stick standing up, lit along its top
    const h = H * 0.9, w = Math.max(6 * dp, Math.min(h * 0.16, W * 0.3)), x = cx - w / 2, y = cy - h / 2;
    metal(x, y + h * 0.3, w, h * 0.7);
    lightBar(c, x + w * 0.12, y, w * 0.76, h * 0.34, n > 1 ? cols.map((_, i) => col(i)) : [col(0), col(0)], true);
  } else if (type === 'board') {   // a motherboard: the board, a socket, slots, and its light down one edge
    const s = Math.min(W * 0.8, H * 0.9), x = cx - s / 2, y = cy - s / 2;
    rr(c, x, y, s, s, 3 * dp); c.fillStyle = '#131313'; c.fill(); c.strokeStyle = 'rgba(255,255,255,.08)'; c.lineWidth = dp; c.stroke();
    metal(x + s * 0.3, y + s * 0.14, s * 0.3, s * 0.3);
    for (let i = 0; i < 2; i++) metal(x + s * 0.12, y + s * (0.62 + i * 0.14), s * 0.62, s * 0.05);
    lightBar(c, x + s * 0.86, y + s * 0.1, Math.max(2 * dp, s * 0.04), s * 0.8, n > 1 ? cols.map((_, i) => col(i)) : [col(0), col(0)], true);
  } else if (type === 'fan') {   // a fan: its ring lit around, the LEDs going round
    const r = Math.min(W, H) * 0.4, m = Math.max(n, 8);
    c.save();
    if (glow) { c.shadowColor = glow; c.shadowBlur = 18 * dp; }
    c.lineWidth = Math.max(3 * dp, r * 0.16);
    for (let i = 0; i < m; i++) {
      c.beginPath(); c.arc(cx, cy, r, -Math.PI / 2 + i / m * Math.PI * 2, -Math.PI / 2 + (i + 1.05) / m * Math.PI * 2);
      c.strokeStyle = col(Math.floor(i * Math.max(n, 1) / m)); c.stroke();
    }
    c.restore();
    c.beginPath(); c.arc(cx, cy, r * 0.8, 0, 7); c.fillStyle = '#0e0e0e'; c.fill();
    c.strokeStyle = 'rgba(255,255,255,.06)'; c.lineWidth = dp;
    for (let i = 0; i < 7; i++) { const a = i / 7 * Math.PI * 2; c.beginPath(); c.moveTo(cx + Math.cos(a) * r * 0.22, cy + Math.sin(a) * r * 0.22); c.quadraticCurveTo(cx + Math.cos(a + .5) * r * 0.6, cy + Math.sin(a + .5) * r * 0.6, cx + Math.cos(a + .9) * r * 0.76, cy + Math.sin(a + .9) * r * 0.76); c.stroke(); }
    c.beginPath(); c.arc(cx, cy, r * 0.22, 0, 7); c.fillStyle = '#1c1c1c'; c.fill();
  } else if (type === 'keyboard') {   // a keyboard: rows of keys, the LEDs spread over the columns
    const w = Math.min(W * 0.95, H * 3.2), h = w * 0.33, x = cx - w / 2, y = cy - h / 2, cols_ = 15, rows = 5;
    rr(c, x, y, w, h, 4 * dp); c.fillStyle = '#121212'; c.fill(); c.strokeStyle = 'rgba(255,255,255,.08)'; c.lineWidth = dp; c.stroke();
    const kw = (w - 6 * dp) / cols_, kh = (h - 6 * dp) / rows;
    for (let r = 0; r < rows; r++) for (let k = 0; k < cols_; k++) {
      // per-key LEDs go row by row; a few (a keyboard driven by columns, e.g. a Wooting) spread across
      const i = n <= 24 ? Math.floor(k * Math.max(n, 1) / cols_) : Math.floor((r * cols_ + k) * Math.max(n, 1) / (rows * cols_)), kc = col(i);
      rr(c, x + 3 * dp + k * kw + kw * 0.1, y + 3 * dp + r * kh + kh * 0.12, kw * 0.8, kh * 0.76, 1.5 * dp);
      c.fillStyle = on && lit(kc) ? kc : '#1d1d1d'; c.fill();
    }
  } else if (type === 'gate') {   // Divoom Times Gate: five screens in a cross, its light glowing behind them
    const u = Math.min(W / 3.6, H / 3.6), k = col(0), on1 = on && lit(k);
    if (on1) { const g = c.createRadialGradient(cx, cy, 0, cx, cy, u * 2.6); g.addColorStop(0, k); g.addColorStop(1, 'rgba(0,0,0,0)'); c.globalAlpha = .55; c.fillStyle = g; c.fillRect(cx - u * 2.6, cy - u * 2.6, u * 5.2, u * 5.2); c.globalAlpha = 1; }
    [[0, -1], [-1, 0], [0, 0], [1, 0], [0, 1]].forEach(([dx, dy]) => {
      const x = cx + dx * u * 1.08 - u / 2, y = cy + dy * u * 1.08 - u / 2;
      rr(c, x, y, u, u, 2 * dp); c.fillStyle = '#101010'; c.fill();
      c.strokeStyle = on1 ? k : 'rgba(255,255,255,.1)'; c.lineWidth = dp; c.stroke();
    });
  } else if (type === 'frame') {   // Divoom Times Frame: a clear 10" screen on a foot, its light along the edges
    const w = Math.min(W * 0.8, H * 1.35), h = w * 0.62, x = cx - w / 2, y = cy - h / 2 - H * 0.04, k = col(0), on1 = on && lit(k);
    metal(cx - w * 0.2, y + h + 2 * dp, w * 0.4, 4 * dp);
    c.save();
    if (on1) { c.shadowColor = k; c.shadowBlur = 18 * dp; }
    rr(c, x, y, w, h, 3 * dp); c.strokeStyle = on1 ? k : 'rgba(255,255,255,.12)'; c.lineWidth = 2.5 * dp; c.stroke();
    c.restore();
    rr(c, x + 5 * dp, y + 5 * dp, w - 10 * dp, h - 10 * dp, 2 * dp); c.fillStyle = 'rgba(20,24,28,.85)'; c.fill();
    c.strokeStyle = 'rgba(255,255,255,.05)'; c.lineWidth = dp;
    for (let i = 1; i < 4; i++) { c.beginPath(); c.moveTo(x + w * i / 4 - 8 * dp, y + 8 * dp); c.lineTo(x + w * i / 4 - 20 * dp, y + h - 8 * dp); c.stroke(); }
  } else if (type === 'keylight') {   // a key light: a flat glowing panel on a pole, tilted a little towards you
    const h = H * 0.86, pw = Math.min(W * 0.62, h * 0.9), ph = pw * 0.62, top = cy - h / 2;
    metal(cx - dp, top + ph * 0.8, 2 * dp, h - ph * 0.8 - 3 * dp);
    metal(cx - pw * 0.28, top + h - 3 * dp, pw * 0.56, 3 * dp);
    c.save();
    if (glow) { c.shadowColor = glow; c.shadowBlur = 26 * dp; }
    rr(c, cx - pw / 2, top, pw, ph, 3 * dp); c.fillStyle = '#141414'; c.fill();
    c.restore();
    const g = c.createLinearGradient(0, top, 0, top + ph), k = col(0), on1 = on && lit(k);
    g.addColorStop(0, on1 ? '#fff' : '#202020'); g.addColorStop(1, on1 ? k : '#171717');
    rr(c, cx - pw / 2 + 3 * dp, top + 3 * dp, pw - 6 * dp, ph - 6 * dp, 2 * dp); c.fillStyle = g; c.globalAlpha = on1 ? .92 : 1; c.fill(); c.globalAlpha = 1;
    c.strokeStyle = 'rgba(255,255,255,.08)'; c.lineWidth = dp; rr(c, cx - pw / 2, top, pw, ph, 3 * dp); c.stroke();
  } else if (type === 'mouse') {   // a mouse from above, its logo and side light
    const h = Math.min(H * 0.85, W * 1.2), w = h * 0.58, x = cx - w / 2, y = cy - h / 2;
    c.save();
    if (glow) { c.shadowColor = glow; c.shadowBlur = 20 * dp; }
    rr(c, x, y, w, h, w * 0.5); c.fillStyle = '#151515'; c.fill();
    c.restore();
    c.strokeStyle = 'rgba(255,255,255,.08)'; c.lineWidth = dp; rr(c, x, y, w, h, w * 0.5); c.stroke();
    c.beginPath(); c.moveTo(cx, y); c.lineTo(cx, y + h * 0.36); c.stroke();
    orb(cx, y + h * 0.68, w * 0.12, col(Math.floor(n / 2)));
  } else if (type === 'panels') {   // a honeycomb of hexagons, the LEDs spread over them
    const m = Math.min(Math.max(n, 3), 7), R = Math.min(W / (m * 1.9 + 1), H * 0.3);
    for (let i = 0; i < m; i++) {
      const x = cx + (i - (m - 1) / 2) * R * 1.75, y = cy + (i % 2 ? R * 0.5 : -R * 0.5), k = col(Math.floor(i * Math.max(n, 1) / m));
      c.save();
      if (on && lit(k)) { c.shadowColor = k; c.shadowBlur = 22 * dp; }
      c.beginPath();
      for (let v = 0; v < 6; v++) { const a = Math.PI / 6 + v * Math.PI / 3; c[v ? 'lineTo' : 'moveTo'](x + Math.cos(a) * R * 0.92, y + Math.sin(a) * R * 0.92); }
      c.closePath(); c.fillStyle = k; c.fill(); c.restore();
      c.strokeStyle = 'rgba(255,255,255,.08)'; c.lineWidth = dp; c.stroke();
    }
  }
}

// A LAN device, drawn as its type, from the thinned-out live frame.
function drawExt(c, X, Y, W, H, k) {
  const d = S.ext.devs[k];
  if (!d) return;
  const cols = F.ext[k] || [], on = d.online && d.enabled, n = Math.max(1, cols.length || Math.min(d.leds || 1, 60));
  drawFixture(c, X, Y, W, H, d.type || (d.per_led ? 'strip' : 'bulb'), [...Array(n)].map((_, i) => cols[i] || OFF), on);
}
function label(c, text, x, y, bright) {
  const d = devicePixelRatio;
  c.fillStyle = bright ? THEME.hi : THEME.lo; c.font = `${9.5 * d}px ${WIDE}`; c.textAlign = 'center'; c.letterSpacing = `${3 * d}px`;
  c.fillText(text.toUpperCase(), x, y);
}

// The preview is also the quick-access widget: every device drawn in it is a hit area (HERO.hits, canvas pixels)
// that opens its settings in a floating sheet. [ui] hero_hide lists what is left out of the preview: whole groups
// (ram, gpu, nano, bulbs, ext) or single tiles (nano<slot>, bulb<i>, dev<id>).
const HERO = { hits: [], hover: '', hoverPw: '', anim: {}, t: 0, k: 1, moving: false, raf: 0 };
const HERO_GROUPS = [['ram', 0.2], ['gpu', 0.38], ['nano', 0.27], ['bulbs', 0.15], ['ext', 0.3]];
const heroGroups = () => HERO_GROUPS.filter(([k]) => (k === 'ram' && S.sticks) || (k === 'gpu' && S.msi) || (k === 'nano' && nanoOn()) ||
  (k === 'bulbs' && S.bulbs.length) || (k === 'ext' && S.ext.devs.length));
const heroHidden = () => cv('ui', 'hero_hide', '').split(',').filter(Boolean);
const groupName = k => ({ ram: t('pc.memory'), gpu: stripName(), nano: 'Nanoleaf', bulbs: t('nav.bulbs'), ext: t('nav.devices') })[k];

// Eased 0..1 value per preview item (a group, a bulb, a LAN device): every draw moves it part of the way to its
// target, frame-rate independent. While anything is still moving, drawHero keeps itself going.
function heroAnim(key, target) {
  let v = HERO.anim[key] ?? 0;
  if (document.body.classList.contains('calm')) v = target;
  else { v += (target - v) * HERO.k; if (Math.abs(target - v) < .004) v = target; }
  HERO.anim[key] = v;
  if (v !== target) HERO.moving = true;
  return v;
}

// ---- the preview as tiles: every device is a tile of its own, in reading order (memory, strip, each Nanoleaf
// controller, each bulb, each LAN device), laid out in rows; a row that is full wraps to the next one. Full rows
// stretch a little to fill the width, the last row stays left-aligned. The preview grows by whole rows (CSS
// animates the height) and tiles glide to their new places when something is added or removed.
const HERO_EXT_W = { strip: 2.4, tv: 1.7, bars: 1.1, floor: .9, lamp: 1, panels: 1.7, gpu: 2, ram: .9, board: 1.2, fan: 1, keyboard: 2.2, mouse: .8, keylight: 1, gate: 1.2, frame: 1.4 };
function heroItems(all) {
  const hide = all ? [] : heroHidden(), items = [];
  if (S.sticks && !hide.includes('ram')) items.push({ key: 'ram', k: 'ram', zone: 'zone.ram', w: 1.1, name: t('pc.memory'), draw: drawRam });
  if (S.msi && !hide.includes('gpu')) items.push({ key: 'gpu', k: 'gpu', zone: 'zone.gpu', w: 2.4, name: stripName(), draw: drawGpu });
  if (!hide.includes('nano')) nanoCtls().filter(n => !hide.includes('nano' + n.slot)).forEach(n => {
    const P = n.panels || [], xs = P.map(p => p[0]), ys = P.map(p => p[1]), sd = n.side || .2;
    const a = P.length ? (Math.max(...xs) - Math.min(...xs) + sd) / (Math.max(...ys) - Math.min(...ys) + sd) : 1;
    items.push({ key: 'nano' + n.slot, k: 'nano', slot: n.slot, zone: nanoZone(n.slot), w: Math.max(1.1, Math.min(3, a * 1.25)), name: nanoCtls().length > 1 ? nanoName(n) : 'Nanoleaf',
      draw: (c, x, y, w, h) => drawNano(c, x, y, w, h, n) });
  });
  if (!hide.includes('bulbs')) S.bulbs.forEach((b, i) => hide.includes('bulb' + i) || items.push({ key: 'bulb' + i, k: 'bulb', i, zone: 'zone.light' + (i + 1), w: .85, name: t('bulb', i + 1),
    draw: (c, x, y, w, h) => { const col = F.bulbs[i], on = lit(col) && b.online; drawFixture(c, x, y, w, h, bulbType(i), [on ? col : OFF], on); } }));
  if (!hide.includes('ext')) S.ext.devs.forEach((dv, k) => {
    if (hide.includes('dev' + dv.id)) return;
    const ty = dv.type || (dv.per_led ? 'strip' : 'bulb');
    items.push({ key: 'ext' + dv.id, k: 'ext', i: k, id: dv.id, zone: 'zone.dev' + dv.id, name: dv.name,
      w: HERO_EXT_W[ty] || Math.min(2.4, .55 + .35 * Math.max(1, dv.leds || 1)), draw: (c, x, y, w, h) => drawExt(c, x, y, w, h, k) });
  });
  // the order the user chose by dragging ([ui] hero_order); devices not in it keep their places after those that are
  const ord = cv('ui', 'hero_order', '').split(',').filter(Boolean);
  if (ord.length) {
    const at = it => { const i = ord.indexOf(it.key); return i < 0 ? ord.length + items.indexOf(it) : i; };
    items.sort((a, b) => at(a) - at(b));
  }
  return items;
}
// two devices swap their places in the preview
function heroMove(key, toKey) {
  const keys = heroItems(true).map(it => it.key), from = keys.indexOf(key), to = keys.indexOf(toKey);
  if (from < 0 || to < 0 || from === to) return;
  [keys[from], keys[to]] = [keys[to], keys[from]];
  setCfg('ui', 'hero_order', keys.join(','));
}
// What the preview shows, in reading order: single devices and groups (a group sits where its first device would).
function heroEntries() {
  const gs = groups(), out = [], seen = new Map();
  for (const it of heroItems()) {
    const g = gs.find(x => x.members.includes(it.zone));
    if (!g) { out.push({ it }); continue; }
    if (!seen.has(g.n)) { const e = { g, items: [] }; seen.set(g.n, e); out.push(e); }
    seen.get(g.n).items.push(it);
  }
  return out;
}
// Tile rectangles in CSS pixels for a preview W wide; returns { tiles: [{ it, x, y, w, h, group }], frames: [{ g, x,
// y, w, h }], height }. A group is a frame with a header around its devices: as wide as they are, or the full width
// with its devices in rows of their own when they do not fit in one. A row is as tall as its tallest entry.
function heroLayout(W) {
  const phone = matchMedia('(max-width: 700px)').matches;
  const U = phone ? 70 : 92, RH = phone ? 118 : 144, G = 10, P = 14, GH = phone ? 24 : 28, GP = 7, inner = Math.max(40, W - P * 2);
  const pack = (list, width) => {
    const rows = []; let row = [], used = 0;
    for (const b of list) {
      b.w = Math.min(b.w, width);
      if (row.length && (b.full || used + G + b.w > width)) { rows.push(row); row = []; used = 0; }
      used += (row.length ? G : 0) + b.w; row.push(b);
      if (b.full) { rows.push(row); row = []; used = 0; }
    }
    if (row.length) rows.push(row);
    return rows;
  };
  // full rows stretch to the width (at most 1.7x), the last one no more than the first; place(b, x, w, row)
  const spread = (rows, width, reserve, place) => {
    let f0 = 1.7;
    rows.forEach((r, ri) => {
      const last = ri === rows.length - 1, sum = r.reduce((a, b) => a + b.w, 0), gaps = G * (r.length - 1);
      let f = Math.min(1.7, (width - (last ? reserve : 0) - gaps) / sum);
      if (last && rows.length > 1) f = Math.min(f, f0);
      if (!ri) f0 = f;
      let x = 0;
      for (const b of r) { place(b, x, b.w * f, ri); x += b.w * f + G; }
    });
  };
  const boxes = heroEntries().map(e => {
    if (e.it) return { it: e.it, w: e.it.w * U, h: RH };
    const nat = GP * 2 + e.items.reduce((a, it) => a + it.w * U, 0) + G * (e.items.length - 1);
    if (nat <= inner) return { g: e.g, items: e.items, w: nat, h: GH + RH + GP };
    const inRows = pack(e.items.map(it => ({ it, w: it.w * U })), inner - GP * 2);
    return { g: e.g, items: e.items, w: inner, full: true, inRows, h: GH + inRows.length * RH + (inRows.length - 1) * G + GP };
  });
  const rows = pack(boxes, inner), tiles = [], frames = [];
  let y = P;
  const rowH = rows.map(r => Math.max(...r.map(b => b.h)));
  // the last row leaves the bottom-right corner to the preview's settings button (#hero-edit)
  spread(rows, inner, 40, (b, x, w, ri) => {
    const by = P + rowH.slice(0, ri).reduce((a, h) => a + h + G, 0), h = rowH[ri], bx = P + x;
    if (b.it) { tiles.push({ it: b.it, x: bx, y: by, w, h }); return; }
    frames.push({ g: b.g, x: bx, y: by, w, h });
    const n = b.g.n;
    if (!b.full) {   // one row inside: the devices share the frame's width by their own widths
      const sum = b.items.reduce((a, it) => a + it.w, 0), aw = w - GP * 2 - G * (b.items.length - 1);
      let ix = bx + GP;
      for (const it of b.items) { const iw = aw * it.w / sum; tiles.push({ it, x: ix, y: by + GH, w: iw, h: h - GH - GP, group: n }); ix += iw + G; }
    } else spread(b.inRows, w - GP * 2, 0, (m, mx, mw, mri) => tiles.push({ it: m.it, x: bx + GP + mx, y: by + GH + mri * (RH + G), w: mw, h: RH, group: n }));
  });
  y = P + rowH.reduce((a, h) => a + h, 0) + G * Math.max(0, rows.length - 1) + P;
  return { tiles, frames, height: rows.length ? y : (phone ? 150 : 180) };
}
function heroHeight() {
  const el = $('#tab-effects .hero'), bar = $('#arrange-bar');
  bar.classList.toggle('hidden', !HERO.arrange);
  const h = Math.round(heroLayout($('#hero').clientWidth || el.clientWidth).height + (HERO.arrange ? bar.offsetHeight + 12 : 0));
  if (+el.dataset.h !== h) { el.dataset.h = h; el.style.height = h + 'px'; }
}
// a tile's outline: faceted like the cards (top-left and bottom-right corners cut)
function tilePath(c, x, y, w, h, cut) {
  c.beginPath();
  c.moveTo(x + cut, y); c.lineTo(x + w, y); c.lineTo(x + w, y + h - cut); c.lineTo(x + w - cut, y + h);
  c.lineTo(x, y + h); c.lineTo(x, y + cut); c.closePath();
}
function tileLabel(c, text, x, y, w, bright) {
  const d = devicePixelRatio;
  c.font = `${9 * d}px ${WIDE}`; c.letterSpacing = `${2 * d}px`; c.textAlign = 'center';
  c.fillStyle = bright ? THEME.hi : THEME.lo;
  let s = text.toUpperCase();
  if (c.measureText(s).width > w) { while (s.length > 1 && c.measureText(s + '…').width > w) s = s.slice(0, -1); s += '…'; }
  c.fillText(s, x + w / 2, y);
  c.letterSpacing = '0px';
}

function drawHero() {
  const cvs = $('#hero'); if (!cvs.width) return;
  const now = performance.now();
  HERO.k = 1 - Math.exp(-Math.min(100, now - (HERO.t || now - 16)) / 120);
  HERO.t = now; HERO.moving = false;
  heroHeight();
  const c = cvs.getContext('2d'), W = cvs.width, H = cvs.height, d = devicePixelRatio;
  c.clearRect(0, 0, W, H);
  const { tiles, frames } = heroLayout(W / d), calm = document.body.classList.contains('calm'), hits = [], ghits = [];
  HERO.pos = HERO.pos || {};
  const seen = new Set(), drag = HERO.drag, to = drag && drag.to;
  const glide = (key, T) => {   // eased position of a tile or frame (a new one starts in place)
    let p = HERO.pos[key];
    if (!p) p = HERO.pos[key] = { x: T.x, y: T.y, w: T.w, h: T.h };
    for (const k of ['x', 'y', 'w', 'h']) {
      const tgt = T[k];
      if (p[k] == null || calm || Math.abs(tgt - p[k]) < .5) p[k] = tgt; else { p[k] += (tgt - p[k]) * HERO.k; HERO.moving = true; }
    }
    return p;
  };
  for (const F of frames) {
    const key = 'g' + F.g.n; seen.add(key);
    const p = glide(key, F), v = heroAnim('a:' + key, 1), x = p.x * d, y = p.y * d, w = p.w * d, h = p.h * d;
    const hot = HERO.hover === key || (to && to.key === key);
    c.save(); c.globalAlpha = v;
    tilePath(c, x, y, w, h, 11 * d);
    c.fillStyle = hot ? inkA(.035) : inkA(.012); c.fill();
    c.strokeStyle = to && to.key === key ? inkA(.6) : hot ? inkA(.26) : inkA(.1);
    c.lineWidth = d; c.setLineDash(to && to.key === key ? [5 * d, 4 * d] : []); c.stroke(); c.setLineDash([]);
    c.font = `${9 * d}px ${WIDE}`; c.letterSpacing = `${2.5 * d}px`; c.textAlign = 'left';
    c.fillStyle = hot ? THEME.hi : THEME.mid;
    let s = groupTitle(F.g).toUpperCase();
    const room = w - 46 * d;
    if (c.measureText(s).width > room) { while (s.length > 1 && c.measureText(s + '…').width > room) s = s.slice(0, -1); s += '…'; }
    c.fillText(s, x + 13 * d, y + 17 * d); c.letterSpacing = '0px';
    c.restore();
    ghits.push({ key, k: 'group', g: F.g.n, x, y, w, h });
  }
  for (const T of tiles) {
    const { it } = T; seen.add(it.key);
    const p = glide(it.key, T);   // glides from where the tile was (a new tile fades / grows in)
    const v = heroAnim('a:' + it.key, 1);
    const x = p.x * d, y = p.y * d, w = p.w * d, h = p.h * d, hot = HERO.hover === it.key || (to && to.key === it.key);
    c.save();
    c.globalAlpha = drag && drag.src.key === it.key ? v * .35 : v;
    const sc = .92 + .08 * v; c.translate(x + w / 2, y + h / 2); c.scale(sc, sc);
    if (HERO.arrange && !calm && !(drag && drag.src.key === it.key)) { c.rotate(Math.sin(now / 95 + it.key.length * 1.7 + x * .01) * .011); HERO.moving = true; }
    c.translate(-x - w / 2, -y - h / 2);
    tilePath(c, x, y, w, h, 8 * d);
    c.fillStyle = hot ? inkA(.04) : inkA(.015); c.fill();
    c.strokeStyle = to && to.key === it.key ? inkA(.6) : hot ? inkA(.28) : inkA(.06); c.lineWidth = d;
    c.setLineDash(to && to.key === it.key ? [5 * d, 4 * d] : []); c.stroke(); c.setLineDash([]);
    c.save(); tilePath(c, x, y, w, h, 8 * d); c.clip();   // a device never draws outside its tile
    try { it.draw(c, x + 8 * d, y + 22 * d, w - 16 * d, h - 50 * d); } catch (e) { console.warn('preview', it.key, e); }
    c.restore();
    // given back to its own lighting: what it shows is not haku's, said on the tile
    if (it.k === 'ram' ? S.ram_own : it.k === 'gpu' ? S.msi_own : it.k === 'ext' ? (S.ext.devs[it.i] || {}).own : 0) {
      c.font = `${8 * d}px ${WIDE}`; c.letterSpacing = `${1.5 * d}px`; c.textAlign = 'center'; c.fillStyle = THEME.mid;
      let s = t('hero.own').toUpperCase();
      while (s.length > 1 && c.measureText(s).width > w - 16 * d) s = s.slice(0, -2) + '…';
      c.fillText(s, x + w / 2, y + h - 26 * d); c.letterSpacing = '0px';
    }
    tileLabel(c, it.name, x + 8 * d, y + h - 12 * d, w - 16 * d, hot);
    c.restore();
    hits.push({ key: it.key, k: it.k, i: it.i, id: it.id, slot: it.slot, zone: it.zone, name: it.name, group: T.group || 0, x, y, w, h });
  }
  hits.push(...ghits);   // a device in a group is found before its group
  for (const k of Object.keys(HERO.pos)) if (!seen.has(k)) { delete HERO.pos[k]; delete HERO.anim['a:' + k]; }
  if (!tiles.length && S.effects.length) {   // nothing to show yet: the whole preview invites to add a device
    c.fillStyle = HERO.hover === 'add' ? THEME.hi : THEME.lo; c.font = `${13 * d}px ${WIDE}`; c.textAlign = 'center';
    c.fillText('+  ' + t('hero.empty'), W / 2, H / 2);
    hits.push({ key: 'add', k: 'add', x: W * .3, y: H * .3, w: W * .4, h: H * .4 });
  }
  HERO.hits = hits;   // (before the power buttons: a group's button asks its devices)
  if (!HERO.arrange) hits.forEach(h => heroPowerButton(c, h));
  if (drag && drag.px != null) {   // the device being dragged: its name under the pointer, and what dropping does
    const msg = to && to.group ? t('group.drop.in', groupTitle(groups().find(g => g.n === to.group) || { n: to.group }))
      : to && to.swap ? t('hero.drop.move') : to && to.tile ? t('group.drop.new') : drag.src.group && !HERO.arrange ? t('group.drop.out') : '';
    c.save();
    c.font = `${11 * d}px ${WIDE}`; c.textAlign = 'left';
    const s = drag.src.name + (msg ? '  ·  ' + msg : ''), tw = c.measureText(s).width, px = Math.min(drag.px + 14 * d, W - tw - 24 * d), py = drag.py + 18 * d;
    rr(c, px, py - 14 * d, tw + 20 * d, 22 * d, 11 * d);
    c.fillStyle = THEME.tip; c.fill(); c.strokeStyle = inkA(.35); c.lineWidth = d; c.stroke();
    c.fillStyle = THEME.hi; c.fillText(s, px + 10 * d, py + 1 * d);
    c.restore();
  }
  if (HERO.moving && !HERO.raf) HERO.raf = requestAnimationFrame(() => { HERO.raf = 0; if (tab === 'effects') drawHero(); });
}
// the canvas follows the card while its height animates
new ResizeObserver(() => {
  const cvs = $('#hero'), h = cvs.parentElement.clientHeight, w = cvs.clientWidth;
  if (!w || !h || (cvs.height === Math.round(h * devicePixelRatio) && cvs.width === Math.round(w * devicePixelRatio))) return;
  cvs.style.height = h + 'px'; cvs.width = w * devicePixelRatio; cvs.height = h * devicePixelRatio;
  drawHero();
}).observe($('#tab-effects .hero'));
const heroXY = e => { const r = $('#hero').getBoundingClientRect(), d = devicePixelRatio; return [(e.clientX - r.left) * d, (e.clientY - r.top) * d]; };
const heroHit = e => {
  const [px, py] = heroXY(e);
  return HERO.hits.find(h => px >= h.x && px <= h.x + h.w && py >= h.y && py <= h.y + h.h);
};
// the power button of the device under the pointer, if the pointer is on it
const heroOnPower = (h, e) => { if (!h || !h.pw) return false; const [px, py] = heroXY(e); return Math.hypot(px - h.pw.x, py - h.pw.y) <= h.pw.r * 1.5; };
// Drag a device (with the mouse) onto another one: a new group of the two; onto a group: into it; out of its group's
// frame: out of the group.
function heroDropTarget(e) {
  const [px, py] = heroXY(e), src = HERO.drag.src, inside = h => px >= h.x && px <= h.x + h.w && py >= h.y && py <= h.y + h.h;
  const tile = HERO.hits.find(h => h.zone && h.key !== src.key && inside(h));
  if (HERO.arrange) return tile ? { tile, key: tile.key, swap: true } : null;   // arranging: only swaps
  if (tile) return tile.group ? (tile.group === src.group ? null : { group: tile.group, key: 'g' + tile.group }) : { tile, key: tile.key };
  const fr = HERO.hits.find(h => h.k === 'group' && inside(h));
  if (fr) return fr.g === src.group ? null : { group: fr.g, key: fr.key };
  return src.group ? { out: true } : null;
}
function heroDrop(d) {
  const to = d.to, z = d.src.zone;
  if (!to) return;
  if (to.group) groupAdd(to.group, z);
  else if (to.out) groupRemove(z);
  else if (to.swap) heroMove(d.src.key, to.tile.key);
  else if (to.tile) {
    const n = groupNew(); if (!n) return;
    groupRemove(z);
    setMembers(n, [to.tile.zone, z]);
    groupDialog(n, true);   // a name for it
  }
}
$('#hero').addEventListener('pointerdown', e => {
  if (e.button !== 0 || e.pointerType === 'touch') return;
  const h = heroHit(e);
  HERO.press = h && h.zone && !heroOnPower(h, e) ? { h, x: e.clientX, y: e.clientY, id: e.pointerId } : null;
});
$('#hero').addEventListener('pointermove', e => {
  const P = HERO.press;
  if (P && !HERO.drag && e.buttons === 1 && Math.hypot(e.clientX - P.x, e.clientY - P.y) > 6) {
    HERO.drag = { src: P.h };
    try { $('#hero').setPointerCapture(P.id); } catch (x) { }
  }
  if (HERO.drag) {
    [HERO.drag.px, HERO.drag.py] = heroXY(e);
    HERO.drag.to = heroDropTarget(e);
    HERO.hover = HERO.hoverPw = '';
    $('#hero').style.cursor = 'grabbing'; $('#hero').title = '';
    drawHero();
    return;
  }
  const h = heroHit(e), key = h ? h.key : '', pw = heroOnPower(h, e) ? key : '';
  $('#hero').style.cursor = h ? 'pointer' : '';
  $('#hero').title = pw ? t(h.k === 'group' ? (h.pw.on ? 'power.group.off' : 'power.group.on') : (h.pw.on ? 'power.dev.off' : 'power.dev.on')) : '';
  if (key !== HERO.hover || pw !== HERO.hoverPw) { HERO.hover = key; HERO.hoverPw = pw; drawHero(); }
});
const heroDragEnd = drop => {
  const d = HERO.drag; HERO.press = null;
  if (!d) return;
  HERO.drag = null; HERO.noClick = true; setTimeout(() => { HERO.noClick = false; });
  $('#hero').style.cursor = '';
  if (drop) heroDrop(d);
  drawHero();
};
$('#hero').addEventListener('pointerup', () => heroDragEnd(true));
$('#hero').addEventListener('pointercancel', () => heroDragEnd(false));
$('#hero').addEventListener('pointerleave', () => { if (HERO.hover && !HERO.drag) { HERO.hover = HERO.hoverPw = ''; drawHero(); } });
$('#hero').addEventListener('click', e => {
  if (HERO.noClick || HERO.arrange) return;
  const h = heroHit(e);
  if (!h) return;
  if (heroOnPower(h, e)) { togglePower(h); drawHero(); }
  else if (h.k === 'group') groupDialog(h.g);
  else openSheet(h, e.clientX, e.clientY);
});

// ---- one power button per device in the preview: memory, the ARGB strip, each Nanoleaf controller, each bulb,
// each LAN device. The same switches as on their own pages ([layout] ..._enabled, [dev.N] enabled).
function devicePower(h) {
  const L = k => cv('layout', k, '1') !== '0';
  if (h.k === 'ram') return { on: L('ram_enabled'), keys: ['ram_enabled'] };
  if (h.k === 'gpu') return { on: L('gpu_enabled'), keys: ['gpu_enabled'] };
  if (h.k === 'nano' && h.slot) return { on: L(nanoKey(h.slot)), keys: [nanoKey(h.slot)] };
  if (h.k === 'bulb') return { on: L('lights_enabled') && L(`light${h.i + 1}_enabled`), keys: [`light${h.i + 1}_enabled`] };
  if (h.k === 'ext') { const d = S.ext.devs[h.i]; return d ? { on: !!d.enabled, dev: d } : null; }
  if (h.k === 'group') {   // on while any of its devices is; the switch turns all of them off, or all on
    const members = HERO.hits.filter(x => x.group === h.g && x.zone), on = members.some(m => (devicePower(m) || {}).on);
    return members.length ? { on, members } : null;
  }
  return null;
}
function togglePower(h) {
  const p = devicePower(h); if (!p) return;
  if (p.members) { p.members.forEach(m => { const q = devicePower(m); if (q && q.on === p.on) togglePower(m); }); return; }
  if (p.dev) {
    p.dev.enabled = p.on ? 0 : 1;
    setCfg('dev.' + p.dev.id, 'enabled', p.dev.enabled);
    updateDevices(); $$(`#dev-grid .dev-on`).forEach((el, i) => { if (S.ext.devs[i]) el.checked = !!S.ext.devs[i].enabled; });
    return;
  }
  // a bulb while all bulbs are off: all bulbs back on, this one included
  if (h.k === 'bulb' && cv('layout', 'lights_enabled', '1') === '0') {
    setCfgLocal('layout', 'lights_enabled', '1'); send({ cmd: 'toggle', k: 'lights_enabled' });
    if (cv('layout', p.keys[0], '1') !== '0') { syncToggles(); return; }
  }
  const k = p.keys[0], v = cv('layout', k, '1') !== '0' ? '0' : '1';
  setCfgLocal('layout', k, v); send({ cmd: 'toggle', k });
  syncToggles();
}
function heroPowerButton(c, h) {
  const p = devicePower(h);
  if (!p) { h.pw = null; return; }
  const d = devicePixelRatio, r = 8 * d, x = h.x + h.w - r - 7 * d, y = h.y + r + 7 * d;
  h.pw = { x, y, r, on: p.on };
  const hot = HERO.hoverPw === h.key, show = hot ? 1 : HERO.hover === h.key ? .85 : p.on ? .28 : .7;
  c.save(); c.globalAlpha = show;
  c.beginPath(); c.arc(x, y, r, 0, 7);
  c.fillStyle = hot ? 'rgba(255,255,255,.14)' : 'rgba(10,10,10,.75)'; c.fill();
  c.strokeStyle = p.on ? 'rgba(255,255,255,.45)' : 'rgba(255,255,255,.18)'; c.lineWidth = d; c.stroke();
  c.strokeStyle = p.on ? '#e8e8e8' : '#6a6a6a'; c.lineWidth = 1.4 * d; c.lineCap = 'round';
  c.beginPath(); c.arc(x, y, r * 0.48, -Math.PI / 2 + 0.75, -Math.PI / 2 - 0.75 + 2 * Math.PI); c.stroke();
  c.beginPath(); c.moveTo(x, y - r * 0.62); c.lineTo(x, y - r * 0.08); c.stroke();
  c.restore();
}

// ---- device sheet: the real settings cards are moved in (placeholders mark their place) and back on close,
// so everything keeps working exactly as on its own page
function sheetCards(h) {
  if (h.k === 'ram') return [[$('#ram-card')], 'pc'];
  if (h.k === 'gpu') return [[$('#gpu-card')], 'pc'];
  if (h.k === 'nano') { if (h.slot) { nanoSlot = h.slot; updateNano(); } return [nanoOn() ? $$('#nano-main > .card') : [$('#nano-empty')], 'nano']; }
  if (h.k === 'bulb') return [[$('#bulb-grid').children[h.i]], 'bulbs'];
  if (h.k === 'ext') { const d = S.ext.devs.find(x => x.id === h.id); return [[$(`#dev-live-${h.id}`)?.closest('.card')], d ? brandTab(d) : 'devices']; }
  if (h.k === 'add') return [[], 'devices'];
  return [[], ''];
}
function openSheet(h, cx, cy) {
  closeSheet();
  const [cards, page] = sheetCards(h);
  if (!cards.length || !cards[0]) { showTab(page || 'effects'); return; }
  const body = $('#sheet-body');
  for (const el of cards) {
    const ph = document.createComment('sheet');
    el.before(ph); body.appendChild(el);
    SHEET.moved.push([el, ph]);
  }
  SHEET.kind = h.k; SHEET.page = page;
  const wrap = $('#sheet'), sh = wrap.querySelector('.sheet');
  sh.style.removeProperty('--dx'); sh.style.removeProperty('--dy');
  sh.classList.toggle('wide', h.k === 'nano' && nanoOn());
  wrap.classList.remove('hidden');
  const r = sh.getBoundingClientRect();   // grow out of the device that was clicked
  sh.style.transformOrigin = `${cx - r.left}px ${cy - r.top}px`;
  sh.classList.remove('in'); void sh.offsetWidth; sh.classList.add('in');
  requestAnimationFrame(sizeCanvases);
}
function closeSheet() {
  if (!SHEET.kind) return;
  for (const [el, ph] of SHEET.moved) { if (ph.isConnected) ph.replaceWith(el); else el.remove(); }
  SHEET.moved = []; SHEET.kind = null;
  $('#sheet').classList.add('hidden');
  requestAnimationFrame(sizeCanvases);
}
$('#sheet-x').addEventListener('click', closeSheet);
$('#sheet').addEventListener('pointerdown', e => { if (e.target.id === 'sheet') closeSheet(); });
$('#sheet-page').addEventListener('click', () => { const p = SHEET.page; closeSheet(); showTab(p); });
document.addEventListener('keydown', e => { if (e.key === 'Escape' && SHEET.kind && pk.classList.contains('hidden')) closeSheet(); });
// drag it around by the top bar, like a small window
$('#sheet-head').addEventListener('pointerdown', e => {
  if (e.target.closest('button') || matchMedia('(max-width: 700px)').matches) return;
  const sh = $('#sheet .sheet'), cs = getComputedStyle(sh);
  const sx = e.clientX - (parseFloat(cs.getPropertyValue('--dx')) || 0), sy = e.clientY - (parseFloat(cs.getPropertyValue('--dy')) || 0);
  const move = m => { sh.style.setProperty('--dx', (m.clientX - sx) + 'px'); sh.style.setProperty('--dy', (m.clientY - sy) + 'px'); };
  const up = () => { removeEventListener('pointermove', move); removeEventListener('pointerup', up); sh.classList.remove('drag'); };
  sh.classList.add('drag');
  addEventListener('pointermove', move); addEventListener('pointerup', up);
});

// ---- customize the preview: which tiles it shows (every device on its own; the memory and the strip as they are),
// and a way to add devices
function heroMenuEntries() {
  const out = [];
  for (const [k] of heroGroups()) {
    if (k === 'nano' && nanoCtls().length > 1) nanoCtls().forEach(n => out.push({ k: 'nano' + n.slot, group: 'nano', name: nanoName(n) }));
    else if (k === 'bulbs' && S.bulbs.length > 1) S.bulbs.forEach((b, i) => out.push({ k: 'bulb' + i, group: 'bulbs', name: t('bulb', i + 1) }));
    else if (k === 'ext') S.ext.devs.forEach(d => out.push({ k: 'dev' + d.id, group: 'ext', name: d.name }));
    else out.push({ k, group: k, name: groupName(k) });
  }
  return out;
}
function buildHeroMenu() {
  const hide = heroHidden(), entries = heroMenuEntries();
  $('#hero-menu').innerHTML = `<div class="lbl"><span>${t('hero.show')}</span></div><div class="hero-checks">` +
    entries.map(e => `<label class="check"><input type="checkbox" data-hg="${e.k}" ${hide.includes(e.k) || hide.includes(e.group) ? '' : 'checked'}><span></span><em>${esc(e.name)}</em></label>`).join('') + '</div>' +
    `<div class="btn-row"><button class="btn small ghost" id="hero-add"><span>+ ${t('hero.add')}</span></button>` +
    `<button class="btn small ghost" id="hero-group"><span>+ ${t('group.add')}</span></button>` +
    `<button class="btn small ghost" id="hero-arrange"><span>${t('hero.arrange')}</span></button></div><p class="hint">${t('hero.hint')}</p>`;
  $$('[data-hg]').forEach(i => i.addEventListener('change', () => {
    const h = new Set(heroHidden()), e = entries.find(x => x.k === i.dataset.hg);
    if (e.group !== e.k && h.has(e.group)) {   // a whole group was hidden (older setting): its tiles one by one now
      h.delete(e.group); entries.filter(x => x.group === e.group).forEach(x => h.add(x.k));
    }
    i.checked ? h.delete(e.k) : h.add(e.k);
    if (entries.every(x => h.has(x.k) || h.has(x.group))) { i.checked = true; return; }   // keep at least one
    setCfg('ui', 'hero_hide', [...h].join(','));
    drawHero();
  }));
  $('#hero-add').addEventListener('click', () => { $('#hero-menu').classList.add('hidden'); showTab('devices'); send({ cmd: 'scan' }); });
  $('#hero-arrange').addEventListener('click', () => { $('#hero-menu').classList.add('hidden'); heroArrange(true); });
  $('#hero-group').addEventListener('click', () => { $('#hero-menu').classList.add('hidden'); const n = groupNew(); if (n) groupDialog(n, true); });
}

// ---- group window: name, which devices are in it (a device is in one group at most) and one look for all of them
// (the zone editor on [group.N]: every change goes to the members, see groupSpread). Changes apply at once;
// Cancel puts the name and the devices back (a new group goes away).
const GDLG = { n: 0, isNew: false, before: null };
function groupDialog(n, isNew) {
  const sec = groupSec(n), g = groups().find(x => x.n === n);
  GDLG.n = n; GDLG.isNew = !!isNew;
  GDLG.before = { name: cv(sec, 'name', ''), members: g ? g.members.slice() : [], homes: {} };
  heroItems(true).forEach(it => { const o = groupOf(it.zone); if (o) GDLG.before.homes[it.zone] = o.n; });
  $('#gdlg-title').textContent = t(isNew ? 'group.new' : 'group.edit');
  $('#gdlg-name').value = cv(sec, 'name', '');
  $('#gdlg-name').placeholder = t('group.n', n);
  const del = $('#gdlg-del'); del.classList.toggle('hidden', !!isNew); del.classList.remove('confirm'); del.querySelector('span').textContent = t('group.delete');
  gdlgDevs();
  const z = $('#gdlg-zone'); z.dataset.zone = sec; renderZone(z);
  $('#gdlg').classList.remove('hidden');
  setTimeout(() => { $('#gdlg-name').focus(); $('#gdlg-name').select(); }, 30);
}
function gdlgDevs() {
  const g = groups().find(x => x.n === GDLG.n), mine = g ? g.members : [];
  $('#gdlg-devs').innerHTML = heroItems(true).map(it => {
    const o = groupOf(it.zone), on = mine.includes(it.zone);
    return `<button type="button" data-z="${it.zone}" class="${on ? 'on' : ''}"><i></i><b>${esc(it.name)}</b>${o && !on ? `<small>${esc(t('group.in', groupTitle(o)))}</small>` : ''}</button>`;
  }).join('') || `<p class="hint">${t('hero.empty')}</p>`;
}
$('#gdlg-devs').addEventListener('click', e => {
  const b = e.target.closest('[data-z]'); if (!b) return;
  if (b.classList.contains('on')) groupRemove(b.dataset.z); else groupAdd(GDLG.n, b.dataset.z);
  gdlgDevs(); drawHero();
});
$('#gdlg-name').addEventListener('input', e => { setCfgSoon(groupSec(GDLG.n), 'name', e.target.value.replace(/[;#\[\]=,]/g, ' ').trim()); drawHero(); });
function closeGroupDialog() { $('#gdlg').classList.add('hidden'); closePicker(); renderZones(); drawHero(); }
$('#gdlg-form').addEventListener('submit', e => { e.preventDefault(); closeGroupDialog(); });
$('#gdlg-cancel').addEventListener('click', () => {
  const B = GDLG.before, sec = groupSec(GDLG.n);
  if (cv(sec, 'name', '') !== B.name) setCfg(sec, 'name', B.name);
  if (GDLG.isNew) setMembers(GDLG.n, []);
  else {   // the devices back where they were
    heroItems(true).forEach(it => { const home = B.homes[it.zone], now = groupOf(it.zone); if ((now ? now.n : 0) !== (home || 0)) { if (home) groupAdd(home, it.zone); else groupRemove(it.zone); } });
  }
  closeGroupDialog();
});
$('#gdlg-del').addEventListener('click', () => {
  const b = $('#gdlg-del');
  if (!b.classList.contains('confirm')) { b.classList.add('confirm'); b.querySelector('span').textContent = t('preset.delete.sure'); return; }
  setMembers(GDLG.n, []); setCfg(groupSec(GDLG.n), 'name', '');
  closeGroupDialog();
});
$('#gdlg').addEventListener('pointerdown', e => { if (e.target.id === 'gdlg') closeGroupDialog(); });
document.addEventListener('keydown', e => { if (e.key === 'Escape' && !$('#gdlg').classList.contains('hidden') && pk.classList.contains('hidden')) $('#gdlg-cancel').click(); });
// arranging the preview: drag a device onto another to swap their places (Done or Esc ends it)
function heroArrange(on) { HERO.arrange = on; HERO.hover = HERO.hoverPw = ''; drawHero(); }
$('#arrange-done').addEventListener('click', () => heroArrange(false));
document.addEventListener('keydown', e => { if (e.key === 'Escape' && HERO.arrange) heroArrange(false); });
$('#hero-edit').addEventListener('click', e => {
  e.stopPropagation();
  const m = $('#hero-menu');
  if (m.classList.contains('hidden')) buildHeroMenu();
  m.classList.toggle('hidden');
});
document.addEventListener('pointerdown', e => { if (!e.target.closest('#hero-menu, #hero-edit')) $('#hero-menu').classList.add('hidden'); });

function drawCanvas(id, fn) {
  const cvs = $(id); if (!cvs || !cvs.width || !cvs.offsetParent) return;
  const c = cvs.getContext('2d');
  c.clearRect(0, 0, cvs.width, cvs.height);
  fn(c, 0, 0, cvs.width, cvs.height);
}

function drawAll() {
  const sh = SHEET.kind;   // the device sheet shows live canvases from other tabs
  if (tab === 'effects') drawHero();
  if (tab === 'pc' || sh === 'ram' || sh === 'gpu') { drawCanvas('#live-ram', drawRam); drawCanvas('#live-gpu', drawGpu); }
  if (tab === 'nano' || sh === 'nano') drawCanvas('#live-nano', (c, x, y, w, h) => drawNano(c, x, y, w, h, curNano() || {}));
  if (tab === 'devices' || tab.startsWith('brand-') || sh === 'ext') S.ext.devs.forEach((d, k) => { if (sh === 'ext' || brandTab(d) === tab) drawCanvas('#dev-live-' + d.id, (c, x, y, w, h) => drawExt(c, x, y, w, h, k)); });
  if (tab === 'bulbs' || sh === 'bulb') S.bulbs.forEach((b, i) => {
    const o = $('#bulb-orb-' + i); if (!o) return;
    const col = F.bulbs[i], on = lit(col) && b.online;
    o.classList.toggle('off', !on);
    if (on) o.style.setProperty('--c', col);
  });
}

function onFrame(l) {
  F = { ram: [[], []], gpu: [], board: null, bulbs: [], nano: {}, ext: {} };
  for (const [d, i, c] of l) {
    const col = '#' + c;
    if (d === 0) F.ram[0][i] = col; else if (d === 1) F.ram[1][i] = col;
    else if (d === 2) F.gpu[i] = col; else if (d === 3) F.board = col;
    else if (d === 4) F.bulbs[i] = col;
    else if (d >= 200) (F.nano[d - 200] = F.nano[d - 200] || [])[i] = col;
    else if (d >= 100) (F.ext[d - 100] = F.ext[d - 100] || [])[i] = col;
  }
  drawAll();
  ambient();
}

// Background light: one colour per device group, brightness-weighted, with a little extra saturation so a
// rainbow does not average to grey. Groups without devices borrow the overall colour, a bit weaker.
const AMB = { t: 0, last: [] };
function groupColor(list) {
  let r = 0, g = 0, b = 0, w = 0;
  for (const c of list) {
    if (!c) continue;
    const n = parseInt(c.slice(1), 16), R = n >> 16, G = (n >> 8) & 255, B = n & 255, l = Math.max(R, G, B);
    if (l < 10) continue;
    r += R * l; g += G * l; b += B * l; w += l;
  }
  if (!w) return null;
  r /= w; g /= w; b /= w;
  const m = Math.max(r, g, b), avg = (r + g + b) / 3;
  const sat = x => Math.max(0, Math.min(255, (avg + (x - avg) * 1.6) * 255 / m));
  return [sat(r), sat(g), sat(b), Math.min(1, m / 255 * 1.3)];
}
function ambient() {
  const now = performance.now();
  if (now - AMB.t < 400) return;
  AMB.t = now;
  const groups = [[...F.ram[0], ...F.ram[1]], [...F.gpu, F.board], Object.values(F.nano).flat(), [...F.bulbs, ...Object.values(F.ext).flat()]];
  const all = groupColor(groups.flat());
  const el = $('#ambient');
  groups.forEach((g, i) => {
    let c = groupColor(g), a = 0;
    if (c) a = .08 + .16 * c[3];
    else if (all) { c = all; a = .05 + .07 * all[3]; }
    const v = c ? `rgba(${c[0] | 0}, ${c[1] | 0}, ${c[2] | 0}, ${a.toFixed(2)})` : 'rgba(0, 0, 0, 0)';
    if (AMB.last[i] !== v) { AMB.last[i] = v; el.style.setProperty('--amb' + i, v); }
  });
}

// ------------------------------------------------------------------ PC / Nanoleaf / bulbs / settings
function syncToggles() {
  $$('[data-toggle]').forEach(t => { t.checked = cv('layout', t.dataset.toggle, '1') !== '0'; });
  $$('[data-own]').forEach(t => { t.checked = cv('devices', t.dataset.own, '0') === '1'; });
  $$('[data-layout]').forEach(t => { t.checked = cv('layout', t.dataset.layout, t.dataset.layout === 'board_led' ? '1' : '0') !== '0'; });
  $('#gpu-leds').textContent = cv('layout', 'gpu_leds', 8);
  $$('[data-cal]').forEach(t => { t.checked = cv('calibration', t.dataset.cal, '1') !== '0'; });
  $$('[data-wb]').forEach(r => { const v = +cv('calibration', r.dataset.wb, 0); setRange(r, v); showWb(r.dataset.wb, v); });
}
// white balance / gamma of the PWM LEDs (RAM, water block)
const showWb = (k, v) => $$(`[data-wb-val="${k}"]`).forEach(b => b.textContent = (v > 0 ? '+' : '') + v);
$$('[data-wb]').forEach(r => r.addEventListener('input', () => { showWb(r.dataset.wb, +r.value); setCfgSoon('calibration', r.dataset.wb, r.value); }));
$$('[data-cal]').forEach(t => t.addEventListener('change', () => {
  setCfg('calibration', t.dataset.cal, t.checked ? 1 : 0);
  $$(`[data-cal="${t.dataset.cal}"]`).forEach(o => o.checked = t.checked);
}));
$$('[data-toggle]').forEach(t => t.addEventListener('change', () => {
  setCfgLocal('layout', t.dataset.toggle, t.checked ? '1' : '0');
  send({ cmd: 'toggle', k: t.dataset.toggle });
}));
function setCfgLocal(s, k, v) { (S.cfg[s] = S.cfg[s] || {})[k] = v; }
$$('[data-layout]').forEach(t => t.addEventListener('change', () => setCfg('layout', t.dataset.layout, t.checked ? 1 : 0)));
// the board / the memory given back to their own lighting ([devices] msi_own / ene_own)
$$('[data-own]').forEach(t => t.addEventListener('change', () => { setCfg('devices', t.dataset.own, t.checked ? 1 : 0); S[t.dataset.own === 'msi_own' ? 'msi_own' : 'ram_own'] = t.checked ? 1 : 0; updateChips(); drawHero(); }));
$$('[data-step]').forEach(b => b.addEventListener('click', () => {
  const n = Math.max(1, Math.min(40, +cv('layout', 'gpu_leds', 8) + +b.dataset.step));
  setCfg('layout', 'gpu_leds', n); $('#gpu-leds').textContent = n;
}));

$('#strip-name').addEventListener('input', e => {
  setCfgSoon('layout', 'strip_name', e.target.value.replace(/[;#\[\]=]/g, '').trim());
  $('#strip-title').textContent = stripName(); renderOwnList();
});

// nanoleaf
$$('[data-pair]').forEach(b => b.addEventListener('click', () => { S.nano.pair = 1; send({ cmd: 'pair' }); updateNano(); }));
$('#nano-rot-l').addEventListener('click', () => rotateNano(-90));
$('#nano-rot-r').addEventListener('click', () => rotateNano(90));
$('#nano-flip').addEventListener('click', () => { const s = nanoSec(curNano().slot); setCfg(s, 'flip', cv(s, 'flip', '0') === '1' ? 0 : 1); });
function rotateNano(d) { const s = nanoSec(curNano().slot); setCfg(s, 'rotate', ((+cv(s, 'rotate', 0) + d) % 360 + 360) % 360); }
$('#nano-forget').addEventListener('click', () => {
  const n = curNano(); if (!n || !confirm(t('nano.forget.ask', nanoName(n)))) return;
  if (SHEET.kind === 'nano') closeSheet();
  send({ cmd: 'nano_forget', slot: String(n.slot) });
});
$('#nano-pick').addEventListener('click', e => {
  const b = e.target.closest('button[data-slot]'); if (!b) return;
  nanoSlot = +b.dataset.slot; updateNano(); drawAll();
});
$('#nano-rate').addEventListener('input', e => { $('#nano-rate-val').textContent = e.target.value + t('per.s'); setCfgSoon('nanoleaf', 'rate', e.target.value); });
function updateNano() {
  const L = nanoCtls(), n = curNano() || {}, on = L.length > 0;
  $('#nano-empty').classList.toggle('hidden', on);
  $('#nano-main').classList.toggle('hidden', !on);
  if (n.slot) nanoSlot = n.slot;
  const pick = $('#nano-pick'), sig = JSON.stringify(L.map(c => [c.slot, c.name, c.online])) + nanoSlot;
  if (pick.dataset.sig !== sig) {   // one button per controller, only when there are several
    pick.dataset.sig = sig;
    pick.innerHTML = L.length > 1 ? L.map(c => `<button data-slot="${c.slot}" class="${c.slot === nanoSlot ? 'on' : ''}"><span class="dot ${c.online ? 'on' : 'off'}"></span>${esc(nanoName(c))}</button>`).join('') : '';
  }
  $('#nano-name').textContent = nanoName(n);
  const model = n.model ? ' · ' + n.model : '';
  $('#nano-status').innerHTML = n.online ? `<span class="dot on" style="display:inline-block;margin-right:6px"></span>${t('nano.online', n.ip, (n.panels || []).length)}${model}${n.stream ? ' · ' + t('nano.streaming') : ''}`
    : `<span class="dot off" style="display:inline-block;margin-right:6px"></span>${t('nano.offline')}${n.ip ? ' · ' + n.ip : ''}`;
  if (n.slot) {
    const tg = $('#nano-main [data-toggle]');
    if (tg.dataset.toggle !== nanoKey(n.slot)) tg.dataset.toggle = nanoKey(n.slot);
    tg.checked = cv('layout', nanoKey(n.slot), '1') !== '0';
    const z = $('#nano-zone');
    if (z.dataset.zone !== nanoZone(n.slot)) { z.dataset.zone = nanoZone(n.slot); renderZone(z); }
  }
  const P = S.nano || {};
  $$('[data-pair-msg]').forEach(p => p.textContent = P.pair ? t('pair.' + P.pair) : '');
  $$('[data-pair]').forEach(b => b.disabled = P.pair === 1 || P.pair === 2);
  const r = +cv('nanoleaf', 'rate', 10); setRange($('#nano-rate'), r); $('#nano-rate-val').textContent = r + t('per.s');
  const dev = cv('nanoleaf', 'mode', 'device') !== 'stream';
  $('#nano-device').checked = dev;
  $('#nano-rate-note').textContent = t(dev ? 'nano.rate.live' : 'nano.rate.stream');
}
$('#nano-device').addEventListener('change', e => { setCfg('nanoleaf', 'mode', e.target.checked ? 'device' : 'stream'); updateNano(); });

// bulbs
function buildBulbs() {
  if (SHEET.kind === 'bulb') closeSheet();
  const grid = $('#bulb-grid');
  grid.innerHTML = '';
  $('#bulbs-empty').classList.toggle('hidden', S.bulbs.length > 0);
  S.bulbs.forEach((b, i) => {
    const d = document.createElement('div');
    d.className = 'card';
    const short = (b.name.split(' ').pop() || b.name);
    d.innerHTML = `<div class="bulb-top"><div class="bulb-orb off" id="bulb-orb-${i}"></div>
      <div><h3>${t('bulb', i + 1)}</h3><p class="muted" id="bulb-st-${i}"></p></div>
      <label class="switch"><input type="checkbox" data-toggle="light${i + 1}_enabled"><span></span></label></div>
      <div class="zone" data-zone="zone.light${i + 1}"></div>
      <div class="opts"><label class="num"><span>${t('fix.type')}</span><select class="select bulb-type">${typeOptions(bulbType(i), null, ['bulb', 'lamp', 'floor', 'strip'])}</select></label></div>`;
    d.title = b.name;
    grid.appendChild(d);
    const sw = d.querySelector('[data-toggle]');
    sw.checked = cv('layout', sw.dataset.toggle, '1') !== '0';
    sw.addEventListener('change', () => { setCfgLocal('layout', sw.dataset.toggle, sw.checked ? '1' : '0'); send({ cmd: 'toggle', k: sw.dataset.toggle }); });
    d.querySelector('.bulb-type').addEventListener('change', e => { setCfg('zone.light' + (i + 1), 'type', e.target.value); drawAll(); });
    renderZone(d.querySelector('.zone'));
    d.querySelector('.muted').dataset.short = short;
  });
  updateBulbs();
}
function updateBulbs() {
  const on = S.bulbs.filter(b => b.online).length;
  $('#bulbs-status').textContent = S.bulbs.length ? t('bulbs.status', on, S.bulbs.length) : t('bulbs.notset');
  S.bulbs.forEach((b, i) => {
    const el = $('#bulb-st-' + i); if (!el) return;
    el.innerHTML = `<span class="dot ${b.online ? 'on' : 'off'}" style="display:inline-block;margin-right:6px"></span>${el.dataset.short || ''} · ${b.online ? b.ip : t('bulb.offline')}`;
  });
  const sm = +cv('lights', 'smooth', 0.35); setRange($('#bulb-smooth'), Math.round(sm * 100)); $('#smooth-val').textContent = sm.toFixed(2) + t('sec');
  const rt = +cv('lights', 'rate', 4); setRange($('#bulb-rate'), rt); $('#rate-val').textContent = rt;
}
$('#bulb-smooth').addEventListener('input', e => { const v = (e.target.value / 100).toFixed(2); $('#smooth-val').textContent = v + t('sec'); setCfgSoon('lights', 'smooth', v); });
$('#bulb-rate').addEventListener('input', e => { $('#rate-val').textContent = e.target.value; setCfgSoon('lights', 'rate', e.target.value); });

// Govee cloud mode as it works out: "auto" is screen sync for devices that have it (a sync box), colours otherwise
const gcSync = (d, sec) => { const m = cv(sec, 'mode', 'auto'); return m === 'sync' || (m === 'auto' && /screen sync/.test(d.info || '')); };
// "Type" choices: auto (the core's guess, named) or one of FIXTURES; bulbs offer only the ones a bulb can be in
function typeOptions(cur, guess, only) {
  const list = only || FIXTURES;
  return (only ? '' : `<option value="auto"${FIXTURES.includes(cur) ? '' : ' selected'}>${t('fix.auto')}${guess ? ' · ' + t('fix.' + guess) : ''}</option>`) +
    list.map(v => `<option value="${v}"${v === cur ? ' selected' : ''}>${t('fix.' + v)}</option>`).join('');
}

// LAN / bridge devices
let devSig = '';
const kindTitle = k => (S.ext.kinds.find(x => x.kind === k) || { title: k }).title;
// the PC's own devices (not the lights on the network): they can be given back to their own lighting
const pcDev = d => ['openrgb', 'wooting', 'nlusb', 'razer', 'steelseries', 'logitech'].includes(d.kind);
// the resizable zones of an OpenRGB device (a motherboard's ARGB headers): [{name, count, min, max}]
const devZones = d => (d.zones || '').split('|').filter(Boolean).map(z => { const [name, count, min, max] = z.split(':'); return { name, count: +count, min: +min, max: +max }; });
function devStatus(d) {
  if (!d.enabled) return t('dev.off');
  if (d.own) return t('own.status');
  if (!d.online && devZones(d).length && devZones(d).every(z => !z.count)) return t('zones.need');
  if (d.pro && proLocked()) return t('dev.pro');
  if (!d.online) return d.info && /button|reach|forgot|colour|token/i.test(d.info) ? d.info : t('dev.offline');
  return t(d.per_led ? 'dev.online' : 'dev.online.lights', d.leds);
}
function buildDevices() {
  if (SHEET.kind === 'ext') closeSheet();
  buildBrandTabs();
  const grid = $('#dev-grid');
  grid.innerHTML = '';
  $$('[data-brand-grid]').forEach(g => { g.innerHTML = ''; });
  $('#dev-empty').classList.toggle('hidden', S.ext.devs.length > 0);
  S.ext.devs.forEach(d => {
    const el = document.createElement('div');
    el.className = 'card dev-card';
    const sec = 'dev.' + d.id;
    el.innerHTML = `<div class="card-head">
        <div><h3>${esc(d.name)}</h3><p class="muted" id="dev-st-${d.id}"></p><p class="dev-info">${esc(kindTitle(d.kind))} · ${esc(d.host)}${d.sub >= 0 && d.kind === 'openrgb' ? ' · #' + d.sub : ''}</p></div>
        <label class="switch"><input type="checkbox" class="dev-on" ${d.enabled ? 'checked' : ''}><span></span></label>
      </div>
      <canvas class="live" id="dev-live-${d.id}" data-h="${d.per_led ? 70 : 90}"></canvas>
      <div class="zone" data-zone="zone.dev${d.id}"></div>
      <div class="opts">
        <label class="num"><span>${t('strip.name')}</span><input type="text" class="dev-name" maxlength="40" spellcheck="false" value="${esc(cv(sec, 'name', d.name))}"></label>
        ${d.per_led ? `${devZones(d).length ? '' : `<div class="stepper"><span>${t('leds')}</span><button data-d="-1">−</button><b class="dev-leds">${d.leds}</b><button data-d="1">+</button></div>`}
        <label class="check"><input type="checkbox" class="dev-rev" ${cv(sec, 'reverse', '0') === '1' ? 'checked' : ''}><span></span><em>${t('reverse')}</em></label>` : ''}
        ${d.kind === 'goveecloud' ? `<label class="num"><span>${t('gc.mode')}</span><select class="select dev-mode">${['auto', 'sync', 'colour'].map(v =>
          `<option value="${v}"${cv(sec, 'mode', 'auto') === v ? ' selected' : ''}>${t('gc.' + v)}</option>`).join('')}</select></label>
          <p class="hint dev-mode-note">${t(gcSync(d, sec) ? 'gc.note.sync' : 'gc.note.colour')}</p>` : ''}
        ${d.kind === 'divoom' ? `<label class="num"><span>LocalToken</span><input type="text" class="dev-token" inputmode="numeric" maxlength="16" spellcheck="false" value="${esc(cv(sec, 'key', ''))}"></label>
          <p class="hint">${t('dv.token')}</p>
          ${d.type === 'frame' ? `<label class="num"><span>${t('dv.fx')}</span><select class="select dev-fx">${Array.from({ length: 16 }, (_, v) =>
            `<option value="${v}"${+cv(sec, 'frame_fx', 0) === v ? ' selected' : ''}>${t('dv.fx.n', v)}</option>`).join('')}</select></label>
          <p class="hint">${t('dv.fx.note')}</p>
          <label class="num"><span>${t('dv.screen')}</span><select class="select dev-screen">${['own', 'dial', 'monitor'].map(v =>
            `<option value="${v}"${cv(sec, 'screen', 'own') === v ? ' selected' : ''}>${t('dv.s.' + v)}</option>`).join('')}</select></label>
          <div class="dev-dial-box${cv(sec, 'screen', 'own') === 'dial' ? '' : ' hidden'}">${frameDials(sec)}</div>
          <label class="check"><input type="checkbox" class="dev-follow" ${cv(sec, 'screen_follow', '1') !== '0' ? 'checked' : ''}><span></span><em>${t('dv.follow')}</em></label>
          <p class="hint">${t('dv.screen.note')}</p>` : `<label class="num"><span>${t('dv.lights')}</span><select class="select dev-lights">${['both', 'back', 'sides', 'back_cycle', 'back_rainbow'].map(v =>
            `<option value="${v}"${cv(sec, 'lights', 'both') === v ? ' selected' : ''}>${t('dv.l.' + v)}</option>`).join('')}</select></label>
          <p class="hint">${t('dv.lights.note')}</p>
          <label class="check"><input type="checkbox" class="dev-follow" ${cv(sec, 'screen_follow', '1') !== '0' ? 'checked' : ''}><span></span><em>${t('dv.follow')}</em></label>`}` : ''}
        <label class="num"><span>${t('fix.type')}</span><select class="select dev-type">${typeOptions(cv(sec, 'type', 'auto'), d.type)}</select></label>
        ${devZones(d).length ? `<div class="field dev-zones"><div class="lbl"><span>${t('zones.title')}</span></div>
          ${devZones(d).map(z => `<label class="num"><span>${esc(z.name)}</span><input type="number" class="dev-zone" data-zone="${esc(z.name)}" min="${z.min}" max="${Math.min(z.max, 512)}" value="${z.count}"></label>`).join('')}
          <p class="hint">${t('zones.note')}</p></div>` : ''}
        ${d.kind === 'openrgb' ? `<label class="num"><span>${t('dev.rate')}</span><select class="select dev-rate">${[0, 30, 10, 5, 2].map(v =>
          `<option value="${v}"${+cv(sec, 'rate', 0) === v ? ' selected' : ''}>${v ? t('dev.rate.n', v) : t('dev.rate.auto')}</option>`).join('')}</select></label>
          <p class="hint">${t('dev.rate.note')}</p>` : ''}
        ${!pcDev(d) ? '' : `<label class="check"><input type="checkbox" class="dev-own" ${d.own ? 'checked' : ''}><span></span><em>${t('own')}</em></label>
          <p class="hint">${t('own.note.dev')}</p>`}
        <button class="btn danger small dev-del">${t('dev.remove')}</button>
      </div>`;
    ($(`[data-brand-grid="${(brandOf(d.kind) || [])[0]}"]`) || grid).appendChild(el);
    renderZone(el.querySelector('.zone'));
    el.querySelector('.dev-on').addEventListener('change', e => { setCfg(sec, 'enabled', e.target.checked ? 1 : 0); });
    el.querySelector('.dev-own')?.addEventListener('change', e => { d.own = e.target.checked ? 1 : 0; setCfg(sec, 'own', d.own); updateDevices(); drawHero(); });
    el.querySelector('.dev-name').addEventListener('change', e => {
      const v = e.target.value.replace(/[;#\[\]=]/g, '').trim(); if (!v) return;
      setCfg(sec, 'name', v); el.querySelector('h3').textContent = v; d.name = v; renderOwnList();
    });
    el.querySelector('.dev-rev')?.addEventListener('change', e => setCfg(sec, 'reverse', e.target.checked ? 1 : 0));
    el.querySelector('.dev-token')?.addEventListener('change', e => { const v = e.target.value.replace(/\D/g, ''); e.target.value = v; setCfg(sec, 'key', v); });
    el.querySelector('.dev-type').addEventListener('change', e => setCfg(sec, 'type', e.target.value));
    el.querySelector('.dev-lights')?.addEventListener('change', e => setCfg(sec, 'lights', e.target.value));
    el.querySelector('.dev-rate')?.addEventListener('change', e => setCfg(sec, 'rate', e.target.value));
    // LEDs on each connector: all of them saved together ([dev.N] zones), the device reconnects with them
    el.querySelectorAll('.dev-zone').forEach(inp => inp.addEventListener('change', () => {
      const v = [...el.querySelectorAll('.dev-zone')].map(x => {
        const n = Math.max(+x.min, Math.min(+x.max, Math.round(+x.value) || 0)); x.value = n;
        return `${x.dataset.zone}=${n}`;
      }).join('|');
      if (+cv(sec, 'leds', 0)) setCfg(sec, 'leds', 0);   // the connectors decide its LED count now, not a cap set earlier
      setCfg(sec, 'zones', v);
    }));
    el.querySelector('.dev-fx')?.addEventListener('change', e => setCfg(sec, 'frame_fx', e.target.value));
    el.querySelector('.dev-screen')?.addEventListener('change', e => {
      setCfg(sec, 'screen', e.target.value);
      el.querySelector('.dev-dial-box').classList.toggle('hidden', e.target.value !== 'dial');
    });
    el.querySelector('.dev-dial')?.addEventListener('change', e => { const v = String(e.target.value).replace(/\D/g, ''); if (v) setCfg(sec, 'clock', v); });
    el.querySelector('.dev-follow')?.addEventListener('change', e => setCfg(sec, 'screen_follow', e.target.checked ? 1 : 0));
    el.querySelector('.dev-mode')?.addEventListener('change', e => {
      setCfg(sec, 'mode', e.target.value);
      el.querySelector('.dev-mode-note').textContent = t(gcSync(d, sec) ? 'gc.note.sync' : 'gc.note.colour');
    });
    el.querySelectorAll('[data-d]').forEach(b => b.addEventListener('click', () => {
      const n = Math.max(1, Math.min(512, +(cv(sec, 'leds', 0) > 0 ? cv(sec, 'leds') : d.leds) + +b.dataset.d));
      setCfg(sec, 'leds', n); el.querySelector('.dev-leds').textContent = n;
    }));
    const del = el.querySelector('.dev-del');
    del.addEventListener('click', () => {
      if (!del.classList.contains('confirm')) {
        del.classList.add('confirm'); del.textContent = t('dev.remove.sure');
        setTimeout(() => { del.classList.remove('confirm'); del.textContent = t('dev.remove'); }, 3000);
        return;
      }
      send({ cmd: 'dev_remove', id: d.id });
    });
  });
  updateDevices();
  requestAnimationFrame(sizeCanvases);
}
// ---- other PC hardware: what OpenRGB on this PC offers (S.orgb: state 0 not looked, 1 looking, 2 answers, 3 none).
// Hardware haku drives itself (the MSI board, ENE memory) is shown but not offered, so two programs never fight.
const ORGB_TYPES = { 0: 'board', 1: 'ram', 2: 'gpu', 3: 'fan', 4: 'strip', 5: 'keyboard', 6: 'mouse', 18: 'keyboard' };
// hardware haku drives itself: its memory and MSI board, and devices added directly (their brand in the name)
const orgbOwn = c => (c.type === 1 && S.sticks > 0) || (c.type === 0 && S.msi && /msi|mystic/i.test(c.name)) ||
  S.ext.devs.some(d => d.kind !== 'openrgb' && (d.title || '').split(' ')[0].length >= 3 && c.name.toLowerCase().includes(d.title.split(' ')[0].toLowerCase())) ||
  (nanoOn() && /nanoleaf/i.test(c.name));
const orgbDev = c => S.ext.devs.find(d => d.kind === 'openrgb' && /^(127\.0\.0\.1|localhost)(:\d+)?$/.test(d.host) &&
  (cv('dev.' + d.id, 'match', '') ? cv('dev.' + d.id, 'match', '') === c.name : d.sub === c.i));
const orgbAdded = c => !!orgbDev(c);
const orgbAdd = c => send({ cmd: 'dev_add', kind: 'openrgb', host: '127.0.0.1', sub: c.i, name: c.name, leds: 0 });
function updateOrgb() {
  const O = S.orgb || { state: 0, ctls: [] }, ctls = O.ctls || [];
  $('#orgb-status').textContent = t('orgb.st' + O.state, ctls.length) + (O.state === 2 && O.ours ? ' · ' + t('orgb.ours') : '');
  $('#orgb-check').disabled = O.state === 1;
  // not there or not running: the setup (download, or point to it), with its progress
  const su = O.setup || 0, busy = su >= 1 && su <= 3;
  $('#orgb-guide').classList.toggle('hidden', !(O.state === 3 || O.state === 4 || busy || su === 5));
  $('#orgb-guide-text').textContent = busy || su === 4 ? t('orgb.su' + su, O.pct || 0) : su === 5 ? t('orgb.err.' + O.err) : O.state === 4 ? t('orgb.none') : t('orgb.off');
  $('#orgb-guide-text').classList.toggle('bad', su === 5);
  const bar = $('#orgb-bar');
  bar.classList.toggle('hidden', !busy); bar.classList.toggle('busy', busy && su !== 1);
  bar.firstElementChild.style.width = su === 1 ? (O.pct || 0) + '%' : '';
  $('#orgb-setup').classList.toggle('hidden', !!O.exe && su !== 5);
  $('#orgb-setup').disabled = busy;
  $('#orgb-locate').classList.toggle('hidden', !!O.exe || busy);
  $('#orgb-auto').checked = O.auto !== 0;
  $('#orgb-note').classList.toggle('hidden', !ctls.length);
  const free = ctls.filter(c => !orgbOwn(c) && !orgbAdded(c));
  $('#orgb-all-row').classList.toggle('hidden', free.length < 2);
  const html = ctls.map(c => {
    const own = orgbOwn(c), added = orgbAdded(c);
    return `<div class="found-row"><span class="kind">${t('fix.' + (ORGB_TYPES[c.type] || 'strip'))}</span>
      <div class="what"><b>${esc(c.name)}</b><small>${t('dev.leds', c.leds)}${own ? ' · ' + t('orgb.own') : ''}</small></div>
      ${added ? `<span class="added">${t('dev.added')}</span>` : own ? '' : `<button class="btn small" data-orgb="${c.i}">${t('dev.add')}</button>`}</div>`;
  }).join('');
  const list = $('#orgb-list');
  if (list.dataset.html !== html) {
    list.dataset.html = html; list.innerHTML = html;
    list.querySelectorAll('[data-orgb]').forEach(b => b.addEventListener('click', () => {
      const c = ctls.find(x => x.i === +b.dataset.orgb); if (!c) return;
      b.disabled = true; orgbAdd(c);
    }));
  }
}
// ---- lighting in this PC: what haku found (hw, from hwinfo.c) and which way each is lit: by haku itself, through
// OpenRGB (found there, added or not), OpenRGB still to set up, PawnIO missing, or nothing known
const HW_KEYS = {   // USB maker -> words its OpenRGB controllers carry in their names
  '048D': ['gigabyte', 'rgb fusion', 'aorus'], '0B05': ['asus', 'aura', 'rog'], '1462': ['msi', 'mystic'], '26CE': ['asrock', 'polychrome'],
  '1B1C': ['corsair'], '1E71': ['nzxt'], '1532': ['razer'], '046D': ['logitech'], '1038': ['steelseries'], '0CF2': ['lian li', 'strimer'],
  '2516': ['cooler master'], '3633': ['deepcool'], '0951': ['hyperx', 'kingston'], '03F0': ['hyperx'], '1044': ['gigabyte', 'aorus'],
  '2F68': ['thermaltake'], '264A': ['thermaltake'], '0416': ['lian li', 'thermalright'], '3402': ['glorious'], '2433': ['asetek'],
};
function hwRoute(it) {
  const O = S.orgb || { state: 0, ctls: [] }, ctls = O.ctls || [], vid = (it.id || '').slice(0, 4);
  const lc = x => (x || '').toLowerCase();
  // haku's own drivers first (or given back to their own lighting)
  if ((it.cat === 'board' || (it.cat === 'usb' && vid === '1462')) && S.msi) return S.msi_own ? { cls: 'off', text: t('hw.own') } : { cls: 'on', text: t('hw.haku') };
  if (it.cat === 'ram' && S.sticks > 0) return S.ram_own ? { cls: 'off', text: t('hw.own') } : { cls: 'on', text: t('hw.haku') };
  const own = { '31E3': 'wooting', '1B80': 'wooting', '37FA': 'nlusb' }[vid];
  if (own) {
    const ds = S.ext.devs.filter(d => d.kind === own && lc(d.host).startsWith(lc(vid)));
    return !ds.length ? { cls: 'warn', text: t('hw.haku.add'), act: 'scan' } : ds.every(d => d.own) ? { cls: 'off', text: t('hw.own') } : { cls: 'on', text: t('hw.haku') };
  }
  // through the maker's own app (Synapse, GG, G HUB), when one of its devices is added
  const bridge = { '1532': ['razer', 'Razer Synapse'], '1038': ['steelseries', 'SteelSeries GG'], '046D': ['logitech', 'Logitech G HUB'] }[vid];
  const bds = bridge ? S.ext.devs.filter(d => d.kind === bridge[0]) : [];
  if (bds.length) return bds.every(d => d.own) ? { cls: 'off', text: t('hw.own') } : { cls: 'on', text: t('hw.bridge', bridge[1]) };
  // then what OpenRGB found for it
  let match = [];
  if (it.cat === 'board') match = ctls.filter(c => c.type === 0);
  else if (it.cat === 'ram') match = ctls.filter(c => c.type === 1);
  else if (it.cat === 'gpu') {
    const words = [lc(it.maker), ...lc(it.name).split(/\s+/).filter(w => /\d/.test(w) && w.length >= 3)].filter(Boolean);
    match = ctls.filter(c => c.type === 2 && words.some(w => lc(c.name).includes(w)));
  } else match = ctls.filter(c => c.type > 2 && (HW_KEYS[vid] || []).some(k => lc(c.name).includes(k)));
  const mine = match.filter(c => !orgbOwn(c));
  if (mine.length) {
    const free = mine.filter(c => !orgbAdded(c));
    if (free.length) return { cls: 'warn', text: t('hw.orgb.found'), act: 'add', ctls: free };
    return mine.every(c => (orgbDev(c) || {}).own) ? { cls: 'off', text: t('hw.own') } : { cls: 'on', text: t('hw.orgb') };
  }
  if (bridge && S.ext.found.some(f => f.kind === bridge[0])) return { cls: 'warn', text: t('hw.bridge.add', bridge[1]), act: 'scan' };
  if (it.cat === 'ram' && !S.pawnio) return { cls: 'warn', text: t('hw.pawnio'), act: 'pawnio' };
  if (it.cat === 'gpu' && /^(nvidia|amd|intel)$/i.test(it.maker || '')) return { cls: 'off', text: t('hw.none') };   // reference cards
  if (O.state === 4) return { cls: 'warn', text: t('hw.orgb.need'), act: 'setup' };
  if (O.state === 3) return { cls: 'warn', text: t('hw.orgb.off'), act: 'check' };
  if (O.state < 2) return { cls: 'off', text: t('hw.looking') };
  return { cls: 'off', text: t('hw.orgb.nothing') };
}
function updateHw() {
  const H = S.hw || { busy: 0, done: 0, items: [] }, items = H.items || [];
  const routes = items.map(hwRoute);
  const lit = routes.filter(r => r.cls === 'on').length;
  $('#hw-status').textContent = H.busy || !H.done ? t('hw.busy') : t('hw.sum', items.length, lit);
  $('#hw-scan').disabled = !!H.busy;
  const html = items.map((it, i) => {
    const r = routes[i];
    const btn = r.act === 'add' ? t('dev.add') : r.act === 'setup' ? t('orgb.setup') : r.act === 'pawnio' ? 'pawnio.eu' : r.act === 'scan' ? t('hw.find') : r.act === 'check' ? t('orgb.check') : '';
    return `<div class="found-row"><span class="kind">${t('hw.cat.' + it.cat)}</span>
      <div class="what"><b>${esc([it.maker, it.name].filter(Boolean).join(' · ') || it.id)}</b><small><span class="dot ${r.cls}"></span> ${esc(r.text)}${it.id ? ' · ' + esc(it.id) : ''}</small></div>
      ${btn ? `<button class="btn small${r.act === 'add' || r.act === 'setup' ? '' : ' ghost'}" data-hw="${i}">${esc(btn)}</button>` : ''}</div>`;
  }).join('');
  const list = $('#hw-list');
  if (list.dataset.html === html) return;
  list.dataset.html = html; list.innerHTML = html;
  list.querySelectorAll('[data-hw]').forEach(b => b.addEventListener('click', () => {
    const r = routes[+b.dataset.hw];
    if (r.act === 'add') { b.disabled = true; r.ctls.forEach(orgbAdd); }
    else if (r.act === 'setup') { $('#orgb-setup').click(); $('#orgb-guide').scrollIntoView({ behavior: 'smooth', block: 'center' }); }
    else if (r.act === 'check') $('#orgb-check').click();
    else if (r.act === 'pawnio') send({ cmd: 'open', what: 'pawnio' });
    else if (r.act === 'scan') { send({ cmd: 'scan' }); showTab('devices'); }
  }));
}
$('#pc-own').addEventListener('click', () => {
  const v = S.pc_own ? 0 : 1;
  S.pc_own = S.msi_own = S.ram_own = v;
  S.ext.devs.forEach(d => { if (pcDev(d)) { d.own = v; setCfgLocal('dev.' + d.id, 'own', String(v)); } });
  setCfgLocal('devices', 'msi_own', String(v)); setCfgLocal('devices', 'ene_own', String(v));
  send({ cmd: 'pc_own', v: String(v) });
  updateChips(); updateDevices(); drawHero();
});
$('#hw-scan').addEventListener('click', () => { S.hw = Object.assign({}, S.hw, { busy: 1 }); updateHw(); send({ cmd: 'hw_scan' }); });

$('#orgb-check').addEventListener('click', () => { S.orgb = Object.assign({}, S.orgb, { state: 1 }); updateOrgb(); send({ cmd: 'orgb_check' }); });
$('#orgb-setup').addEventListener('click', () => { S.orgb = Object.assign({}, S.orgb, { setup: 1, pct: 0 }); updateOrgb(); send({ cmd: 'orgb_setup' }); });
$('#orgb-locate').addEventListener('click', () => send({ cmd: 'orgb_locate' }));
$('#orgb-auto').addEventListener('change', e => send({ cmd: 'orgb_auto', v: e.target.checked ? '1' : '0' }));
$('#orgb-all').addEventListener('click', () => { (S.orgb.ctls || []).filter(c => !orgbOwn(c) && !orgbAdded(c)).forEach(orgbAdd); $('#orgb-all').disabled = true; setTimeout(() => { $('#orgb-all').disabled = false; }, 3000); });

function updateDevices() {
  const E = S.ext;
  E.devs.forEach(d => {
    const el = $('#dev-st-' + d.id); if (!el) return;
    el.innerHTML = `<span class="dot ${d.enabled && d.online ? 'on' : 'off'}" style="display:inline-block;margin-right:6px"></span>${esc(devStatus(d))}`;
    const own = el.closest('.card')?.querySelector('.dev-own'); if (own && own !== document.activeElement) own.checked = !!d.own;
  });
  const btn = $('#scan-btn');
  btn.disabled = !!E.scanning;
  btn.querySelector('span').textContent = E.scanning ? t('dev.scanning') : t('dev.scan');
  $('#scan-status').textContent = E.scanning ? t('dev.scanning.note') : E.found.length ? t('dev.found', E.found.length) : t('dev.scan.note');
  const list = $('#found-list');
  const html = E.found.map((f, i) => `<div class="found-row"><span class="kind">${esc(f.title)}</span>
      <div class="what"><b>${esc(f.name || f.title)}</b><small>${esc(f.host)}${f.sub >= 0 && f.kind === 'openrgb' ? ' · #' + f.sub : ''}${f.leds ? ' · ' + t((S.ext.kinds.find(k => k.kind === f.kind) || {}).per_led ? 'dev.leds' : 'dev.lights', f.leds) : ''}${f.info ? ' · ' + esc(f.info) : ''}</small></div>
      ${f.added ? `<span class="added">${t('dev.added')}</span>` : `<button class="btn small" data-found="${i}">${t('dev.add')}</button>`}</div>`).join('');
  if (list.dataset.html !== html) {
    list.dataset.html = html; list.innerHTML = html;
    list.querySelectorAll('[data-found]').forEach(b => b.addEventListener('click', () => {
      const f = E.found[+b.dataset.found];
      b.disabled = true;
      send({ cmd: 'dev_add', kind: f.kind, host: f.host, sub: f.sub, name: f.name || f.title, leds: 0 });
    }));
  }
  const ks = $('#man-kind');
  if (ks.options.length !== E.kinds.length + 1)   // + Nanoleaf: paired by address (Nanoleaf page), not a LAN device
    ks.innerHTML = `<option value="nanoleaf">Nanoleaf / Secretlab MAGRGB</option>` + E.kinds.map(k => `<option value="${k.kind}">${esc(k.title)}</option>`).join('');
  manualHints();
}
function manualHints() {
  const k = $('#man-kind').value;
  $('#man-sub-f').classList.toggle('hidden', k !== 'openrgb');
  $('#man-note').textContent = k ? t('dev.hint.' + k) : '';
}
const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
$('#scan-btn').addEventListener('click', () => { S.ext.scanning = 1; updateDevices(); send({ cmd: 'scan' }); });
$('#man-kind').addEventListener('change', manualHints);
$('#man-add').addEventListener('click', () => {
  const kind = $('#man-kind').value, host = $('#man-host').value.trim();
  if (!kind || !/^[\w.\-]+(:\d+)?$/.test(host)) { $('#man-host').focus(); return; }
  if (kind === 'nanoleaf') {   // pairing with that address; the Nanoleaf page shows what to press
    if (!/^\d+\.\d+\.\d+\.\d+$/.test(host)) { $('#man-host').focus(); return; }
    S.nano.pair = 1; send({ cmd: 'pair', ip: host }); $('#man-host').value = ''; showTab('nano'); updateNano();
    return;
  }
  send({ cmd: 'dev_add', kind, host, sub: kind === 'openrgb' ? (+$('#man-sub').value || 0) : -1, name: kindTitle(kind), leds: 0 });
  $('#man-host').value = '';
});

// ---- first-start wizard ([general] welcome=1 in a freshly created settings file)
let wzStep = 0, wzShown = false;
function wizard(show) {
  $('#wizard').classList.toggle('hidden', !show);
  if (show) { wzShown = true; wzGo(0); }
}
function wzGo(n) {
  wzStep = Math.max(0, Math.min(3, n));
  $$('.wz-step').forEach(s => s.classList.toggle('on', +s.dataset.step === wzStep));
  $$('#wz-dots i').forEach((d, i) => d.classList.toggle('on', i <= wzStep));
  $('#wz-back').classList.toggle('hidden', wzStep === 0);
  $('#wz-next span').textContent = t(wzStep === 3 ? 'wz.finish' : 'wz.next');
  if (wzStep === 2 && !S.ext.found.length && !S.ext.scanning) { S.ext.scanning = 1; send({ cmd: 'scan' }); }
  updateWizard();
}
function updateWizard() {
  if ($('#wizard').classList.contains('hidden')) return;
  $$('#wz-lang button').forEach(b => b.classList.toggle('on', b.dataset.v === LANG));
  const row = (name, ok, detail, note) => `<div class="wz-row"><div><b>${name}</b><small>${note || ''}</small></div>
      <span class="state"><span class="dot ${ok ? 'on' : 'off'}"></span>${detail}</span></div>`;
  $('#wz-pc').innerHTML =
    row(t('wz.board'), S.msi, S.msi ? t('wz.found') : t('wz.notfound'), S.msi ? '' : t('wz.nomsi')) +
    row(t('wz.mem'), S.sticks, S.sticks ? t('wz.sticks', S.sticks) : t('wz.notfound'), S.sticks || S.pawnio ? '' : t('wz.pawnio')) +
    row(t('wz.gpu'), S.gpu_temp != null, S.gpu_temp != null ? S.gpu_temp + ' °C' : t('wz.notfound'));
  const nL = nanoCtls();
  $('#wz-nano').textContent = nL.length ? t('wz.nano.on', nL.map(c => c.name || c.ip).join(', ')) : S.nano.pair ? '' : t('wz.nano.off');
  const E = S.ext;
  $('#wz-scan').disabled = !!E.scanning;
  $('#wz-scan span').textContent = E.scanning ? t('dev.scanning') : t('dev.scan');
  $('#wz-scan-st').textContent = E.scanning ? t('dev.scanning.note') : E.found.length ? t('dev.found', E.found.length) : '';
  const html = E.found.map((f, i) => `<div class="found-row"><span class="kind">${esc(f.title)}</span>
      <div class="what"><b>${esc(f.name || f.title)}</b><small>${esc(f.host)}${f.info ? ' · ' + esc(f.info) : ''}</small></div>
      ${f.added ? `<span class="added">${t('dev.added')}</span>` : `<button class="btn small" data-wzf="${i}">${t('dev.add')}</button>`}</div>`).join('');
  const box = $('#wz-found');
  if (box.dataset.html !== html) {
    box.dataset.html = html; box.innerHTML = html;
    box.querySelectorAll('[data-wzf]').forEach(b => b.addEventListener('click', () => {
      const f = E.found[+b.dataset.wzf]; b.disabled = true;
      send({ cmd: 'dev_add', kind: f.kind, host: f.host, sub: f.sub, name: f.name || f.title, leds: 0 });
    }));
  }
  const a = $('#wz-autostart'); a.checked = S.autostart === 1; a.disabled = S.autostart < 0;
}
$('#wz-back').addEventListener('click', () => wzGo(wzStep - 1));
$('#wz-next').addEventListener('click', () => {
  if (wzStep < 3) return wzGo(wzStep + 1);
  setCfg('general', 'welcome', 0);
  wizard(false);
});
$('#wz-scan').addEventListener('click', () => { S.ext.scanning = 1; updateWizard(); send({ cmd: 'scan' }); });
$('#wz-autostart').addEventListener('change', e => send({ cmd: 'autostart', v: e.target.checked ? 1 : 0 }));
$$('#wz-lang button').forEach(b => b.addEventListener('click', () => $$('#lang button').find(x => x.dataset.v === b.dataset.v).click()));

// settings
$('#autostart').addEventListener('change', e => send({ cmd: 'autostart', v: e.target.checked ? 1 : 0 }));
$('#ui-motion').addEventListener('change', e => { setCfg('general', 'ui_motion', e.target.checked ? 1 : 0); updateSettings(); });
function setTheme(v) { setCfg('general', 'theme', v); applyTheme(); send({ cmd: 'theme', v }); drawAll(); }
$$('#theme button').forEach(b => b.addEventListener('click', () => setTheme(b.dataset.v)));
// the code field: a right code opens its theme and switches to it (with a little burst), a wrong one shakes
$('#code').addEventListener('submit', e => {
  e.preventDefault();
  const inp = $('#code-in'), th = CODES[codeHash(inp.value.trim().toLowerCase())];
  inp.classList.remove('bad', 'ok'); void inp.offsetWidth;
  if (!th) { if (inp.value.trim()) inp.classList.add('bad'); return; }
  if (!unlocked().includes(th)) setCfg('general', 'unlocked', unlocked().concat(th).join(','));
  inp.value = ''; inp.classList.add('ok'); inp.blur();
  if (th === 'verity') { verityArrives(); return; }
  if (th === 'kyoka') { kyokaArrives(); return; }
  setTheme(th);
  if (!document.body.classList.contains('calm')) burst($('#code'), ['✨', '💖', '⭐', '💜', '🌈', '💫']);
});
function burst(at, bits) {
  const r = at.getBoundingClientRect();
  for (let i = 0; i < 28; i++) {
    const s = document.createElement('span');
    s.className = 'burst'; s.textContent = bits[i % bits.length];
    const a = Math.random() * Math.PI * 2, d = 60 + Math.random() * 140;
    s.style.left = r.left + r.width / 2 + 'px'; s.style.top = r.top + r.height / 2 + 'px';
    s.style.setProperty('--dx', Math.cos(a) * d + 'px'); s.style.setProperty('--dy', Math.sin(a) * d - 40 + 'px');
    s.style.setProperty('--rot', (Math.random() * 720 - 360) + 'deg');
    document.body.appendChild(s);
    setTimeout(() => s.remove(), 1300);
  }
}
$('#hotspot-auto').addEventListener('change', e => setCfg('hotspot', 'auto', e.target.checked ? 1 : 0));
$('#fps').addEventListener('input', e => { $('#fps-val').textContent = e.target.value; setCfgSoon('general', 'fps', e.target.value); });
$$('[data-open]').forEach(b => b.addEventListener('click', () => send({ cmd: 'open', what: b.dataset.open })));
$('#quit').addEventListener('click', () => send({ cmd: 'quit' }));

function buildHotkeys() {
  $('#hotkeys').innerHTML = HOTKEYS.map(k => `<div class="hk"><span>${t('hk.' + k)}</span><button class="kbd" data-hk="${k}"></button></div>`).join('');
  $$('[data-hk]').forEach(b => hotkeyButton(b, 'hotkeys', b.dataset.hk));
}

// a .kbd button that shows [s] k as a shortcut; a click records a new one (Esc cancels, Backspace clears)
function hotkeyButton(b, s, k) {
  const show = () => { const v = cv(s, k, ''); b.textContent = v ? v.replace(/\+/g, ' + ') : t('hk.none'); b.classList.toggle('empty', !v); };
  show();
  b.addEventListener('click', () => {
    $$('.kbd.rec').forEach(x => x !== b && x.blur());
    b.classList.add('rec'); b.textContent = t('hk.press');
    const onKey = e => {
      e.preventDefault(); e.stopPropagation();
      if (e.key === 'Escape') return done();
      if (e.key === 'Backspace') { setCfg(s, k, ''); return done(); }
      const map = { ArrowLeft: 'Left', ArrowRight: 'Right', ArrowUp: 'Up', ArrowDown: 'Down', PageUp: 'PageUp', PageDown: 'PageDown', Home: 'Home', End: 'End' };
      let key = map[e.key] || (/^F\d{1,2}$/.test(e.key) ? e.key : '');
      if (!key && /^(Key|Digit)[A-Z0-9]$/.test(e.code)) key = e.code.slice(-1);
      if (!key) return;   // modifier alone: keep waiting
      const mods = [e.ctrlKey && 'Ctrl', e.altKey && 'Alt', e.shiftKey && 'Shift', e.metaKey && 'Win'].filter(Boolean);
      if (!mods.length && !/^F\d/.test(key)) return;
      setCfg(s, k, [...mods, key].join('+'));
      done();
    };
    const done = () => { document.removeEventListener('keydown', onKey, true); b.classList.remove('rec'); show(); };
    document.addEventListener('keydown', onKey, true);
    b.addEventListener('blur', done, { once: true });
  });
}

let qrUrl = '';   // the address the QR code points at (the PC may be on several networks)
// ---- haku Pro: the trial / key in Settings, the chip in the sidebar, a notice on the tabs whose lights wait for Pro
const PRO_TABS = ['nano', 'bulbs'];
function proLocked() { return !!(S.pro && !S.pro.on); }
function updatePro() {
  // the open build has no network lights, sign-ins or phone control ("none" in their status): their places go
  document.body.classList.toggle('no-net', !!(S.nano && S.nano.none));
  const P = S.pro;
  $('#pro-card').classList.toggle('hidden', !P);
  $('#pro-chip').classList.toggle('hidden', !P || P.state === 'key');
  if (!P) { $$('.pro-lock').forEach(e => e.remove()); return; }
  const chip = $('#pro-chip');
  chip.textContent = P.state === 'trial' ? t('pro.chip.trial', P.days) : t('pro.chip.off');
  chip.classList.toggle('off', P.state === 'free');
  const badge = $('#pro-badge');
  badge.textContent = t('pro.badge.' + P.state); badge.classList.toggle('on', !!P.on);
  let st = P.state === 'trial' ? (P.days <= 1 ? t('pro.trial.last') : t('pro.trial', P.days))
    : P.state === 'key' ? t(P.kind === 'gift' ? 'pro.on.gift' : 'pro.on.sub') + (P.until ? ' ' + t('pro.until', P.until) : '')
    : t('pro.off');
  if (P.kind === 'sub' && P.checked) st += ' · ' + t('pro.checked', P.checked);
  if (P.stale) st += ' · ' + t('pro.stale');
  $('#pro-state').textContent = st;
  const inp = $('#pro-key');
  inp.placeholder = P.key ? t('pro.key.have', P.key) : t('pro.key.ph');
  inp.disabled = !!P.busy;
  $('#pro-form button span').textContent = P.busy ? t('pro.busy') : t('pro.activate');
  $('#pro-form button').disabled = !!P.busy;
  $('#pro-remove').classList.toggle('hidden', !P.key);
  $('#pro-buy').classList.toggle('hidden', P.state === 'key' && P.kind === 'gift');
  $('#pro-buy span').textContent = t(P.state === 'key' && P.kind === 'sub' ? 'pro.manage' : 'pro.buy');
  const err = $('#pro-err'); err.textContent = P.err || ''; err.classList.toggle('hidden', !P.err);
  // a notice on top of the tabs whose lights need Pro, and on the phone card
  const want = proLocked() ? [...PRO_TABS.map(x => '#tab-' + x), ...$$('.tab[data-brand]').filter(s => !['openrgb'].includes(s.dataset.brand)).map(s => '#' + s.id), '#phone-card'] : [];
  $$('.pro-lock').forEach(e => { if (!want.includes('#' + e.parentElement.id)) e.remove(); });
  want.forEach(sel => {
    const host = $(sel); if (!host || host.querySelector(':scope > .pro-lock')) return;
    const el = document.createElement('div');
    el.className = 'card pro-lock';
    el.innerHTML = `<div><h3>${t(sel === '#phone-card' ? 'pro.lock.phone' : 'pro.lock')}</h3><p class="muted">${t('pro.lock.text')}</p></div>
      <div class="btn-row"><button class="btn small primary" data-pro-buy>${t('pro.buy')}</button><button class="btn small ghost" data-pro-key>${t('pro.lock.key')}</button></div>`;
    el.querySelector('[data-pro-buy]').addEventListener('click', () => send({ cmd: 'open', what: 'pro' }));
    el.querySelector('[data-pro-key]').addEventListener('click', proGoKey);
    host.prepend(el);
  });
}
function proGoKey() { showTab('settings'); setTimeout(() => { $('#pro-card').scrollIntoView({ block: 'center', behavior: 'smooth' }); $('#pro-key').focus(); }, 60); }
$('#pro-chip').addEventListener('click', proGoKey);
$('#pro-buy').addEventListener('click', () => send({ cmd: 'open', what: 'pro' }));
$('#pro-form').addEventListener('submit', e => {
  e.preventDefault();
  const k = $('#pro-key').value.trim();
  if (!k) { $('#pro-key').focus(); return; }
  send({ cmd: 'pro_key', key: k }); $('#pro-key').value = '';
  S.pro.busy = 1; updatePro();
});
$('#pro-remove').addEventListener('click', () => {
  const b = $('#pro-remove');
  if (!b.classList.contains('confirm')) { b.classList.add('confirm'); b.querySelector('span').textContent = t('preset.delete.sure'); return; }
  b.classList.remove('confirm'); b.querySelector('span').textContent = t('pro.remove');
  send({ cmd: 'pro_key', key: '' });
});

// ---- Home Assistant (MQTT, haku Pro): the broker's address and login in [mqtt], the connection's state from the core
function updateHa() {
  const M = S.mqtt, card = $('#ha-card');
  card.classList.toggle('hidden', !M);   // (the open build has no MQTT)
  if (!M) return;
  const locked = !!(S.pro && !S.pro.on);
  $('#ha-badge').textContent = 'Pro'; $('#ha-badge').classList.toggle('on', !locked);
  const on = $('#ha-on'); on.checked = cv('mqtt', 'on', '0') === '1'; on.disabled = locked;
  [['#ha-host', 'host'], ['#ha-port', 'port'], ['#ha-user', 'user'], ['#ha-pass', 'password']].forEach(([id, k]) => {
    const el = $(id); el.disabled = locked;
    if (document.activeElement !== el) el.value = cv('mqtt', k, '');
  });
  const st = $('#ha-state');
  st.textContent = locked ? t('ha.pro') : !on.checked ? '' : M.state === 'connected' ? t('ha.connected', M.topic) :
    M.state === 'connecting' ? t('ha.connecting') : M.state === 'error' ? t('ha.error', M.err) : '';
  st.className = 'ha-state ' + (M.state === 'connected' && on.checked && !locked ? 'ok' : M.state === 'error' && on.checked ? 'bad' : '');
}
$('#ha-on').addEventListener('change', e => { setCfg('mqtt', 'on', e.target.checked ? '1' : '0'); updateHa(); });
[['#ha-host', 'host'], ['#ha-port', 'port'], ['#ha-user', 'user'], ['#ha-pass', 'password']].forEach(([id, k]) =>
  $(id).addEventListener('change', e => setCfg('mqtt', k, e.target.value.trim())));

function updateRemote() {
  const R = S.remote || {};
  $('#remote-on').checked = cv('remote', 'enabled', '0') === '1';
  $('#remote-info').classList.toggle('hidden', !(cv('remote', 'enabled', '0') === '1'));
  const urls = R.on ? R.urls || [] : [];
  if (!urls.includes(qrUrl)) qrUrl = urls[0] || '';
  const list = $('#remote-urls'), sig = urls.join() + '|' + qrUrl;
  if (list.dataset.sig !== sig) {   // rebuilt only when something changed, so a click is not lost
    list.dataset.sig = sig;
    list.innerHTML = R.on ? urls.map(u => `<button class="addr${u === qrUrl ? ' on' : ''}" data-u="${u}">${u.replace('http://', '')}</button>`).join('')
                          : `<p class="muted">${t('phone.off')}</p>`;
    list.querySelectorAll('.addr').forEach(b => b.addEventListener('click', () => { qrUrl = b.dataset.u; updateRemote(); }));
  }
  const link = R.on && qrUrl && R.pin ? `${qrUrl}/#pin=${R.pin}` : '';
  const qr = $('#remote-qr');
  if (qr.dataset.link !== link) { qr.dataset.link = link; qr.innerHTML = link ? qrSvg(link) : ''; }
  $('#remote-pin').textContent = R.on ? R.pin : '';
  $('#remote-paired').textContent = t('phone.paired', R.paired || 0);
}
$('#remote-on').addEventListener('change', e => { setCfg('remote', 'enabled', e.target.checked ? 1 : 0); updateRemote(); });
$('#remote-newpin').addEventListener('click', () => send({ cmd: 'remote_pin' }));
$('#remote-forget').addEventListener('click', () => send({ cmd: 'remote_forget' }));

function updateUpdate() {
  const U = S.update || {};
  $('#about1').textContent = t('about1', U.version || '');
  $('#upd-row').classList.toggle('hidden', !U.repo);
  $('#upd-btns').classList.toggle('hidden', !U.repo);
  const st = U.inst || 0, busy = st >= 1 && st <= 3;
  $('#upd-status').textContent = st === 1 ? t('upd.dl', U.pct || 0) : st === 2 ? t('upd.verify') : st === 3 ? t('upd.run') :
    st === 4 ? t('upd.err.' + U.err) : U.latest ? t('upd.avail', U.latest) : t('upd.note');
  $('#upd-status').classList.toggle('bad', st === 4);
  $('#upd-install').classList.toggle('hidden', !U.latest || !U.can);
  $('#upd-install').textContent = st === 4 ? t('upd.retry') : t('upd.install', U.latest || '');
  $('#upd-install').disabled = busy;
  $('#upd-get').classList.toggle('hidden', !U.latest);
  $('#upd-get').textContent = t('upd.page');
}
$('#upd-install').addEventListener('click', () => { S.update = { ...S.update, inst: 1, pct: 0 }; updateUpdate(); send({ cmd: 'update_install' }); });
$('#upd-get').addEventListener('click', () => send({ cmd: 'open', what: 'release' }));
$('#upd-now').addEventListener('click', () => send({ cmd: 'update_check' }));
// ---- what is new: shown once in the window after an update ([general] seen_version; the texts are in
// whatsnew.js), with every version since the one seen last; Settings opens it again. Not on the phone, and not
// on a fresh install (the first-start guide is shown then).
const verNum = v => String(v || '').split('.').concat(['0', '0', '0', '0']).slice(0, 4).reduce((a, x) => a * 10000 + (parseInt(x, 10) || 0), 0);
let wnChecked = false;
function whatsNew(all) {
  const cur = (S.update || {}).version || '', seen = cv('general', 'seen_version', '');
  const list = (typeof WHATSNEW === 'undefined' ? [] : WHATSNEW).filter(e => verNum(e.v) <= verNum(cur) &&
    (all || (seen ? verNum(e.v) > verNum(seen) : e.v === cur)));
  if (!list.length) return false;
  $('#wn-title').textContent = t('wn.title', cur);
  $('#wn-list').innerHTML = list.map(e => `<section><h4>${t('wn.v', e.v)}</h4><ul>${(e[LANG] || e.en).map(p => `<li>${esc(p)}</li>`).join('')}</ul></section>`).join('');
  $('#wn').classList.remove('hidden');
  $('#wn-list').scrollTop = 0;
  return true;
}
function whatsNewCheck() {
  if (wnChecked || document.documentElement.classList.contains('remote')) return;
  const cur = (S.update || {}).version; if (!cur) return;
  wnChecked = true;
  if (cv('general', 'seen_version', '') === cur) return;
  if (wzShown || !whatsNew(false)) setCfg('general', 'seen_version', cur);   // a fresh install, or nothing to show
}
function whatsNewClose() { $('#wn').classList.add('hidden'); const cur = (S.update || {}).version; if (cur) setCfg('general', 'seen_version', cur); }
$('#wn-ok').addEventListener('click', whatsNewClose);
$('#wn').addEventListener('pointerdown', e => { if (e.target.id === 'wn') whatsNewClose(); });
document.addEventListener('keydown', e => { if (e.key === 'Escape' && !$('#wn').classList.contains('hidden')) whatsNewClose(); });
$('#wn-open').addEventListener('click', () => whatsNew(true));

// diagnostics: one text file in Downloads (log, settings, devices, network), without keys or passwords
// ---- reporting a problem: a window where the person says what happened; it goes to the developer with the
// diagnostics (diag.c, the site's report API). Saving the diagnostics as a file stays as the way without sending.
const REPORT = { open: false, sent: false };
function updateDiag() {
  const D = S.diag || {}, R = D.report || {};
  $('#diag-go').disabled = !!D.busy;
  $('#diag-note').textContent = D.busy ? t('diag.busy') : D.ok === 1 ? t('diag.done', D.file) : D.ok === 0 ? t('diag.fail') : t('report.note');
  if (!REPORT.open) return;
  const busy = !!R.busy;
  if (R.ok === 1 && REPORT.waiting) { REPORT.waiting = false; REPORT.sent = true; $('#report-text').value = ''; }
  if (R.ok === 0 && REPORT.waiting) REPORT.waiting = false;
  $('#report-send').disabled = busy || REPORT.sent;
  $('#report-text').disabled = $('#report-contact').disabled = $('#report-attach').disabled = busy || REPORT.sent;
  $('#report-close span').textContent = t(REPORT.sent ? 'close' : 'cancel');
  const msg = $('#report-msg');
  msg.classList.toggle('bad', R.ok === 0 && !busy);
  msg.textContent = busy ? t('report.sending') : REPORT.sent ? t('report.sent', R.id || '') : R.ok === 0 && REPORT.tried
    ? t({ busy: 'report.err.busy', offline: 'report.err.offline', invalid: 'report.err.invalid' }[R.err] || 'report.err') : REPORT.short ? t('report.short') : '';
}
function reportDialog() {
  REPORT.open = true; REPORT.sent = REPORT.tried = REPORT.short = REPORT.waiting = false;
  $('#report-text').placeholder = t('report.ph');
  $('#report-contact').placeholder = t('report.contact.ph');
  $('#report-dlg').classList.remove('hidden');
  updateDiag();
  setTimeout(() => $('#report-text').focus(), 30);
}
const closeReport = () => { REPORT.open = false; $('#report-dlg').classList.add('hidden'); };
$('#report-open').addEventListener('click', reportDialog);
$('#report-close').addEventListener('click', closeReport);
$('#report-dlg').addEventListener('pointerdown', e => { if (e.target.id === 'report-dlg') closeReport(); });
document.addEventListener('keydown', e => { if (e.key === 'Escape' && REPORT.open) closeReport(); });
$('#report-form').addEventListener('submit', e => {
  e.preventDefault();
  const text = $('#report-text').value.trim();
  REPORT.short = text.length < 10;
  if (REPORT.short) { updateDiag(); $('#report-text').focus(); return; }
  REPORT.tried = REPORT.waiting = true;
  S.diag = Object.assign({}, S.diag, { report: { busy: 1, ok: -1 } });
  send({ cmd: 'report', text, contact: $('#report-contact').value.trim(), install: commInstall(), attach: $('#report-attach').checked ? '1' : '0' });
  updateDiag();
});
$('#diag-go').addEventListener('click', () => { S.diag = Object.assign({}, S.diag, { busy: 1 }); updateDiag(); send({ cmd: 'diag' }); });

// ------------------------------------------------------------------ mood: describe it, a local model picks the colours
const MOOD = { base: 0, pending: false, err: '', applied: false, text: '' };
function askMood(again) {
  const text = again ? MOOD.text : $('#mood-in').value.trim();
  if (!text || (S.mood || {}).busy) return;
  MOOD.text = text; MOOD.base = (S.mood || {}).seq || 0; MOOD.pending = true; MOOD.err = ''; MOOD.applied = false;
  send({ cmd: 'mood', text, again: again ? '1' : '0' });
  updateMood();
}
function updateMood() {
  const M = S.mood || {};
  if (MOOD.pending && M.seq > MOOD.base && !M.busy) { MOOD.pending = false; MOOD.err = M.err || ''; }
  const busy = !!M.busy || MOOD.pending;
  $('#mood-in').placeholder = t('mood.ph');
  $('#mood').classList.toggle('busy', busy);
  $('#mood-go').disabled = $('#mood-again').disabled = $('#mood-apply').disabled = $('#mood-save').disabled = busy;
  const has = (M.colors || []).length > 1;
  $('#mood-out').classList.toggle('hidden', !has);
  if (has) {
    const pal = M.colors.join();
    if ($('#mood-pal').dataset.pal !== pal) {
      $('#mood-pal').dataset.pal = pal;
      $('#mood-pal').innerHTML = M.colors.map(c => `<i style="background:${c}"></i>`).join('');
    }
    const fx = (S.effects.find(e => e.id === M.effect) || {}).title || M.effect;
    $('#mood-name').textContent = M.name || fx;
    $('#mood-fx').textContent = `${fx} · ${t('speed').toLowerCase()} ${M.speed}`;
  }
  $('#mood-msg').textContent = busy ? t('mood.busy') : MOOD.err ? t('mood.' + MOOD.err) : MOOD.applied ? t('mood.applied') : '';
}
// ---- the local model: set up from here (Ollama's installer, then the model), and kept out of memory
const AI_BUSY = [1, 2, 3, 4, 5];
function updateAi() {
  const A = S.ollama || {}, st = A.stage || 0, busy = AI_BUSY.includes(st), ready = A.exe && A.model;
  $('#ai-status').textContent = busy ? t('ai.st.' + st, A.pct || 0) : st === 7 ? t('ai.err.' + A.err) : st === 6 ? t('ai.st.6') :
    ready ? t('ai.ready') : A.exe ? t('ai.nomodel') : t('ai.none');
  $('#ai-status').classList.toggle('bad', st === 7);
  const bar = $('#ai-bar'), known = st === 1 || st === 5;
  bar.classList.toggle('hidden', !busy);
  bar.classList.toggle('busy', busy && !known);
  bar.firstElementChild.style.width = known ? (A.pct || 0) + '%' : '';
  const b = $('#ai-setup');
  b.classList.toggle('hidden', !!ready && st !== 7);
  b.disabled = busy;
  b.querySelector('span').textContent = st === 7 ? t('ai.retry') : t('ai.go');
  $('#ai-demand').checked = !!A.on_demand;
  $('#ai-demand').disabled = !A.exe;
  $('#ai-demand-note').textContent = t('ai.demand.note') + (A.ours ? ' ' + t('ai.demand.running') : '');
  // the mood card offers the same button while the model is missing
  const ms = $('#mood-setup');
  ms.classList.toggle('hidden', !!ready || !S.ollama);
  ms.disabled = busy;
  ms.querySelector('span').textContent = busy ? t('ai.st.' + st, A.pct || 0) : t('ai.go');
}
function aiSetup() { S.ollama = { ...(S.ollama || {}), stage: 4, pct: 0, err: '' }; updateAi(); send({ cmd: 'ai_setup' }); }
$('#ai-setup').addEventListener('click', aiSetup);
$('#mood-setup').addEventListener('click', aiSetup);
$('#ai-demand').addEventListener('change', e => send({ cmd: 'ai_on_demand', v: e.target.checked ? '1' : '0' }));
$('#mood-form').addEventListener('submit', e => { e.preventDefault(); askMood(false); });
$('#mood-again').addEventListener('click', () => askMood(true));
$('#mood-save').addEventListener('click', () => { $('#mood-apply').click(); presetDialog(0, (S.mood || {}).name || ''); });
$('#mood-apply').addEventListener('click', () => {
  const M = S.mood || {};
  if (!(M.colors || []).length) return;
  setCfg(M.effect, 'palette', palStr(M.colors));
  setCfg(M.effect, 'speed', M.speed);
  S.effect = M.effect;
  send({ cmd: 'effect', id: M.effect });
  MOOD.applied = true;
  renderEffectSide(); markEffect(); drawAll(); updateMood();
});

function updateSettings() {
  updateRemote();
  updateUpdate();
  updateDiag();
  const a = $('#autostart');
  a.checked = S.autostart === 1; a.disabled = S.autostart < 0;
  $('#hotspot-auto').checked = cv('hotspot', 'auto', '0') !== '0';
  const motion = cv('general', 'ui_motion', '1') !== '0';
  $('#ui-motion').checked = motion; document.body.classList.toggle('calm', !motion);
  $('#hotspot-status').textContent = S.hotspot ? t('hotspot.on') : t('hotspot.off');
  const f = +cv('general', 'fps', 30); setRange($('#fps'), f); $('#fps-val').textContent = f;
  const ex = cv('general', 'on_exit', 'off');
  $$('#on-exit button').forEach(b => b.classList.toggle('on', b.dataset.v === ex));
  $('#on-exit-note').textContent = t('exit.' + ex + '.note');
  $$('#lang button').forEach(b => b.classList.toggle('on', b.dataset.v === LANG));
}
$$('#on-exit button').forEach(b => b.addEventListener('click', () => { setCfg('general', 'on_exit', b.dataset.v); updateSettings(); }));

// language switch: everything is re-rendered in the new language
$$('#lang button').forEach(b => b.addEventListener('click', () => {
  if (LANG === b.dataset.v) return;
  LANG = b.dataset.v;
  setCfg('general', 'lang', LANG);
  applyI18n(); applyTheme(); buildEffects(); buildHotkeys(); buildBulbs(); buildDevices(); renderEffectSide();
  showTab(tab); updateNano(); updateChips(); updateSettings(); drawAll();
  if (!$('#wizard').classList.contains('hidden')) wzGo(wzStep);
}));

// ------------------------------------------------------------------ status chips
function chip(dot, text) { return `<span class="chip"><span class="dot ${dot}"></span>${text}</span>`; }
function updateChips() {
  const h = [];
  if (S.msi) h.push(chip(S.msi_own ? 'off' : 'on', `${t('chip.board')} <b>MSI</b>`));
  if (S.sticks) h.push(chip(S.ram_own ? 'off' : 'on', `${t('chip.mem')} <b>${S.sticks} ${t('chip.pcs')}</b>`));
  if (S.gpu_temp != null) h.push(chip('on', `GPU <b>${S.gpu_temp}°</b>`));
  const nL = nanoCtls();
  if (nL.length) { const on = nL.filter(c => c.online).length; h.push(chip(on === nL.length ? 'on' : on ? 'warn' : 'off', nL.length > 1 ? `Nanoleaf <b>${on}/${nL.length}</b>` : `<b>Nanoleaf</b>`)); }
  if (S.bulbs.length) { const on = S.bulbs.filter(b => b.online).length; h.push(chip(on === S.bulbs.length ? 'on' : on ? 'warn' : 'off', `${t('chip.lamps')} <b>${on}/${S.bulbs.length}</b>`)); }
  const ed = S.ext.devs.filter(d => d.enabled);
  if (ed.length) { const on = ed.filter(d => d.online).length; h.push(chip(on === ed.length ? 'on' : on ? 'warn' : 'off', `${t('chip.devs')} <b>${on}/${ed.length}</b>`)); }
  $('#chips').innerHTML = h.join('');
  $('#ram-status').textContent = !S.sticks ? t('ram.none') : S.ram_own ? t('own.status') : t('ram.status', S.sticks);
  $$('[data-own]').forEach(el => { if (el !== document.activeElement) el.checked = !!S[el.dataset.own === 'msi_own' ? 'msi_own' : 'ram_own']; });
  // all of the PC's lighting given back / taken again: shown once there is something of the PC haku lights
  const pcDevs = S.sticks || S.msi || S.ext.devs.some(pcDev), pb = $('#pc-own');
  pb.classList.toggle('hidden', !pcDevs);
  pb.querySelector('span').textContent = t(S.pc_own ? 'pc.own.take' : 'pc.own.give');
  pb.title = t('pc.own.tip');
  // the Memory and ARGB strip cards only where there is such hardware; the rest of the PC tab is for every PC
  const rc = $('#ram-card'), gc = $('#gpu-card');
  if (rc.classList.contains('hidden') === !!S.sticks || gc.classList.contains('hidden') === !!S.msi) {
    rc.classList.toggle('hidden', !S.sticks); gc.classList.toggle('hidden', !S.msi);
    requestAnimationFrame(sizeCanvases);
  }
  updateOrgb();
  updateHw();
  if (S.effect === 'screen') updateScreen();
  $('#gpu-status').textContent = !S.msi ? t('gpu.none') : S.msi_own ? t('own.status') : t('gpu.status');
  $('#strip-title').textContent = stripName();
  const sn = $('#strip-name'); if (document.activeElement !== sn) sn.value = cv('layout', 'strip_name', '');
  sn.placeholder = t('pc.block');
}

// ------------------------------------------------------------------ messages from the core
function applyStatus(m) {
  const bulbCountChanged = (m.bulbs || []).length !== S.bulbs.length;
  const nanoSig = N => JSON.stringify(((N || {}).ctls || []).map(c => [c.slot, c.panels]));
  const nanoLayoutChanged = nanoSig(m.nano) !== nanoSig(S.nano);
  const effectChanged = m.effect !== S.effect;
  Object.assign(S, m);
  S.ext = S.ext || { devs: [], found: [], kinds: [], scanning: 0 };
  const sig = JSON.stringify(S.ext.devs.map(d => [d.id, d.leds, d.enabled, d.per_led, d.name, d.type, d.zones]));
  if (!dragging) { setRange($('#bright'), S.brightness); $('#bright-val').textContent = S.brightness + '%'; }
  if (effectChanged) { markEffect(); renderEffectSide(); }
  if (bulbCountChanged) buildBulbs(); else updateBulbs();
  if (sig !== devSig) { devSig = sig; buildDevices(); drawAll(); } else updateDevices();
  updateNano(); updateChips(); updateSettings(); updateWizard(); updateMood(); updateAi(); updateNav(); updatePro(); updateHa(); updateBackup();
  if (nanoLayoutChanged) drawAll();
}

if (wv) wv.addEventListener('message', e => {
  const m = e.data;
  if (m.type === 'frame') onFrame(m.l);
  else if (m.type === 'status') applyStatus(m);
  else if (m.type === 'state') {
    S.cfg = m.cfg || {}; S.effects = m.effects || []; S.autostart = m.autostart;
    LANG = ['ru', 'fr'].includes(cv('general', 'lang', 'en')) ? cv('general', 'lang', 'en') : 'en';
    applyI18n();
    applyTheme(); buildEffects(); buildHotkeys(); syncToggles(); renderProfiles();
    S.effect = null;           // force a full refresh
    S.bulbs = [];
    devSig = '';
    applyStatus(m);
    renderEffectSide();
    sizeCanvases();
    if (!wzShown && cv('general', 'welcome', '0') === '1') wizard(true);
    whatsNewCheck();
  }
});

$('#bright').addEventListener('input', e => { $('#bright-val').textContent = e.target.value + '%'; send({ cmd: 'brightness', v: +e.target.value }); });
$('#power').addEventListener('click', () => send({ cmd: 'power' }));
document.addEventListener('keydown', e => { if (e.key === 'Escape' && !pk.classList.contains('hidden')) closePicker(); });

// ------------------------------------------------------------------ collapsible sidebar (the triangle tab)
function setSide(collapsed) {
  document.body.classList.toggle('collapsed', collapsed);
  $('#side-toggle').dataset.i18nTitle = collapsed ? 'side.show' : 'side.hide';
  $('#side-toggle').title = t(collapsed ? 'side.show' : 'side.hide');
  try { localStorage.setItem('side', collapsed ? '1' : '0'); } catch (e) { }
}
$('#side-toggle').addEventListener('click', () => setSide(!document.body.classList.contains('collapsed')));
try { if (localStorage.getItem('side') === '1') { document.body.classList.add('no-anim'); setSide(true); requestAnimationFrame(() => document.body.classList.remove('no-anim')); } } catch (e) { }

applyI18n();
try { const tb = localStorage.getItem('tab') || ''; if (tb.startsWith('brand-')) wantTab = tb; showTab(TABS.includes(tb) ? tb : 'effects'); } catch (e) { showTab('effects'); }
$$('.range').forEach(fill);
send({ cmd: 'hello' });
