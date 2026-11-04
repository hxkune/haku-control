// SPDX-License-Identifier: GPL-3.0-only
// The effect previews: src/effects.c led_color() for one addressable strip (an external device's zone), ported line
// by line so a card shows what the LEDs will do. tools/sim/preview_check.js checks it against the C engine and
// FX_PARAMS against its FXP table; change the two together. No DOM here, so node can load it.
// Colours are 0..255 (the C works in 0..1; every step is linear until the final clamp, so it is the same picture).

// Effect settings the window shows: [key, default, min, max, opts] as effects.c FXP.
// opts: u unit ('s', 'ms', '%', '°'), a under Advanced, o n choices (fxp.<id>.<key>.<n>), b on/off,
// when [key, value] shown only then, st slider step, zero the label for 0
var FX_PARAMS = {
  breathe: [['cycle', 0, 0, 30, { u: 's', zero: 'fxp.byspeed' }], ['depth', 50, 0, 100, { u: '%' }], ['single', 0, 0, 1, { b: 1 }],
            ['wave', 0, 0, 100, { u: '%', a: 1 }]],
  pendulum: [['motion', 0, 0, 1, { o: 2 }], ['width', 18, 3, 60, { u: '%' }], ['tail', 25, 0, 100, { u: '%' }], ['mirror', 0, 0, 1, { b: 1 }],
             ['axis', 0, 0, 2, { o: 3, a: 1 }], ['soft', 80, 0, 100, { u: '%', a: 1 }], ['bg', 4, 0, 30, { u: '%', a: 1 }]],
  twinkle: [['density', 35, 5, 100, { u: '%' }], ['life', 3, 1, 10, { u: 's' }], ['halo', 30, 0, 100, { u: '%', a: 1 }],
            ['variety', 100, 0, 100, { u: '%', a: 1 }], ['bg', 3, 0, 20, { u: '%', a: 1 }]],
  meteor: [['rate', 4, 1, 20, {}], ['tail', 35, 5, 60, { u: '%' }], ['dir', 0, 0, 2, { o: 3 }], ['spread', 40, 0, 100, { u: '%', a: 1 }],
           ['bg', 2, 0, 20, { u: '%', a: 1 }]],
  interference: [['scale', 8, 2, 20, {}], ['contrast', 60, 0, 100, { u: '%' }], ['sources', 2, 2, 3, { a: 1 }], ['angle', 0, 0, 360, { u: '°', a: 1, st: 15 }]],
  aurora: [['layers', 2, 1, 3, {}]],
  steps: [['segments', 6, 2, 16, {}], ['pause', 1, 0, 5, { u: 's', st: 0.5 }], ['dir', 0, 0, 2, { o: 3 }], ['hard', 0, 0, 1, { b: 1 }]],
  fire: [['height', 75, 30, 100, { u: '%' }]],
  candle: [['flicker', 50, 0, 100, { u: '%' }]],
  matrix: [['style', 0, 0, 1, { o: 2 }]],
  audio: [['mode', 0, 0, 1, { o: 2 }], ['sens', 50, 0, 100, { u: '%' }], ['dir', 0, 0, 3, { o: 4, when: ['mode', 0] }], ['peak', 1, 0, 1, { b: 1, when: ['mode', 0] }],
          ['decay', 350, 100, 1000, { u: 'ms', a: 1, st: 50, when: ['mode', 1] }], ['gap', 250, 100, 800, { u: 'ms', a: 1, st: 50, when: ['mode', 1] }],
          ['shape', 0, 0, 1, { o: 2, a: 1, when: ['mode', 1] }]],
};
// a saved value as the core reads it: a number, clamped; anything else is the default
function fxParamValue(id, key, raw) {
  const d = (FX_PARAMS[id] || []).find(p => p[0] === key); if (!d) return raw;
  const v = raw === '' || raw == null ? NaN : +raw;
  return isFinite(v) ? Math.min(d[3], Math.max(d[2], v)) : d[1];
}
// the clock speed a card runs at: effects.c load_params
function fxClockSpeed(id, speed, P) {
  if (id === 'breathe' && P('cycle') >= 1) return 1 / (0.35 * P('cycle'));
  const sp = +speed / 5; return sp >= 0 ? Math.min(sp, 4) : 1;   // (also not a number)
}

var fxPreview = (function () {
  const fract = x => x - Math.floor(x), clamp = (v, a, b) => v < a ? a : v > b ? b : v;
  const smooth01 = x => { x = clamp(x, 0, 1); return x * x * (3 - 2 * x); };
  const mix = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
  const scale = (a, k) => [a[0] * k, a[1] * k, a[2] * k];
  const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
  const wrapi = (k, n) => { k %= n; return k < 0 ? k + n : k; };
  // unsigned 32-bit arithmetic as in C
  const h32 = x => { x >>>= 0; x ^= x >>> 16; x = Math.imul(x, 0x7feb352d); x ^= x >>> 15; x = Math.imul(x, 0x846ca68b); x ^= x >>> 16; return x >>> 0; };
  const hashf = x => (h32(x) & 0xFFFFFF) / 16777216;
  const vnoise = (x, seed) => {
    const i = Math.floor(x), f = x - i, k = i | 0;
    const a = hashf(Math.imul(k, 0x9E3779B1) ^ seed), b = hashf(Math.imul(k + 1, 0x9E3779B1) ^ seed);
    return a + (b - a) * smooth01(f);
  };
  const vnoise2 = (x, y, seed) => { const j = Math.floor(y), f = y - j, k = j | 0; return vnoise(x, seed ^ h32(k)) * (1 - smooth01(f)) + vnoise(x, seed ^ h32(k + 1)) * smooth01(f); };
  const hsv = (h, s, v) => {
    h = fract(h) * 6; const i = Math.floor(h), f = h - i, p = v * (1 - s), q = v * (1 - s * f), u = v * (1 - s * (1 - f));
    return [[v, u, p], [q, v, p], [p, v, u], [p, q, v], [u, p, v], [v, p, q]][i % 6].map(k => k * 255);
  };
  const causticN = (x, y, t) => { const n = 0.5 + 0.5 * Math.sin(x * 13 + t * 1.7) * Math.sin(y * 9 - t * 1.3 + Math.sin(x * 5 + t)); return n * n; };
  const star = (k, t, life, dens) => {
    const q = t / (life * (0.7 + 0.6 * hashf(k ^ 0x51))) + hashf(k ^ 0x93), f = q - Math.floor(q);
    const id = h32(k ^ Math.imul(Math.floor(q) | 0, 0x9E3779B1)), hue = hashf(id ^ 0xC0);
    return [hashf(id) >= dens ? 0 : smooth01(f / 0.25) * smooth01((1 - f) / 0.25), hue];
  };
  // the strip: DEV_EXT (6) in zone ZONE_EXT0 (18), y in the middle; it is flat
  const DEV = 6, ZONE = 18, KEY0 = DEV * 100003 + ZONE * 1009;

  // demo signals for the effects that follow a reading (the card says "demo"): a level that rises and falls, a beat
  // every half second, a temperature going slowly from cold to hot
  function audioDemo(t) {
    const lv = s => Math.abs(Math.sin(s * 2.3) * Math.sin(s * 0.9 + 1));
    let peak = 0; for (let j = 0; j < 20; j++) peak = Math.max(peak, lv(t - j * 0.06) - j * 0.03);
    return { level: lv(t), peak, bass: 0.5 + 0.5 * Math.sin(t * 1.7), age: fract(t / 0.5) * 0.5, n: Math.floor(t / 0.5) };
  }

  return function preview(id, pal, t, n, P) {
    P = P || (k => fxParamValue(id, k, null));
    const np = pal.length, out = [];
    if (!np) return out;
    const palc = p => { p = fract(p) * np; const i = Math.floor(p); return mix(pal[i % np], pal[(i + 1) % np], p - i); };
    const grad = p => { if (np === 1) return pal[0]; p = clamp(p, 0, 1) * (np - 1); const i = Math.floor(p); return i >= np - 1 ? pal[np - 1] : mix(pal[i], pal[i + 1], p - i); };
    const one = n === 1, y = 0.5, ad = id === 'audio' ? audioDemo(t) : null;
    for (let i = 0; i < n; i++) {
      const x = n > 1 ? i / (n - 1) : 0, path = x, fill = x, key = h32(KEY0 + i);
      let c = [0, 0, 0];
      switch (id) {
        case 'flow': case 'pump': c = palc(path - t * 0.12); break;
        case 'caustic': c = scale(palc(x * 0.6 + t * 0.03), 0.25 + 0.75 * causticN(x, y, t)); break;
        case 'bubbles': {   // shortcut: the core's bubbles are a live list (they rise and pop at random); these four only look alike
          c = scale(pal[0], 0.12);
          for (let k = 0; k < 4; k++) { const pos = fract(t * 0.18 + k * 0.29) * 1.3 - 0.15, d = (pos - x) / 0.06; c = add(c, scale(palc(0.33 + k * 0.21), Math.exp(-d * d))); }
          break;
        }
        case 'comet': {
          const head = fract(t * 0.2) * 1.35, d = head - path;
          c = scale(pal[np > 2 ? 2 : np - 1], 0.05);
          if (d >= 0 && d < 0.3) { const k = 1 - d / 0.3; c = add(c, scale(mix(pal[0], pal[np > 1 ? 1 : 0], d / 0.3), k * k)); }
          break;
        }
        case 'lava': { const v = Math.sin(x * 6 + t * 0.8) + Math.sin(y * 5 - t * 0.6) + Math.sin((x + y) * 4 + t * 0.4); c = palc(v / 6 + t * 0.02); break; }
        case 'breathe': {
          const mn = 1 - P('depth') / 100, w = 0.2 + 0.8 * mn, ph = t * 0.35 - P('wave') / 100 * path, cp = ph + 0.5, k = Math.floor(cp);
          c = P('single') >= 0.5 ? pal[0] : mix(pal[wrapi(k, np)], pal[wrapi(k + 1, np)], smooth01((cp - k - 0.5) / w + 0.5));
          c = scale(c, mn + (1 - mn) * 0.5 * (1 - Math.cos(6.2831853 * fract(ph))));
          break;
        }
        case 'temperature': c = scale(grad(0.5 + 0.5 * Math.sin(t * 0.25)), 0.8 + 0.2 * causticN(x, y, t * 0.5)); break;   // demo reading
        case 'audio': {     // the core's algorithm on the demo signal; a strip of an external device fills from the middle
          const dm = P('dir'), mid = dm === 3 || dm === 0, pos = mid ? Math.abs(fill - 0.5) * 2 : dm === 2 ? 1 - fill : fill, lv = clamp(ad.level * 1.15, 0, 1);
          if (P('mode') >= 0.5) {
            const pulse = Math.exp(-ad.age / (P('decay') / 1000));
            let b = pulse;
            if (P('shape') >= 0.5 && !one) b = pulse * smooth01(1 - Math.abs(Math.abs(fill - 0.5) * 2 - (1 - pulse) * 1.2) / 0.3);
            c = scale(pal[ad.n % np], 0.06 + 0.94 * b);
          } else if (one) c = scale(grad(lv), 0.08 + 0.92 * lv);
          else {
            const lit = smooth01((lv - pos) / 0.12 + 0.5) * smooth01(lv / 0.06);
            c = scale(grad(pos * 0.85 + ad.bass * 0.15), 0.08 + 0.92 * lit * (0.7 + 0.3 * ad.bass));
            const pk = clamp(ad.peak * 1.15, 0, 1), d = Math.abs(pos - pk) / 0.04;
            if (P('peak') >= 0.5 && pk > 0.05 && d < 1) c = add(c, scale(pal[np - 1], (1 - d) * (1 - d)));
          }
          break;
        }
        case 'rainbow': c = hsv(path * 0.9 - t * 0.15, 1, 1); break;
        case 'wave': {      // wave keeps its own [wave] dir and size; on a strip up and down are the middle row
          const d = P('dir') || 'right', b = clamp(+P('size') || 2, 1, 4);
          let u = d === 'left' ? 1 - x : d === 'down' ? y : d === 'up' ? 1 - y : d === 'out' || d === 'in' ? Math.abs(x - 0.5) * 1.4 : x;
          if (d === 'in') u = 1 - u;
          c = palc(u * b - t * 0.18);
          break;
        }
        case 'fire': {
          const nz = vnoise2(x * 7 + vnoise(t * 0.7, 11) * 2, y * 5 + t * 1.9, 3);
          c = grad(clamp(0.85 - (1 - y) * 0.85 * 75 / P('height') + (nz - 0.5) * 0.75 + 0.12 * vnoise(t * 9 + x * 13, key & 1023), 0, 1));
          break;
        }
        case 'ocean': {
          const u = path + y * 0.35;
          const w = 0.4 * Math.sin(u * 9 + t * 0.71) + 0.3 * Math.sin(u * 13 - t * 1.13 + 1.3) + 0.2 * Math.sin(u * 21 + t * 1.7 + 2.1) + 0.1 * Math.sin(u * 31 - t * 2.3), v = 0.5 + 0.5 * w;
          c = grad(v * 0.85); if (v > 0.8) c = add(c, scale(pal[np - 1], (v - 0.8) * 2.5));
          break;
        }
        case 'twinkle': {
          const life = P('life'), dens = one ? 1 : P('density') / 100, vr = P('variety') / 100, halo = P('halo') / 100;
          const [b, hue] = star(key, t, life, dens);
          c = add(scale(pal[np - 1], P('bg') / 100), scale(palc(hue * vr), b));
          for (let o = -2; halo > 0 && !one && o <= 2; o++) {
            if (!o) continue;
            const [nb, h2] = star(h32(KEY0 + i + o), t, life, dens);
            if (nb > 0) c = add(c, scale(palc(h2 * vr), nb * halo * (o === 1 || o === -1 ? 0.35 : 0.12)));
          }
          break;
        }
        case 'meteor': {
          c = scale(pal[np - 1], P('bg') / 100);
          const tail = P('tail') / 100, spread = P('spread') / 100, dir = P('dir');
          const slot = Math.max(40 / P('rate'), (1.15 + tail) / (0.45 * (1 - 0.5 * spread)));
          for (let m = 0; m < 4; m++) {
            const q = t / slot + m * 0.25, qs = Math.floor(q), mid = h32((qs | 0) * 4 + m + 0x3A1);
            const v = 0.45 * (1 + spread * (hashf(mid) - 0.5)), start = hashf(mid ^ 5) * (slot - (1.15 + tail) / v);
            const u = dir === 1 || (dir === 2 && (mid & 0x100)) ? 1 - path : path, d = ((q - qs) * slot - start) * v - 0.05 - u;
            if (d < -0.02 || d > tail) continue;
            const k = d < 0 ? 1 + d / 0.02 : 1 - d / tail;
            c = add(c, scale(mix(pal[0], pal[np > 1 ? 1 : 0], Math.max(d, 0) / tail), k * k));
          }
          break;
        }
        case 'plasma': {
          const v = Math.sin(x * 10 + t) + Math.sin(y * 8 - t * 1.3) + Math.sin((x + y) * 7 + t * 0.7) + Math.sin(Math.sqrt((x - 0.5) ** 2 + (y - 0.5) ** 2) * 14 - t * 1.6);
          c = palc(v * 0.125 + 0.5 + t * 0.04);
          break;
        }
        case 'aurora': {
          const nl = P('layers'), u = path, tt = t * 0.6;
          let acc = [0, 0, 0];
          for (let k = 0; k < nl; k++) {
            const sp = 1 - k * 0.3;
            const wave = Math.sin(u * (6 + 2.5 * k) + tt * 0.5 * sp + k * 1.9 + 2 * Math.sin(u * (2.3 + 0.6 * k) - tt * 0.3 * sp));
            let cu = smooth01((wave + 1) * 0.5); cu *= cu;
            const sh = 0.85 + 0.15 * Math.sin(u * 23 + tt * 1.1 + k * 2.3);
            acc = add(acc, scale(grad(clamp(0.5 + 0.5 * Math.sin(u * 2 + tt * 0.2 + y * 1.5 + k * 2.1), 0, 1)), cu * sh * (k ? 0.6 : 1)));
          }
          c = add(scale(pal[0], 0.03), scale(acc, (0.35 + 0.65 * (1 - y)) / (1 + 0.2 * (nl - 1))));
          break;
        }
        case 'ripple': {
          c = scale(pal[0], 0.03);
          for (let k = 0; k < 4; k++) {
            const tt = t * 0.45 + k * 0.25, slot = Math.floor(tt), age = tt - slot, rid = ((slot | 0) * 4 + k) >>> 0;
            const cx = hashf(Math.imul(rid, 2654435761)), cy = hashf(Math.imul(rid, 2654435761) ^ 0x1234), d = Math.sqrt((x - cx) ** 2 + (y - cy) ** 2);
            const e = (d - age * 0.9) / 0.06;
            c = add(c, scale(palc(hashf(rid ^ 0x77)), Math.exp(-e * e) * (1 - age) * (1 - age)));
          }
          break;
        }
        case 'matrix': {
          const col = Math.floor(x * 24) >>> 0, sp = 0.35 + hashf(col * 31) * 0.5, head = fract(t * sp * 0.6 + hashf(col * 17)) * 1.6 - 0.3, dist = head - y;
          c = scale(pal[0], 0.01);
          if (P('style') >= 0.5) {
            const q = t * sp * 0.6 + hashf(col * 17), qs = Math.floor(q), rid = h32((col * 31) ^ Math.imul(qs | 0, 0x9E3779B1)), dd = (q - qs) * 1.6 - 0.3 - y;
            if (hashf(rid) < 0.65 && dd >= 0 && dd < 0.25) { const b = 1 - dd / 0.25; c = scale(palc(hashf(rid ^ 0xD1)), b * b); }
          } else if (dist >= 0 && dist < 0.45) {
            const b = (1 - dist / 0.45) ** 2, flick = 0.7 + 0.3 * hashf(key ^ Math.imul(Math.floor(t * 15) | 0, 2246822519));
            c = scale(dist < 0.03 && np > 1 ? pal[1] : pal[0], b * flick);
          }
          break;
        }
        case 'candle': {
          const zs = ZONE * 13, fl = P('flicker') / 100, f = vnoise(t * 5, zs + 1) * 0.6 + vnoise(t * 9, zs + 2) * 0.4;
          const dip = 1 - 0.8 * fl * smooth01((0.22 - vnoise(t * 1.3, zs + 5)) / 0.16);
          c = scale(mix(pal[0], pal[np > 1 ? 1 : 0], vnoise(t * 2, zs + 9)), (1 - 0.9 * fl + 0.9 * fl * f) * dip * (0.93 + 0.07 * hashf(key)));
          break;
        }
        case 'pendulum': {  // a strip is flat: the band runs along it whatever the axis
          const bounce = P('motion') >= 0.5;
          let u = path; if (P('mirror') >= 0.5) u = Math.abs(2 * u - 1);
          const ph = fract(t * 0.25), pos = bounce ? 1 - Math.abs(2 * ph - 1) : 0.5 - 0.5 * Math.cos(6.2831853 * ph);
          const half = P('width') / 200, soft = P('soft') / 100, d = Math.abs(u - pos);
          const band = 1 - smooth01((d - half * (1 - soft)) / Math.max(2 * half * soft, 0.004));
          const tail = P('tail') / 200; let tr = 0;
          if (tail > 0) {
            const p1 = bounce ? u / 2 : Math.acos(clamp(1 - 2 * u, -1, 1)) / 6.2831853, ago = Math.min(fract(ph - p1), fract(ph + p1));
            if (ago < tail) { tr = 1 - ago / tail; tr *= tr * 0.7; }
          }
          c = scale(grad(u), P('bg') / 100 + (1 - P('bg') / 100) * Math.max(band, tr));
          break;
        }
        case 'interference': {   // a strip is flat: the two waves along it (sources and angle are for 2D layouts)
          const k = 6.2831853 * P('scale'), v = 0.5 * (Math.sin(k * path - t * 2) + Math.sin(k * 1.13 * path + t * 1.7));
          const m = clamp(0.5 + 0.5 * v * (0.5 + 1.5 * P('contrast') / 100), 0, 1);
          c = scale(grad(m), 0.12 + 0.88 * m);
          break;
        }
        case 'steps': {
          const F = Math.fround, sg = P('segments'), dir = P('dir'), fp = F(path);   // in float, as C: a LED right on a segment edge falls the same way
          const u = dir === 1 ? F(1 - fp) : dir === 2 ? F(Math.abs(F(2 * fp) - 1)) : fp;
          const cyc = sg * 0.5 + P('pause'), q = t / cyc, qs = Math.floor(q), lit = (q - qs) * cyc / 0.5, g = Math.min(Math.floor(F(u * sg)), sg - 1);
          const f = P('hard') >= 0.5 ? (lit > g + 0.5 ? 1 : 0) : smooth01(lit - g), k = qs | 0, dark = scale(pal[0], 0.04);
          const a = np > 1 ? pal[wrapi(k, np)] : (k & 1) ? pal[0] : dark, b = np > 1 ? pal[wrapi(k + 1, np)] : (k & 1) ? dark : pal[0];
          c = mix(a, b, f);
          break;
        }
        case 'screen': {   // shortcut: a made-up moving picture, seen along the edge (the real one is the screen)
          const v = 0.5 + 0.5 * Math.sin(x * 5 + t * 0.9) * Math.sin(t * 0.37 + x * 2);
          c = hsv(0.58 + 0.25 * Math.sin(t * 0.21 + x * 1.5), 0.55, 0.25 + 0.6 * v);
          break;
        }
        case 'static': c = pal[0]; break;
        case 'off': break;
        default: c = [14, 15, 19];
      }
      out.push(c.map(v => v > 0 ? (v < 255 ? v : 255) : 0));   // and a NaN becomes 0
    }
    return out;
  };
})();
