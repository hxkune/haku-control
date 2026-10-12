// SPDX-License-Identifier: GPL-3.0-only
'use strict';
// The haku intro: plays when the window opens for the first time after haku starts (the core opens the page as
// index.html#intro). The wordmark (haku.svg) is cut into its pieces: the letters h, a, k, u, the orbit and the
// three arcs inside it. Ribbons of light in the colours of the current effect whirl in out of the dark and wind
// down into the orbit, which unrolls as it turns in its own plane; the arcs come round the same way and the letters
// fly in along curves, each on a spring. Everything that moves is drawn with motion blur (sub-frames over the
// shutter) and its red and cyan apart along the motion. The letters land with a short glitch, the logo flashes, a
// ring runs out along the orbit, sparks fly; a light sweeps across it, then it flies into its place in the sidebar
// while the window comes in. A click or a key skips it; without animations (Settings) it does not play.
// The whole film is a function of time: index.html?mock#intro@1.2 stands still at 1.2 s, to look at a frame.
(() => {
  const root = document.documentElement;
  if (!root.classList.contains('intro')) return;
  const hashT = () => (/^#intro@([\d.]+)$/.exec(location.hash) || [])[1];
  let FREEZE = hashT();   // then #intro@<s> again moves to another moment
  if (FREEZE) addEventListener('hashchange', () => { FREEZE = hashT() || FREEZE; });
  try { history.replaceState(null, '', location.pathname + location.search); } catch (e) { }
  const ov = $('#intro');

  // ---- the logo, in haku.svg's units (viewBox 36 28 1540 903)
  const VB = [36, 28, 1576, 931], C0 = [806, 479.5];
  // the orbit is near an ellipse (fitted to its outer edge): centre, half axes, turn
  const OC = [781.5, 512.8], OA = 773, OB = 265, OR = -.3386;
  // its stroke runs (in the ellipse's own angles) from where it leaves the h, down, round and up into its thin end
  const TH0 = 4.12, TH1 = -1.75;
  // a, u and the upper right arc are outlines of their own (the path's 1 + 5, 3 and 2); the rest is one outline,
  // cut here into the h (with its long stroke), the k (with its spike over the orbit) and the two arcs that grow
  // out of the orbit. What is left of it is the orbit. Where two cross, the crossing stays with the orbit.
  const CUT = {
    h: [[200, 10, 262, 10, 352, 180, 352, 320, 430, 300, 600, 300, 700, 420, 745, 485, 826, 684, 842, 706, 830, 716, 560, 640,
      440, 600, 405, 727, 357, 728, 282, 522, 212, 480, 200, 300], [360, 752, 402, 752, 404, 798, 370, 800], [385, 872, 430, 868, 430, 960, 385, 960]],
    k: [[700, 320, 860, 260, 975, 262, 1050, 190, 1095, 186, 1150, 200, 1135, 230, 1100, 300, 920, 390, 1000, 500, 1100, 605,
      1060, 640, 1010, 640, 860, 470, 858, 705, 800, 705, 720, 480, 700, 330], [1092, 150, 1172, 68, 1182, 72, 1160, 145]],
    swl: [[240, 528, 205, 545, 170, 590, 145, 650, 147, 700, 170, 740, 230, 772, 300, 778, 365, 775, 372, 752, 420, 755, 530, 748,
      530, 712, 405, 722, 357, 728, 282, 522]],
    swb: [[1100, 693, 1143, 647, 1187, 628, 1362, 538, 1385, 545, 1300, 645, 1200, 705, 1090, 705]],
  };
  const CUTS = ['h', 'k', 'swl', 'swb'];

  // ---- timing (s)
  const HIT = .97, SWEEP = 1.35, OUT = 2.3, FLY = .72, SHUT = .016, SPLIT = .01;

  // ---- maths
  const clamp = x => x < 0 ? 0 : x > 1 ? 1 : x, smooth = x => (x = clamp(x), x * x * (3 - 2 * x));
  const outCubic = x => 1 - Math.pow(1 - clamp(x), 3);
  const hash = n => { n = Math.sin(n * 127.1 + 311.7) * 43758.5453; return n - Math.floor(n); };
  // a spring let go at 1 (moving towards 0 at speed v), made to come to rest exactly at `end`
  function spring(t, w, z, end, v = 0) {
    if (t <= 0) return 1;
    if (t >= end) return 0;
    const wd = w * Math.sqrt(1 - z * z);
    return Math.exp(-z * w * t) * (Math.cos(wd * t) + (z * w - v) / wd * Math.sin(wd * t)) * (1 - smooth((t / end - .6) / .4));
  }
  function bezier(x1, y1, x2, y2) {   // CSS cubic-bezier()
    const f = (a, b, s) => 3 * a * s * (1 - s) * (1 - s) + 3 * b * s * s * (1 - s) + s * s * s;
    return x => { let lo = 0, hi = 1, s = x = clamp(x); for (let i = 0; i < 22; i++) { s = (lo + hi) / 2; if (f(x1, x2, s) < x) lo = s; else hi = s; } return f(y1, y2, s); };
  }
  const flyEase = bezier(.7, 0, .18, 1);
  // 2D affine matrices as canvas takes them: [a, b, c, d, e, f]
  const mul = (A, B) => [A[0] * B[0] + A[2] * B[1], A[1] * B[0] + A[3] * B[1], A[0] * B[2] + A[2] * B[3], A[1] * B[2] + A[3] * B[3],
    A[0] * B[4] + A[2] * B[5] + A[4], A[1] * B[4] + A[3] * B[5] + A[5]];
  const chain = (...m) => m.reduce(mul), mv = (M, x, y) => [M[0] * x + M[2] * y + M[4], M[1] * x + M[3] * y + M[5]];
  const tr = (x, y) => [1, 0, 0, 1, x, y], rot = a => [Math.cos(a), Math.sin(a), -Math.sin(a), Math.cos(a), 0, 0], scl = (x, y = x) => [x, 0, 0, y, 0, 0];
  const I = [1, 0, 0, 1, 0, 0], lerpM = (A, B, f) => A.map((v, i) => v + (B[i] - v) * f);
  // the orbit's plane: E takes the unit circle onto the orbit; turning in that plane (and growing in it) is E R E^-1,
  // which is how a ring seen at a slant turns
  const E = chain(tr(OC[0], OC[1]), rot(OR), scl(OA, OB)), EI = chain(scl(1 / OA, 1 / OB), rot(-OR), tr(-OC[0], -OC[1]));
  const inPlane = (phi, k) => chain(E, rot(phi), scl(k), EI);
  const onOrbit = (th, k = 1) => mv(E, k * Math.cos(th), k * Math.sin(th));
  const css = (c, a = 1) => `rgba(${c[0]},${c[1]},${c[2]},${a})`, mix = (c, d, f) => c.map((v, i) => Math.round(v + (d[i] - v) * f));
  const WHITE = [255, 255, 255];

  // ---- the colours: the current effect's, made bright (dark ones left out)
  let PAL;
  function colours() {
    let p = [];
    try { p = effPal(S.effect || cv('general', 'effect', 'flow')); } catch (e) { }
    const bright = list => list.map(h => {
      const n = parseInt(h.slice(1), 16), c = [n >> 16, n >> 8 & 255, n & 255], m = Math.max(...c);
      return m < 90 ? null : c.map(v => Math.round(v * 255 / m));
    }).filter(Boolean);
    let c = bright(p);
    if (c.length < 2) c = bright(DEFAULT_PAL);
    while (c.length < 3) c.push(c[c.length - 2]);
    return c;
  }

  // ---- canvas, sizes
  ov.innerHTML = '<div class="bg"></div><canvas></canvas>';
  const bg = ov.firstChild, cvs = ov.lastChild, ctx = cvs.getContext('2d');
  let W = 0, H = 0, D = 1, L = 1;   // window (css px), device pixels per css px, css px per logo unit
  function size() {
    D = devicePixelRatio || 1; W = innerWidth; H = innerHeight;
    cvs.width = Math.round(W * D); cvs.height = Math.round(H * D);
    L = Math.min(W * .5, H * .6 * 1540 / 903, 780) / 1540;
  }

  // ---- sprites: a piece drawn once at the logo's size, with a red and a cyan copy (together they make white again)
  let K;   // device px per logo unit when the sprites were drawn
  function layer(box, pad, draw) {
    const x = box[0] - pad, y = box[1] - pad, w = box[2] - box[0] + 2 * pad, h = box[3] - box[1] + 2 * pad;
    const c = document.createElement('canvas'); c.width = Math.ceil(w * K); c.height = Math.ceil(h * K);
    const g = c.getContext('2d'); g.setTransform(K, 0, 0, K, -x * K, -y * K); g.fillStyle = '#fff'; draw(g);
    return { c, x, y, w: c.width / K, h: c.height / K };
  }
  function tint(c, col) {
    const o = document.createElement('canvas'); o.width = c.width; o.height = c.height;
    const g = o.getContext('2d'); g.drawImage(c, 0, 0); g.globalCompositeOperation = 'source-in'; g.fillStyle = css(col); g.fillRect(0, 0, o.width, o.height);
    return o;
  }
  const RED = [255, 36, 72], CYAN = [0, 219, 183];
  function glowOf(s, blur, col) {   // blur in css px
    const g = layer([s.x, s.y, s.x + s.w, s.y + s.h], blur * 3 / L, c => { c.filter = `blur(${blur * D}px)`; c.drawImage(s.c, s.x, s.y, s.w, s.h); });
    if (col) g.c = tint(g.c, col);
    return g;
  }

  // ---- the pieces and how each one comes in
  // letters: from `off` (logo units, from where it ends) along a curve bent towards `ctl`, turned by `rot` and
  // sized `sc` at the start; arcs: turned `orb` round the orbit's plane and a bit wider
  const P = [
    { id: 'ring', box: [48, 137, 1552, 871], t0: .15 },
    { id: 'swr', box: [1206, 212, 1401, 389], orb: 2.6, t0: .5 },
    { id: 'swl', box: [169, 522, 506, 752], orb: 2.2, t0: .54 },
    { id: 'swb', box: [1109, 542, 1364, 688], orb: 1.9, t0: .58 },
    { id: 'h', box: [230, 32, 835, 928], piv: [330, 470], off: [-250, -1150], ctl: [-800, -350], rot: -.6, sc: 1.5, t0: .2 },
    { id: 'k', box: [731, 82, 1169, 681], piv: [940, 380], off: [1350, -650], ctl: [350, -950], rot: .55, sc: 1.35, t0: .3 },
    { id: 'a', box: [333, 120, 808, 683], piv: [570, 400], off: [-350, 1150], ctl: [-1050, 350], rot: -1.1, sc: .55, t0: .38 },
    { id: 'u', box: [958, 279, 1330, 555], piv: [1145, 420], off: [550, 1050], ctl: [1200, 450], rot: .8, sc: .6, flip: 1, t0: .46 },
  ];
  const ringHead = t => TH0 - (TH0 - TH1) * outCubic((t - .15) / .8);
  for (const p of P) {
    if (p.piv) p.at = t => {
      // the path's spring overshoots only a little (the letters sit close together), the turn and size wobble more
      let r = spring(t - p.t0, 13, .72, .58, 6);
      const q = spring(t - p.t0 - .015, 11, .55, .58, 4);
      if (r < 0) r *= .5;
      const ox = r * r * p.off[0] + 2 * r * (1 - r) * p.ctl[0], oy = r * r * p.off[1] + 2 * r * (1 - r) * p.ctl[1];
      const s = 1 + (p.sc - 1) * q, sx = p.flip ? Math.cos(Math.PI * .9 * clamp(q)) : 1;
      return chain(tr(p.piv[0] + ox, p.piv[1] + oy), rot(p.rot * q), scl(s * sx, s), tr(-p.piv[0], -p.piv[1]));
    };
    else if (p.orb) p.at = t => { const r = spring(t - p.t0, 10, .7, .45, 4); return inPlane(p.orb * r, 1 + .3 * r); };
    else p.at = t => { const r = spring(t - .15, 7, .8, .9, 7); return inPlane(2.6 * r, 1 + .25 * r); };
    // the moment a letter first reaches its place (it glitches there)
    if (p.piv) for (let s = 0; s < .6; s += .002) if (spring(s, 13, .72, .58, 6) <= .01) { p.land = p.t0 + s; break; }
    const [x0, y0, x1, y1] = p.box;
    p.probe = [[x0, y0], [x1, y0], [x0, y1], [x1, y1], [(x0 + x1) / 2, (y0 + y1) / 2]];
  }

  // ---- the ribbons of light: rings seen almost face on, born small in the middle, thrown wide, then drawn in,
  // tilting into the orbit's plane
  const RIB = [
    { c: 0, t0: 0, th: 1.2, len: 2.4, k: 1.75, tilt: .9, rat: .95, spd: 9 },
    { c: 1, t0: .04, th: 3.6, len: 1.8, k: 1.5, tilt: -.7, rat: .8, spd: 10 },
    { c: 2, t0: .08, th: 5.5, len: 2.0, k: 1.9, tilt: .4, rat: .9, spd: 8.5 },
    { c: -1, t0: .13, th: 2.4, len: 1.4, k: 1.35, tilt: -.3, rat: .7, spd: 11 },
  ];
  function ribbon(r, t) {
    const tau = t - r.t0;
    if (tau <= 0) return null;
    const out = .15 + (r.k - .15) * outCubic(tau / .32), e = smooth((t - .3) / .62);
    const k = out + (1 - out) * e, tilt = OR + r.tilt * (1 - e), rat = OB / OA + (r.rat - OB / OA) * (1 - e);
    const M = chain(tr(OC[0], OC[1]), rot(tilt), scl(OA * k, OA * k * rat));
    return { pt: th => mv(M, Math.cos(th), Math.sin(th)), th: r.th - r.spd * .55 * (1 - Math.exp(-tau / .55)), k,
      a: clamp(tau / .1) * (1 - smooth((t - .82) / .24)) };
  }
  // sparks: thrown off the ribbons' heads, and out of the orbit when the logo lands. Each is known from its start,
  // so any moment can be drawn without the ones before it
  const SPARKS = [];
  function sparks() {
    RIB.forEach((r, i) => {
      for (let j = 0; j < 44; j++) {
        const t0 = r.t0 + .04 + j / 44 * (.86 - r.t0), R = ribbon(r, t0), u = hash(i * 97 + j);
        const a = R.pt(R.th), b = R.pt(R.th + .02), l = Math.hypot(a[0] - b[0], a[1] - b[1]) || 1;
        const sp = 350 + 650 * u, side = (hash(i * 31 + j * 7) - .5) * 260;
        SPARKS.push({ t0, x: a[0], y: a[1], vx: (a[0] - b[0]) / l * sp - (a[1] - b[1]) / l * side, vy: (a[1] - b[1]) / l * sp + (a[0] - b[0]) / l * side,
          life: .3 + .4 * hash(i * 13 + j * 3), c: r.c < 0 ? WHITE : PAL[r.c % PAL.length], w: 2 + 2 * u });
      }
    });
    for (let j = 0; j < 70; j++) {
      const th = TH1 + (TH0 - TH1) * hash(500 + j), k = 1 + (hash(600 + j) - .5) * .12, p = onOrbit(th, k);
      const n = mv(E, Math.cos(th) * 1.3, Math.sin(th) * 1.3), dx = n[0] - p[0], dy = n[1] - p[1], l = Math.hypot(dx, dy) || 1;
      const sp = 450 + 1200 * hash(700 + j) * hash(800 + j);
      SPARKS.push({ t0: HIT + .05 * hash(900 + j), x: p[0], y: p[1], vx: dx / l * sp, vy: dy / l * sp, life: .35 + .45 * hash(1000 + j),
        c: hash(1100 + j) < .4 ? WHITE : PAL[j % PAL.length], w: 2.2 + 2 * hash(1200 + j) });
    }
  }
  // the four-pointed stars: where the orbit's stroke ends, on the tips when the logo lands, then a few twinkling
  const STARS = [
    { x: 640, y: 283, t0: .92, d: .5, R: 70, c: 0 },
    { x: 1172, y: 72, t0: HIT + .02, d: .55, R: 90, c: 1 },
    { x: 236, y: 34, t0: HIT + .07, d: .5, R: 64, c: 2 },
    { x: 418, y: 926, t0: HIT + .1, d: .45, R: 56, c: 0 },
    { x: 1552, y: 290, t0: HIT + .05, d: .5, R: 66, c: 1 },
    { x: 1470, y: 172, t0: 1.32, d: .9, R: 34, c: -1 },
    { x: 140, y: 600, t0: 1.62, d: .9, R: 30, c: -1 },
    { x: 990, y: 905, t0: 1.9, d: .8, R: 28, c: -1 },
  ];

  let whole, wholeR, wholeC, glowW, glowM, glowT, scratch;
  function build() {
    size(); K = L * D; PAL = colours();
    const d = /\sd="([^"]+)"/.exec(SVG)[1], sub = d.match(/M[^M]+/g);
    const path = (...i) => new Path2D(i.map(j => sub[j]).join(''));
    const poly = list => { const q = new Path2D(); for (const a of list) { q.moveTo(a[0], a[1]); for (let i = 2; i < a.length; i += 2) q.lineTo(a[i], a[i + 1]); q.closePath(); } return q; };
    const rest = path(0, 4, 6), own = { a: path(1, 5), u: path(3), swr: path(2) };
    P.forEach((p, i) => {
      p.s = layer(p.box, 3, g => {
        if (own[p.id]) { g.fill(own[p.id], 'evenodd'); return; }
        const n = CUTS.indexOf(p.id);
        if (n >= 0) { g.save(); g.clip(poly(CUT[p.id])); }
        g.fill(rest, 'evenodd');
        if (n >= 0) g.restore();
        g.globalCompositeOperation = 'destination-out';
        for (const id of n >= 0 ? CUTS.slice(0, n) : CUTS) g.fill(poly(CUT[id]));
      });
      p.r = tint(p.s.c, RED); p.cy = tint(p.s.c, CYAN); p.tn = tint(p.s.c, PAL[i % PAL.length]);
    });
    whole = layer(VB, 3, g => g.fill(new Path2D(d), 'evenodd'));
    wholeR = tint(whole.c, RED); wholeC = tint(whole.c, CYAN);
    glowW = glowOf(whole, 16, mix(PAL[0], WHITE, .25)); glowM = glowOf(whole, 8); glowT = glowOf(whole, 3.5);
    scratch = document.createElement('canvas'); scratch.width = glowM.c.width; scratch.height = glowM.c.height;
    sparks();
  }

  // ---- drawing (everything adds light: the canvas is see-through and black is behind it)
  let CAM = I, CAMS = 1;   // logo units -> device px this frame, and its scale
  function put(s, M, a, img = s.c) {
    if (a <= .003) return;
    const m = mul(CAM, M);
    ctx.globalAlpha = Math.min(1, a); ctx.setTransform(m[0], m[1], m[2], m[3], m[4], m[5]);
    ctx.drawImage(img, s.x, s.y, s.w, s.h);
  }
  // a piece over the shutter: as many sub-frames as it moves (a few px apart), red a little behind and cyan a
  // little ahead in time, so its colours part along the motion and come together when it stops
  function moving(p, t, a, clip) {
    const A = mul(CAM, p.at(t)), B = mul(CAM, p.at(t - SHUT));
    let dist = 0;
    for (const [x, y] of p.probe) { const u = mv(A, x, y), v = mv(B, x, y); dist = Math.max(dist, Math.hypot(u[0] - v[0], u[1] - v[1])); }
    const n = Math.max(1, Math.min(22, Math.ceil(dist / (2.5 * D))));
    const one = (img, M, ts, al) => {
      if (clip) { ctx.save(); const m = mul(CAM, M); ctx.setTransform(m[0], m[1], m[2], m[3], m[4], m[5]); ctx.clip(clip(ts)); }
      put(p.s, M, al, img);
      if (clip) ctx.restore();
    };
    // the channels part by their own motion over SPLIT, but never more than a few px
    const f = Math.min(1, 6 * D / Math.max(1e-6, dist * SPLIT / SHUT));
    for (let i = 0; i < n; i++) {
      const ts = n > 1 ? t + SHUT * (.5 - i / (n - 1)) : t, M = p.at(ts);
      if (dist < .4 * D) one(p.s.c, M, ts, a / n);
      else { one(p.r, lerpM(M, p.at(ts - SPLIT), f), ts, a / n); one(p.cy, lerpM(M, p.at(ts + SPLIT), f), ts, a / n); }
    }
    // a fast one leaves a smear of its colour behind it
    if (!clip && dist > 12 * D) {
      const k = Math.min(1, (dist / D - 12) / 40), m = Math.max(4, Math.min(24, Math.ceil(dist * 4.4 / (5 * D))));
      for (let j = 1; j <= m; j++) put(p.s, p.at(t - SHUT / 2 - j * .07 / m), a * k * 1.2 / m * (1 - j / (m + 1)), p.tn);
    }
  }
  // the landing glitch: a few frames in the piece's colour, cut in strips that jump sideways
  function glitch(p, t, a) {
    const f = Math.floor(t * 60), M = p.at(t), s = p.s, n = 5, m = mul(CAM, M);
    put(s, M, a * .45);
    ctx.globalAlpha = a; ctx.setTransform(m[0], m[1], m[2], m[3], m[4], m[5]);
    for (let j = 0; j < n; j++) {
      const dx = (hash(f * 7 + j) - .5) * (hash(f * 3 + j * 5) > .45 ? 46 : 8);
      ctx.drawImage(f % 2 ? p.r : p.tn, 0, j * s.c.height / n, s.c.width, s.c.height / n, s.x + dx, s.y + j * s.h / n, s.w, s.h / n);
    }
  }
  function sector(a0, a1) {   // a wedge from the orbit's centre between two of its angles
    const q = new Path2D(), n = Math.max(2, Math.ceil(Math.abs(a1 - a0) / .06));
    q.moveTo(OC[0], OC[1]);
    for (let i = 0; i <= n; i++) { const o = onOrbit(a0 + (a1 - a0) * i / n, 1.75); q.lineTo(o[0], o[1]); }
    q.closePath(); return q;
  }
  // a stroke of light along pt(angle), its bright head at `th` and its tail behind it, thin and fading
  function streak(pt, th, len, w, col, a) {
    if (a <= .01) return;
    ctx.lineCap = 'round';
    const n = 30; let prev = pt(th + len);
    for (let i = 1; i <= n; i++) {
      const s = i / n, q = pt(th + len * (1 - s)), lw = w * (.12 + .88 * Math.pow(s, 1.5));
      ctx.beginPath(); ctx.moveTo(prev[0], prev[1]); ctx.lineTo(q[0], q[1]);
      ctx.globalAlpha = a * s * s * .26; ctx.strokeStyle = css(col); ctx.lineWidth = lw * 4.5; ctx.stroke();
      ctx.globalAlpha = a * s * s; ctx.strokeStyle = css(mix(col, WHITE, Math.pow(s, 3) * .75)); ctx.lineWidth = lw; ctx.stroke();
      prev = q;
    }
  }
  function star(x, y, R, a, col, spin, waist = .09) {
    if (a <= .01) return;
    const g = ctx.createRadialGradient(x, y, 0, x, y, R);
    g.addColorStop(0, css(col, .5 * a)); g.addColorStop(1, css(col, 0));
    ctx.globalAlpha = 1; ctx.fillStyle = g; ctx.beginPath(); ctx.arc(x, y, R, 0, 6.2832); ctx.fill();
    ctx.globalAlpha = a * .5; ctx.strokeStyle = css(mix(col, WHITE, .5)); ctx.lineWidth = R * .035;   // a flat flare across
    ctx.beginPath(); ctx.moveTo(x - R * 1.8, y); ctx.lineTo(x + R * 1.8, y); ctx.stroke();
    ctx.globalAlpha = a; ctx.fillStyle = '#fff'; ctx.beginPath();
    for (let i = 0; i < 4; i++) {
      const t1 = spin + i * Math.PI / 2, t2 = t1 + Math.PI / 4, t3 = t1 + Math.PI / 2;
      if (!i) ctx.moveTo(x + Math.cos(t1) * R, y + Math.sin(t1) * R);
      ctx.quadraticCurveTo(x + Math.cos(t2) * R * waist, y + Math.sin(t2) * R * waist, x + Math.cos(t3) * R, y + Math.sin(t3) * R);
    }
    ctx.fill();
  }

  // ---- the camera: rolled and pushed in at first, unwinding with the whirl; a kick and a shake on the landing,
  // then a slow push while the logo stands
  function camera(t) {
    const r = spring(t, 4.2, .9, 1.15), h = t - HIT;
    const kick = h > 0 ? .032 * Math.exp(-h * 8) * Math.sin(h * 26) : 0, drift = .022 * smooth((t - HIT) / (OUT - HIT));
    const shake = h > 0 && h < .25 ? 4 * D * Math.exp(-h * 24) : 0, f = Math.floor(t * 60);
    CAMS = 1 + .14 * r + kick + drift;
    return chain(tr(W * D / 2 + shake * (hash(f) - .5) * 2, H * D / 2 + shake * (hash(f + 50) - .5) * 2), rot(.11 * r), scl(L * D * CAMS), tr(-C0[0], -C0[1]));
  }

  function render(t) {
    ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.clearRect(0, 0, cvs.width, cvs.height);
    ctx.globalCompositeOperation = 'lighter';
    if (t >= OUT) { leave(t); return; }
    CAM = camera(t);
    const h = t - HIT;
    // a little of the colour lights the dark behind, more on the landing
    const energy = .35 * clamp(t / .4) * (h < 0 ? 1 : .4) + (h > 0 ? .9 * Math.exp(-h / .25) : 0);
    const cx = W * D / 2, cy = H * D / 2, R0 = Math.max(W, H) * D * .7, gb = ctx.createRadialGradient(cx, cy, 0, cx, cy, R0);
    gb.addColorStop(0, css(PAL[0], .16 * energy)); gb.addColorStop(.5, css(PAL[1], .05 * energy)); gb.addColorStop(1, css(PAL[1], 0));
    ctx.globalAlpha = 1; ctx.fillStyle = gb; ctx.fillRect(0, 0, cvs.width, cvs.height);
    ctx.setTransform(...CAM);
    for (const r of RIB) {
      const R = ribbon(r, t);
      if (R) streak(R.pt, R.th, r.len, 12 * (.6 + .4 * R.k), r.c < 0 ? WHITE : PAL[r.c % PAL.length], R.a);
    }
    // it starts from a point of light in the middle that the ribbons spin out of
    if (t < .42) {
      const q = t / .42, b = Math.sin(Math.PI * Math.pow(q, .6));
      star(OC[0], OC[1], 40 + 340 * outCubic(q), b * .8, mix(PAL[0], WHITE, .4), t * 4, .025);
      star(OC[0], OC[1], 14 + 70 * outCubic(q), b, WHITE, -t * 6 + .4, .06);
    }
    sparkle(t, 0, HIT);
    // the pieces, until they give way to the whole logo (no seams where they were cut)
    const joined = smooth((h - .04) / .2);
    if (joined < 1) for (const p of P) {
      if (t < p.t0) continue;
      const a = (1 - joined) * clamp((t - p.t0) / (p.piv ? .05 : .1));
      if (p.id === 'ring') moving(p, t, a, ts => sector(ringHead(ts), TH0));
      else if (p.land && t > p.land - .01 && t < p.land + .07) glitch(p, t, a);
      else moving(p, t, a);
    }
    // the orbit's stroke: a bright head where it is being drawn, a trail of light behind it
    const hd = ringHead(t), ha = clamp((t - .17) / .1) * (1 - smooth((t - .93) / .1));
    if (ha > 0) {
      const M = P[0].at(t);
      ctx.setTransform(...CAM);
      streak(th => mv(M, ...onOrbit(th)), hd, 1.1, 11, mix(PAL[0], WHITE, .3), ha);
      const o = mv(M, ...onOrbit(hd));
      star(o[0], o[1], 46, ha, PAL[0], t * 9);
    }
    if (joined > 0) {
      // the logo, its red and cyan still a little apart, and glowing
      const sp = (2.6 * Math.exp(-h * 5) + .6) / (L * CAMS), b = joined * (.9 + .1 * Math.exp(-h * 4));
      put(whole, tr(-sp, 0), b, wholeR); put(whole, tr(sp, 0), b, wholeC);
    }
    if (h > 0) {
      put(glowW, I, .8 * Math.exp(-h / .22) + .26 + .05 * Math.sin(t * 5));
      put(glowT, I, .9 * Math.exp(-h / .1) + .16);
      // the ring running out along the orbit
      ctx.setTransform(...CAM);
      for (const [d, k, col] of [[0, .55, PAL[0]], [.05, .32, WHITE]]) {
        const q = (h - d) / .55;
        if (q <= 0 || q >= 1) continue;
        const kk = 1 + k * outCubic(q);
        ctx.globalAlpha = .8 * (1 - q) * (1 - q); ctx.strokeStyle = css(col); ctx.lineWidth = 14 * Math.pow(1 - q, 1.5);
        ctx.beginPath();
        for (let i = 0; i <= 96; i++) { const o = onOrbit(i / 96 * 6.2832, kk); i ? ctx.lineTo(o[0], o[1]) : ctx.moveTo(o[0], o[1]); }
        ctx.stroke();
      }
    }
    sparkle(t, HIT, 9);
    stars(t);
    sweep(t);
  }
  function sparkle(t, from, to) {
    ctx.setTransform(...CAM); ctx.lineCap = 'round';
    for (const s of SPARKS) {
      if (s.t0 < from || s.t0 >= to) continue;
      const tau = t - s.t0;
      if (tau <= 0 || tau >= s.life) continue;
      const e = (1 - Math.exp(-3 * tau)) / 3, v = Math.exp(-3 * tau), x = s.x + s.vx * e, y = s.y + s.vy * e;
      ctx.globalAlpha = Math.pow(1 - tau / s.life, 1.5); ctx.strokeStyle = css(mix(s.c, WHITE, .35)); ctx.lineWidth = s.w;
      ctx.beginPath(); ctx.moveTo(x, y); ctx.lineTo(x - s.vx * v * .045, y - s.vy * v * .045); ctx.stroke();
    }
  }
  function stars(t) {
    ctx.setTransform(...CAM);
    for (const s of STARS) {
      const q = (t - s.t0) / s.d;
      if (q <= 0 || q >= 1) continue;
      const a = q < .15 ? q / .15 : Math.pow(1 - (q - .15) / .85, 1.4);   // pops, then fades
      star(s.x, s.y, s.R * (.4 + .6 * Math.min(1, q / .15)), a, s.c < 0 ? WHITE : PAL[s.c % PAL.length], q * 1.6);
    }
  }
  // a band of light crossing the logo: the logo brightens under it and blooms at its edges
  function sweep(t) {
    const q = (t - SWEEP) / .65;
    if (q <= 0 || q >= 1) return;
    const g = scratch.getContext('2d'), w = scratch.width, hh = scratch.height, x = -w * .25 + q * w * 1.5, band = w * .1;
    g.globalCompositeOperation = 'source-over'; g.clearRect(0, 0, w, hh); g.drawImage(glowM.c, 0, 0);
    g.globalCompositeOperation = 'source-in';
    const gr = g.createLinearGradient(x - band, -band * .4, x + band, band * .4);
    gr.addColorStop(0, 'rgba(255,255,255,0)'); gr.addColorStop(.5, 'rgba(255,255,255,1)'); gr.addColorStop(1, 'rgba(255,255,255,0)');
    g.fillStyle = gr; g.fillRect(0, 0, w, hh);
    const a = Math.sin(Math.PI * q);
    put(glowM, I, a, scratch); put(glowM, I, .7 * a, scratch);
  }

  // ---- off: the logo flies into the sidebar's (same shape and size, its colour), the window comes in under it;
  // when the sidebar is hidden it grows a little and fades
  let to = null, toImg = null, from = null;
  function leave(t) {
    const x = (t - OUT) / FLY;
    if (!from) {
      CAM = camera(OUT); from = { cx: W * D / 2, cy: H * D / 2, s: L * D * CAMS };
      const el = $('.brand .logo'), r = el && !document.body.classList.contains('collapsed') ? el.getBoundingClientRect() : null;
      if (r && r.width > 20 && r.right > 0 && r.bottom > 0 && r.left < W && r.top < H) {
        to = { cx: (r.left + r.width / 2) * D, cy: (r.top + r.height / 2) * D, s: r.width * D / 1540 };
        const m = /(\d+)\D+(\d+)\D+(\d+)/.exec(getComputedStyle(el).backgroundColor);
        toImg = tint(whole.c, m ? [+m[1], +m[2], +m[3]] : WHITE);
      }
      root.classList.add('intro-out'); root.classList.remove('intro');
    }
    bg.style.opacity = 1 - smooth((t - OUT - .04) / .5);
    if (x >= 1) { finish(); return; }
    const at = x => {
      if (!to) return chain(tr(from.cx, from.cy), scl(from.s * (1 + .3 * outCubic(x))), tr(-C0[0], -C0[1]));
      const e = flyEase(x), dx = to.cx - from.cx, dy = to.cy - from.cy;
      const mx = from.cx + dx / 2 - dy * .14, my = from.cy + dy / 2 + dx * .14;   // the path bows a little
      const px = (1 - e) * (1 - e) * from.cx + 2 * (1 - e) * e * mx + e * e * to.cx, py = (1 - e) * (1 - e) * from.cy + 2 * (1 - e) * e * my + e * e * to.cy;
      return chain(tr(px, py), rot(-.05 * Math.sin(Math.PI * e)), scl(from.s * Math.pow(to.s / from.s, e)), tr(-C0[0], -C0[1]));
    };
    CAM = I;
    const A = at(x), B = at(x - SHUT / FLY), probe = [[VB[0], VB[1]], [VB[2], VB[3]], [VB[0], VB[3]], [VB[2], VB[1]]];
    let dist = 0;
    for (const [px, py] of probe) { const u = mv(A, px, py), v = mv(B, px, py); dist = Math.max(dist, Math.hypot(u[0] - v[0], u[1] - v[1])); }
    const n = Math.max(1, Math.min(24, Math.ceil(dist / (2.2 * D))));
    const cf = to ? smooth((x - .3) / .6) : 0, fade = to ? 1 : 1 - smooth(x / .8);
    const f = Math.min(1, 6 * D / Math.max(1e-6, dist * SPLIT / SHUT));
    for (let i = 0; i < n; i++) {
      const xs = n > 1 ? x + SHUT / FLY * (.5 - i / (n - 1)) : x, M = at(xs);
      if (dist < .4 * D) put(whole, M, fade * (1 - cf) / n);
      else { put(whole, lerpM(M, at(xs - SPLIT / FLY), f), fade * (1 - cf) / n, wholeR); put(whole, lerpM(M, at(xs + SPLIT / FLY), f), fade * (1 - cf) / n, wholeC); }
      if (cf > 0) put(whole, M, cf / n, toImg);
    }
    put(glowW, A, .3 * (1 - smooth(x / .45)) * fade);
  }

  // ---- running it
  let SVG = null, ready = false, started = false, done = false, raf = 0, T0 = 0;
  function frame(now) {
    if (done) return;
    if (cvs.width !== Math.round(innerWidth * (devicePixelRatio || 1)) || cvs.height !== Math.round(innerHeight * (devicePixelRatio || 1))) size();
    if (!T0) T0 = now;
    const t = FREEZE ? +FREEZE : (now - T0) / 1000;
    try { render(t); } catch (e) { console.error(e); finish(); return; }
    if (!done) raf = requestAnimationFrame(frame);
  }
  function skip(e) {
    if (e.type === 'keydown' && ['Shift', 'Control', 'Alt', 'Meta'].includes(e.key)) return;
    removeEventListener('keydown', skip, true); ov.removeEventListener('pointerdown', skip);
    root.classList.add('intro-out'); root.classList.remove('intro');
    ov.style.transition = 'opacity .35s ease'; ov.style.opacity = '0';
    setTimeout(finish, 380);
  }
  function finish() {   // when it never started (no animations, no logo) the page is just there, without coming in
    if (done) return;
    done = true; cancelAnimationFrame(raf);
    removeEventListener('keydown', skip, true);
    if (started) root.classList.add('intro-out');
    root.classList.remove('intro');
    ov.remove();
    setTimeout(() => root.classList.remove('intro-out'), 1000);   // after the page's own coming in
  }
  function go() {
    if (started || done || !SVG || !ready) return;
    if (stillMotion()) { finish(); return; }
    try { build(); } catch (e) { console.error(e); finish(); return; }
    started = true;
    ov.classList.add('run');
    ov.addEventListener('pointerdown', skip); addEventListener('keydown', skip, true);
    raf = requestAnimationFrame(frame);
  }
  fetch('haku.svg').then(r => r.text()).then(s => { SVG = s; go(); }).catch(finish);
  // the first state says whether animations are on, and the effect whose colours to use
  if (wv) wv.addEventListener('message', e => { if (e.data && e.data.type === 'state') { ready = true; go(); } });
  setTimeout(() => { ready = true; go(); }, 800);
})();
