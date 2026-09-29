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
const HOTKEYS = ['next', 'prev', 'off', 'brighter', 'dimmer'];
const PRESETS = ['#FF0000', '#FF5A00', '#FFA000', '#FFE000', '#9DFF00', '#00FF6A', '#00FFD5', '#00C8FF',
  '#0068FF', '#2B2BFF', '#7A3CFF', '#B400FF', '#FF00D4', '#FF2D95', '#FFB070', '#FFFFFF'];
const DEFAULT_PAL = ['#00C8FF', '#7A3CFF', '#FF2D95'];
const OFF = '#1c1c1c';
const WIDE = '"Segoe UI Variable Text", "Segoe UI", sans-serif';

let S = { cfg: {}, effects: [], effect: 'flow', brightness: 100, bulbs: [], nano: {}, ext: { devs: [], found: [], kinds: [], scanning: 0 }, autostart: -1 };
let F = { ram: [[], []], gpu: [], board: null, bulbs: [], nano: [], ext: {} };
let tab = 'effects';
let dragging = false;

// ------------------------------------------------------------------ config helpers
const cv = (s, k, d) => { const x = S.cfg[s]; return x && x[k] != null && x[k] !== '' ? x[k] : d; };
function setCfg(s, k, v) {
  v = String(v);
  (S.cfg[s] = S.cfg[s] || {})[k] = v;
  send({ cmd: 'set', s, k, v });
}
const parsePal = str => (str || '').match(/#[0-9a-fA-F]{6}/g)?.map(x => x.toUpperCase()) || [];
const palStr = a => a.join(', ');
const effPal = id => { const p = parsePal(cv(id, 'palette')); if (p.length) return p; const g = parsePal(cv('general', 'palette')); return g.length ? g : DEFAULT_PAL; };

// throttled sender for continuous edits (colour dragging)
const pendingSets = new Map();
let flushTimer = 0;
function setCfgSoon(s, k, v) {
  (S.cfg[s] = S.cfg[s] || {})[k] = String(v);
  pendingSets.set(s + '\u0001' + k, [s, k, String(v)]);
  if (!flushTimer) flushTimer = setTimeout(() => {
    flushTimer = 0;
    for (const [s2, k2, v2] of pendingSets.values()) send({ cmd: 'set', s: s2, k: k2, v: v2 });
    pendingSets.clear();
  }, 40);
}

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
function showTab(t) {
  tab = t;
  $('main').scrollTop = 0;
  $$('#nav button').forEach(b => b.classList.toggle('active', b.dataset.tab === t));
  $$('.tab').forEach(s => s.classList.toggle('active', s.id === 'tab-' + t));
  $('#title').textContent = window.t('nav.' + t);
  $('#subtitle').textContent = window.t('sub.' + t);
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
function renderZone(el) {
  const section = el.dataset.zone, mode = zoneMode(section);
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
      <input type="range" class="range zb" min="5" max="100" value="${br}">
    </div>`;
  const body = el.querySelector('.zbody');
  el.querySelector('.zfx').addEventListener('change', e => { setCfg(section, 'effect', e.target.value); renderZone(el); renderOwnList(); });
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
const renderZones = () => $$('.zone').forEach(renderZone);

// ------------------------------------------------------------------ effects tab
function buildEffects() {
  const grid = $('#fx-grid');
  grid.innerHTML = '';
  S.effects.forEach((e, n) => {
    const b = document.createElement('button');
    b.className = 'fx'; b.dataset.id = e.id;
    b.innerHTML = `<div class="top"><b>${fxName(e.id)}</b><span class="num-i">${String(n + 1).padStart(2, '0')}</span></div><small>${t('short.' + e.id)}</small><canvas></canvas>`;
    b.addEventListener('click', () => { S.effect = e.id; send({ cmd: 'effect', id: e.id }); renderEffectSide(); markEffect(); });
    grid.appendChild(b);
  });
  markEffect();
}
function markEffect() {
  $$('.fx').forEach(b => b.classList.toggle('on', b.dataset.id === S.effect));
  const off = S.effect === 'off';
  $('#power').classList.toggle('off', off);
  $('#power span').textContent = off ? t('power.on') : t('power.off');
  updateBrand();
}
function renderEffectSide() {
  const id = S.effect;
  $('#fx-name').textContent = fxName(id);
  $('#fx-desc').textContent = t('desc.' + id);
  $('#fx-pal-field').classList.toggle('hidden', id === 'off');
  renderPalette($('#fx-pal'), id, false, effPal(id));
  const hasSpeed = !['static', 'off', 'temperature'].includes(id);
  $('#fx-speed-field').classList.toggle('hidden', !hasSpeed);
  const sp = +cv(id, 'speed', cv('general', 'speed', 5));
  setRange($('#fx-speed'), sp); $('#fx-speed-val').textContent = sp;
  $('#fx-temp').classList.toggle('hidden', id !== 'temperature');
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
  if (z === 'zone.nanoleaf') return 'Nanoleaf';
  const dm = /^zone\.dev(\d+)$/.exec(z); if (dm) { const d = S.ext.devs.find(x => x.id === +dm[1]); return d ? d.name : z; }
  const m = /^zone\.light(\d+)$/.exec(z); return m ? t('bulb', m[1]) : z;
}
function renderOwnList() {
  const zones = ['zone.ram', 'zone.gpu'].concat(S.nano && S.nano.configured ? ['zone.nanoleaf'] : [], S.bulbs.map((_, i) => 'zone.light' + (i + 1)), S.ext.devs.map(d => 'zone.dev' + d.id));
  $('#fx-own').innerHTML = zones.filter(z => cv(z, 'effect', '')).map(z => {
    const id = cv(z, 'effect');
    return `<div class="own-item"><span>${zoneName(z)}</span><b>${id === 'off' ? t('zone.offfx') : fxName(id)}</b></div>`;
  }).join('');
}
$('#fx-sync').addEventListener('change', e => { setCfg('general', 'sync', e.target.checked ? 1 : 0); renderEffectSide(); });
$('#fx-speed').addEventListener('input', e => { $('#fx-speed-val').textContent = e.target.value; setCfgSoon(S.effect, 'speed', e.target.value); });
$$('#temp-src button').forEach(b => b.addEventListener('click', () => { setCfg('temperature', 'source', b.dataset.v); renderEffectSide(); }));
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
  for (const b of $$('.fx')) {
    const id = b.dataset.id, cvs = b.querySelector('canvas');
    if (cvs.width !== cvs.clientWidth * devicePixelRatio) { cvs.width = cvs.clientWidth * devicePixelRatio; cvs.height = cvs.clientHeight * devicePixelRatio; }
    const spd = +cv(id, 'speed', cv('general', 'speed', 5)) / 5;
    drawStrip(cvs, preview(id, effPal(id).map(hex2rgb), t * spd, 28));
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

function rr(c, x, y, w, h, r) { c.beginPath(); c.roundRect(x, y, w, h, r); }
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
    c.fillStyle = '#4e4e4e'; c.font = `${9.5 * d}px ${WIDE}`; c.textAlign = 'right';
    c.fillText(t('board'), bx - 10 * d, by + 4 * d);
  }
}

// Nanoleaf panels at their wall positions; panel size from the nearest neighbour.
function nanoGeometry() {
  const P = (S.nano && S.nano.panels) || [];
  const side = S.nano.side || 0.25;
  return P.map(([x, y], i) => {
    let s = side;
    P.forEach(([x2, y2], j) => { if (i !== j) s = Math.min(s, Math.max(Math.abs(x - x2), Math.abs(y - y2))); });
    return { x, y, s };
  });
}
function drawNano(c, X, Y, W, H) {
  const G = nanoGeometry(), d = devicePixelRatio;
  if (!G.length) {
    c.fillStyle = '#4e4e4e'; c.font = `${12 * d}px ${WIDE}`; c.textAlign = 'center';
    c.fillText(S.nano.configured ? t('nano.wait') : t('nano.none'), X + W / 2, Y + H / 2);
    return;
  }
  let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
  G.forEach(p => { x0 = Math.min(x0, p.x - p.s / 2); x1 = Math.max(x1, p.x + p.s / 2); y0 = Math.min(y0, p.y - p.s / 2); y1 = Math.max(y1, p.y + p.s / 2); });
  const pad = 18 * d, k = Math.min((W - pad * 2) / (x1 - x0), (H - pad * 2) / (y1 - y0));
  const ox = X + (W - (x1 - x0) * k) / 2 - x0 * k, oy = Y + (H - (y1 - y0) * k) / 2 - y0 * k;
  G.forEach((p, i) => {
    const col = F.nano[i] || OFF, s = p.s * k, g = s * 0.06;
    const px = ox + p.x * k - s / 2 + g, py = oy + p.y * k - s / 2 + g, ps = s - g * 2;
    c.save();
    if (lit(col)) { c.shadowColor = col; c.shadowBlur = 30 * d; }
    rr(c, px, py, ps, ps, ps * 0.02); c.fillStyle = col; c.fill();
    c.restore();
    const sh = c.createLinearGradient(px, py, px + ps, py + ps);
    sh.addColorStop(0, 'rgba(255,255,255,.25)'); sh.addColorStop(.5, 'rgba(255,255,255,0)'); sh.addColorStop(1, 'rgba(0,0,0,.2)');
    rr(c, px, py, ps, ps, ps * 0.02); c.fillStyle = sh; c.fill();
    c.strokeStyle = 'rgba(255,255,255,.08)'; c.lineWidth = d; c.stroke();
  });
}

function drawBulbs(c, X, Y, W, H) {
  const n = S.bulbs.length, d = devicePixelRatio;
  if (!n) return;
  const r = Math.min(W * 0.3, H / (n * 2.6), 26 * d);
  for (let i = 0; i < n; i++) {
    const cx = X + W / 2, cy = Y + H / 2 + (i - (n - 1) / 2) * r * 2.7, col = F.bulbs[i];
    const on = lit(col) && S.bulbs[i].online;
    const g = c.createRadialGradient(cx, cy - r * 0.1, 0, cx, cy, r * 1.7);
    if (on) { g.addColorStop(0, '#fff'); g.addColorStop(0.28, col); g.addColorStop(1, 'rgba(0,0,0,0)'); }
    else { g.addColorStop(0, '#262626'); g.addColorStop(0.5, '#161616'); g.addColorStop(1, 'rgba(0,0,0,0)'); }
    c.beginPath(); c.arc(cx, cy, r * 1.7, 0, 7); c.fillStyle = g; c.fill();
  }
}

// A LAN device: a strip (light bar) or separate lights (orbs), from the thinned-out live frame.
function drawExt(c, X, Y, W, H, k) {
  const d = S.ext.devs[k], dp = devicePixelRatio;
  if (!d) return;
  const cols = F.ext[k] || [];
  if (d.per_led) {
    const n = Math.max(cols.length, 2), bw = Math.min(W * 0.92, 560 * dp), bh = Math.min(H * 0.34, 14 * dp);
    lightBar(c, X + (W - bw) / 2, Y + (H - bh) / 2, bw, bh, [...Array(n)].map((_, i) => d.online ? cols[i] || OFF : OFF), false);
  } else {
    const n = Math.max(1, d.leds), r = Math.min(H * 0.28, W / (n * 3.2), 24 * dp);
    for (let i = 0; i < n; i++) {
      const cx = X + W / 2 + (i - (n - 1) / 2) * r * 3.2, cy = Y + H / 2, col = cols[i], on = lit(col) && d.online;
      const g = c.createRadialGradient(cx, cy, 0, cx, cy, r * 1.6);
      if (on) { g.addColorStop(0, '#fff'); g.addColorStop(0.3, col); g.addColorStop(1, 'rgba(0,0,0,0)'); }
      else { g.addColorStop(0, '#262626'); g.addColorStop(0.5, '#161616'); g.addColorStop(1, 'rgba(0,0,0,0)'); }
      c.beginPath(); c.arc(cx, cy, r * 1.6, 0, 7); c.fillStyle = g; c.fill();
    }
  }
}
function drawExtAll(c, X, Y, W, H) {
  const list = S.ext.devs.map((d, k) => [d, k]).filter(([d]) => d.enabled && d.leds).slice(0, 6);
  const rh = H / Math.max(1, list.length);
  list.forEach(([d, k], i) => drawExt(c, X, Y + i * rh, W, rh, k));
}

function label(c, text, x, y) {
  const d = devicePixelRatio;
  c.fillStyle = '#4e4e4e'; c.font = `${9.5 * d}px ${WIDE}`; c.textAlign = 'center'; c.letterSpacing = `${3 * d}px`;
  c.fillText(text.toUpperCase(), x, y);
}

function drawHero() {
  const cvs = $('#hero'); if (!cvs.width) return;
  const c = cvs.getContext('2d'), W = cvs.width, H = cvs.height, d = devicePixelRatio;
  c.clearRect(0, 0, W, H);
  const hasN = S.nano.configured, hasB = S.bulbs.length > 0;
  const cols = [['ram', 0.2], ['gpu', 0.38]];
  if (hasN) cols.push(['nano', 0.27]);
  if (hasB) cols.push(['bulbs', 0.15]);
  if (S.ext.devs.some(d => d.enabled && d.leds)) cols.push(['ext', 0.3]);
  const tot = cols.reduce((a, b) => a + b[1], 0);
  let x = 0;
  const names = { ram: t('pc.memory'), gpu: stripName(), nano: 'Nanoleaf', bulbs: t('nav.bulbs'), ext: t('nav.devices') };
  for (const [k, f] of cols) {
    const w = W * f / tot, top = 18 * d, h = H - 46 * d;
    if (k === 'ram') drawRam(c, x, top, w, h);
    if (k === 'gpu') drawGpu(c, x, top, w, h);
    if (k === 'nano') drawNano(c, x, top, w, h);
    if (k === 'bulbs') drawBulbs(c, x, top, w, h);
    if (k === 'ext') drawExtAll(c, x, top, w, h);
    if (w > 64 * d) label(c, names[k], x + w / 2, H - 16 * d);
    x += w;
  }
}

function drawCanvas(id, fn) {
  const cvs = $(id); if (!cvs || !cvs.width || !cvs.offsetParent) return;
  const c = cvs.getContext('2d');
  c.clearRect(0, 0, cvs.width, cvs.height);
  fn(c, 0, 0, cvs.width, cvs.height);
}

function drawAll() {
  if (tab === 'effects') drawHero();
  if (tab === 'pc') { drawCanvas('#live-ram', drawRam); drawCanvas('#live-gpu', drawGpu); }
  if (tab === 'nano') drawCanvas('#live-nano', drawNano);
  if (tab === 'devices') S.ext.devs.forEach((d, k) => drawCanvas('#dev-live-' + d.id, (c, x, y, w, h) => drawExt(c, x, y, w, h, k)));
  if (tab === 'bulbs') S.bulbs.forEach((b, i) => {
    const o = $('#bulb-orb-' + i); if (!o) return;
    const col = F.bulbs[i], on = lit(col) && b.online;
    o.classList.toggle('off', !on);
    if (on) o.style.setProperty('--c', col);
  });
}

function onFrame(l) {
  F = { ram: [[], []], gpu: [], board: null, bulbs: [], nano: [], ext: {} };
  for (const [d, i, c] of l) {
    const col = '#' + c;
    if (d === 0) F.ram[0][i] = col; else if (d === 1) F.ram[1][i] = col;
    else if (d === 2) F.gpu[i] = col; else if (d === 3) F.board = col;
    else if (d === 4) F.bulbs[i] = col; else if (d === 5) F.nano[i] = col;
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
  const groups = [[...F.ram[0], ...F.ram[1]], [...F.gpu, F.board], F.nano, [...F.bulbs, ...Object.values(F.ext).flat()]];
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
$('#nano-flip').addEventListener('click', () => setCfg('nanoleaf', 'flip', cv('nanoleaf', 'flip', '0') === '1' ? 0 : 1));
function rotateNano(d) { setCfg('nanoleaf', 'rotate', ((+cv('nanoleaf', 'rotate', 0) + d) % 360 + 360) % 360); }
$('#nano-rate').addEventListener('input', e => { $('#nano-rate-val').textContent = e.target.value + t('per.s'); setCfgSoon('nanoleaf', 'rate', e.target.value); });
function updateNano() {
  const n = S.nano || {};
  $('#nano-empty').classList.toggle('hidden', !!n.configured);
  $('#nano-main').classList.toggle('hidden', !n.configured);
  $('#nano-name').textContent = n.name || 'Nanoleaf';
  $('#nano-status').innerHTML = n.online ? `<span class="dot on" style="display:inline-block;margin-right:6px"></span>${t('nano.online', n.ip, (n.panels || []).length)}`
    : `<span class="dot off" style="display:inline-block;margin-right:6px"></span>${t('nano.offline')}${n.ip ? ' · ' + n.ip : ''}`;
  $$('[data-pair-msg]').forEach(p => p.textContent = n.pair ? t('pair.' + n.pair) : '');
  $$('[data-pair]').forEach(b => b.disabled = n.pair === 1 || n.pair === 2);
  const r = +cv('nanoleaf', 'rate', 10); setRange($('#nano-rate'), r); $('#nano-rate-val').textContent = r + t('per.s');
}

// bulbs
function buildBulbs() {
  const grid = $('#bulb-grid');
  grid.innerHTML = '';
  $('#bulbs-empty').classList.toggle('hidden', S.bulbs.length > 0);
  S.bulbs.forEach((b, i) => {
    const d = document.createElement('div');
    d.className = 'card';
    const short = (b.name.split(' ').pop() || b.name);
    d.innerHTML = `<div class="bulb-top"><div class="bulb-orb off" id="bulb-orb-${i}"></div>
      <div><h3>${t('bulb', i + 1)}</h3><p class="muted" id="bulb-st-${i}"></p></div></div>
      <div class="zone" data-zone="zone.light${i + 1}"></div>`;
    d.title = b.name;
    grid.appendChild(d);
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

// LAN / bridge devices
let devSig = '';
const kindTitle = k => (S.ext.kinds.find(x => x.kind === k) || { title: k }).title;
function devStatus(d) {
  if (!d.enabled) return t('dev.off');
  if (!d.online) return d.info && /button|reach|forgot|colour/i.test(d.info) ? d.info : t('dev.offline');
  return t(d.per_led ? 'dev.online' : 'dev.online.lights', d.leds);
}
function buildDevices() {
  const grid = $('#dev-grid');
  grid.innerHTML = '';
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
        ${d.per_led ? `<div class="stepper"><span>${t('leds')}</span><button data-d="-1">−</button><b class="dev-leds">${d.leds}</b><button data-d="1">+</button></div>
        <label class="check"><input type="checkbox" class="dev-rev" ${cv(sec, 'reverse', '0') === '1' ? 'checked' : ''}><span></span><em>${t('reverse')}</em></label>` : ''}
        <button class="btn danger small dev-del">${t('dev.remove')}</button>
      </div>`;
    grid.appendChild(el);
    renderZone(el.querySelector('.zone'));
    el.querySelector('.dev-on').addEventListener('change', e => { setCfg(sec, 'enabled', e.target.checked ? 1 : 0); });
    el.querySelector('.dev-name').addEventListener('change', e => {
      const v = e.target.value.replace(/[;#\[\]=]/g, '').trim(); if (!v) return;
      setCfg(sec, 'name', v); el.querySelector('h3').textContent = v; d.name = v; renderOwnList();
    });
    el.querySelector('.dev-rev')?.addEventListener('change', e => setCfg(sec, 'reverse', e.target.checked ? 1 : 0));
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
function updateDevices() {
  const E = S.ext;
  E.devs.forEach(d => {
    const el = $('#dev-st-' + d.id); if (!el) return;
    el.innerHTML = `<span class="dot ${d.enabled && d.online ? 'on' : 'off'}" style="display:inline-block;margin-right:6px"></span>${esc(devStatus(d))}`;
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
  if (ks.options.length !== E.kinds.length) ks.innerHTML = E.kinds.map(k => `<option value="${k.kind}">${esc(k.title)}</option>`).join('');
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
  const n = S.nano || {};
  $('#wz-nano').textContent = n.configured ? t('wz.nano.on', n.name || n.ip || '') : n.pair ? '' : t('wz.nano.off');
  $('#wizard [data-pair]').classList.toggle('hidden', !!n.configured);
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
$('#hotspot-auto').addEventListener('change', e => setCfg('hotspot', 'auto', e.target.checked ? 1 : 0));
$('#fps').addEventListener('input', e => { $('#fps-val').textContent = e.target.value; setCfgSoon('general', 'fps', e.target.value); });
$$('[data-open]').forEach(b => b.addEventListener('click', () => send({ cmd: 'open', what: b.dataset.open })));
$('#quit').addEventListener('click', () => send({ cmd: 'quit' }));

function buildHotkeys() {
  $('#hotkeys').innerHTML = HOTKEYS.map(k => `<div class="hk"><span>${t('hk.' + k)}</span><button class="kbd" data-hk="${k}"></button></div>`).join('');
  $$('[data-hk]').forEach(b => {
    const show = () => { const v = cv('hotkeys', b.dataset.hk, ''); b.textContent = v ? v.replace(/\+/g, ' + ') : t('hk.none'); b.classList.toggle('empty', !v); };
    show();
    b.addEventListener('click', () => {
      $$('.kbd.rec').forEach(x => x !== b && x.blur());
      b.classList.add('rec'); b.textContent = t('hk.press');
      const onKey = e => {
        e.preventDefault(); e.stopPropagation();
        if (e.key === 'Escape') return done();
        if (e.key === 'Backspace') { setCfg('hotkeys', b.dataset.hk, ''); return done(); }
        const map = { ArrowLeft: 'Left', ArrowRight: 'Right', ArrowUp: 'Up', ArrowDown: 'Down', PageUp: 'PageUp', PageDown: 'PageDown', Home: 'Home', End: 'End' };
        let key = map[e.key] || (/^F\d{1,2}$/.test(e.key) ? e.key : '');
        if (!key && /^(Key|Digit)[A-Z0-9]$/.test(e.code)) key = e.code.slice(-1);
        if (!key) return;   // modifier alone: keep waiting
        const mods = [e.ctrlKey && 'Ctrl', e.altKey && 'Alt', e.shiftKey && 'Shift', e.metaKey && 'Win'].filter(Boolean);
        if (!mods.length && !/^F\d/.test(key)) return;
        setCfg('hotkeys', b.dataset.hk, [...mods, key].join('+'));
        done();
      };
      const done = () => { document.removeEventListener('keydown', onKey, true); b.classList.remove('rec'); show(); };
      document.addEventListener('keydown', onKey, true);
      b.addEventListener('blur', done, { once: true });
    });
  });
}

let qrUrl = '';   // the address the QR code points at (the PC may be on several networks)
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
  const on = cv('general', 'update_check', '1') !== '0';
  $('#upd-on').checked = on;
  $('#upd-status').textContent = U.latest ? t('upd.avail', U.latest) : t('upd.note');
  $('#upd-get').classList.toggle('hidden', !U.latest);
  $('#upd-get').textContent = t('upd.get', U.latest || '');
}
$('#upd-on').addEventListener('change', e => { setCfg('general', 'update_check', e.target.checked ? 1 : 0); updateUpdate(); });
$('#upd-get').addEventListener('click', () => send({ cmd: 'open', what: 'release' }));
$('#upd-now').addEventListener('click', () => send({ cmd: 'update_check' }));

function updateSettings() {
  updateRemote();
  updateUpdate();
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
  applyI18n(); buildEffects(); buildHotkeys(); buildBulbs(); buildDevices(); renderEffectSide();
  showTab(tab); updateNano(); updateChips(); updateSettings(); drawAll();
  if (!$('#wizard').classList.contains('hidden')) wzGo(wzStep);
}));

// ------------------------------------------------------------------ status chips
function chip(dot, text) { return `<span class="chip"><span class="dot ${dot}"></span>${text}</span>`; }
function updateChips() {
  const h = [];
  h.push(chip(S.msi ? 'on' : 'off', `${t('chip.board')} <b>${S.msi ? 'MSI' : t('chip.offline')}</b>`));
  h.push(chip(S.sticks ? 'on' : 'off', `${t('chip.mem')} <b>${S.sticks || 0} ${t('chip.pcs')}</b>`));
  if (S.gpu_temp != null) h.push(chip('on', `GPU <b>${S.gpu_temp}°</b>`));
  if (S.nano && S.nano.configured) h.push(chip(S.nano.online ? 'on' : 'off', `<b>Nanoleaf</b>`));
  if (S.bulbs.length) { const on = S.bulbs.filter(b => b.online).length; h.push(chip(on === S.bulbs.length ? 'on' : on ? 'warn' : 'off', `${t('chip.lamps')} <b>${on}/${S.bulbs.length}</b>`)); }
  const ed = S.ext.devs.filter(d => d.enabled);
  if (ed.length) { const on = ed.filter(d => d.online).length; h.push(chip(on === ed.length ? 'on' : on ? 'warn' : 'off', `${t('chip.devs')} <b>${on}/${ed.length}</b>`)); }
  $('#chips').innerHTML = h.join('');
  $('#ram-status').textContent = S.sticks ? t('ram.status', S.sticks) : t('ram.none');
  $('#gpu-status').textContent = S.msi ? t('gpu.status') : t('gpu.none');
  $('#strip-title').textContent = stripName();
  const sn = $('#strip-name'); if (document.activeElement !== sn) sn.value = cv('layout', 'strip_name', '');
  sn.placeholder = t('pc.block');
}

// ------------------------------------------------------------------ messages from the core
function applyStatus(m) {
  const bulbCountChanged = (m.bulbs || []).length !== S.bulbs.length;
  const nanoLayoutChanged = JSON.stringify((m.nano || {}).panels) !== JSON.stringify((S.nano || {}).panels);
  const effectChanged = m.effect !== S.effect;
  Object.assign(S, m);
  S.ext = S.ext || { devs: [], found: [], kinds: [], scanning: 0 };
  const sig = JSON.stringify(S.ext.devs.map(d => [d.id, d.leds, d.enabled, d.per_led, d.name]));
  if (!dragging) { setRange($('#bright'), S.brightness); $('#bright-val').textContent = S.brightness + '%'; }
  if (effectChanged) { markEffect(); renderEffectSide(); }
  if (bulbCountChanged) buildBulbs(); else updateBulbs();
  if (sig !== devSig) { devSig = sig; buildDevices(); drawAll(); } else updateDevices();
  updateNano(); updateChips(); updateSettings(); updateWizard();
  if (nanoLayoutChanged) drawAll();
}

if (wv) wv.addEventListener('message', e => {
  const m = e.data;
  if (m.type === 'frame') onFrame(m.l);
  else if (m.type === 'status') applyStatus(m);
  else if (m.type === 'state') {
    S.cfg = m.cfg || {}; S.effects = m.effects || []; S.autostart = m.autostart;
    LANG = cv('general', 'lang', 'en') === 'ru' ? 'ru' : 'en';
    applyI18n();
    buildEffects(); buildHotkeys(); syncToggles();
    S.effect = null;           // force a full refresh
    S.bulbs = [];
    devSig = '';
    applyStatus(m);
    renderEffectSide();
    sizeCanvases();
    if (!wzShown && cv('general', 'welcome', '0') === '1') wizard(true);
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
try { const tb = localStorage.getItem('tab'); showTab(TABS.includes(tb) ? tb : 'effects'); } catch (e) { showTab('effects'); }
$$('.range').forEach(fill);
send({ cmd: 'hello' });
