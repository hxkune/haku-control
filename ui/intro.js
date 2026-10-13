// SPDX-License-Identifier: GPL-3.0-only
'use strict';
// The haku intro: plays every time the window opens (the core opens the page as
// index.html#intro). A spark flickers on in the dark, light is drawn in to it and it opens into a four-pointed
// star, which pops. Ribbons of light in the colours of the current effect whirl out of it and wind down into the
// orbit, and the star breaks away, swings round and draws the orbit behind it as it turns in its own plane. The
// wordmark (haku.svg) is cut into its pieces: the letters h, a, k, u and the two arcs inside the orbit; the arcs come
// round the orbit's plane, the letters fly in along curves, each on a spring, with motion blur (sub-frames over the
// shutter), their pink and cyan apart along the motion and ghosts of where they just were. They land with a short
// glitch; the star lands on the orbit's thin end, the logo flashes, a ring runs out along the orbit, sparks fly; a
// light sweeps across it, then it flies into its place in the sidebar while the window comes in. A click or a key
// skips it; without animations (Settings) it does not play.
// Each base theme has its own: dark is light in the dark; grey is the same on graphite, its glow softer and the sweep
// two hard glints; light is ink on paper (the film is still drawn as light, and the page inverts the canvas with the
// hues kept, style.css). The hidden themes get the dark one.
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
  // cut here into the h (with its long stroke), the k (with its spike over the orbit and its tail under it) and the
  // arc that grows out of the orbit on the left. What is left of it is the orbit.
  const CUT = {
    h: [[200, 10, 262, 10, 352, 180, 352, 320, 430, 300, 600, 300, 700, 420, 745, 485, 826, 684, 842, 706, 830, 716, 560, 640,
      440, 600, 405, 727, 357, 728, 282, 522, 212, 480, 200, 300], [360, 752, 402, 752, 404, 798, 370, 800], [385, 872, 430, 868, 430, 960, 385, 960]],
    k: [[700, 320, 860, 260, 975, 262, 1050, 190, 1095, 186, 1150, 200, 1135, 230, 1100, 300, 920, 390, 1000, 500, 1100, 605,
      1060, 640, 1010, 640, 860, 470, 858, 705, 800, 705, 720, 480, 700, 330], [1092, 150, 1172, 68, 1182, 72, 1160, 145],
      [1100, 693, 1143, 647, 1187, 628, 1362, 538, 1385, 545, 1300, 645, 1200, 705, 1090, 705]],
    swl: [[240, 528, 205, 545, 170, 590, 145, 650, 147, 700, 170, 740, 230, 772, 300, 778, 365, 775, 372, 752, 420, 755, 530, 748,
      530, 712, 405, 722, 357, 728, 282, 522]],
  };
  const CUTS = ['h', 'k', 'swl'];
  // where the orbit crosses a letter the crossing stays with the orbit, but the letter has it too, so it comes whole
  // (and nothing changes once all is in place): the h's foot tapering into the orbit and its tail through the arc
  // and the orbit, the k's arm and its tail through the orbit
  const MORE = {
    h: [[212, 480, 282, 522, 240, 521, 205, 548, 150, 606, 185, 540, 205, 500],
      [360, 722, 401, 720, 401, 800, 413, 868, 421, 945, 409, 945, 397, 868, 379, 800, 366, 752]],
    k: [[1050, 192, 1090, 150, 1160, 145, 1148, 200], [1028, 642, 1092, 602, 1192, 624, 1143, 650, 1098, 694, 1062, 676]],
  };

  // ---- timing (s): PRE is the star's own entrance; the rest counts from its pop
  const PRE = .46, HIT = .97, SWEEP = 1.35, OUT = 2.3, FLY = .72, SHUT = .016, SPLIT = .011;

  // ---- maths
  const clamp = x => x < 0 ? 0 : x > 1 ? 1 : x, smooth = x => (x = clamp(x), x * x * (3 - 2 * x));
  const outCubic = x => 1 - Math.pow(1 - clamp(x), 3);
  const outBack = x => (x = clamp(x), 1 + 2.4 * Math.pow(x - 1, 3) + 1.4 * Math.pow(x - 1, 2));
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
  // the ghosts' two colours: the two of the effect's furthest apart (cyan and pink by default); a one-colour
  // effect gets it and a paler it
  let CH;
  function ghostCols(p) {
    let best = [p[0], mix(p[0], WHITE, .6)], far = 120 * 120;
    for (const a of p) for (const b of p) {
      const d = a.reduce((s, v, i) => s + (v - b[i]) ** 2, 0);
      if (d > far) { far = d; best = [a, b]; }
    }
    return best;
  }

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

  // ---- sprites: a piece drawn once at the logo's size, with a pink and a cyan copy
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
  function glowOf(s, blur, col) {   // blur in css px
    const g = layer([s.x, s.y, s.x + s.w, s.y + s.h], blur * 3 / L, c => { c.filter = `blur(${blur * D}px)`; c.drawImage(s.c, s.x, s.y, s.w, s.h); });
    if (col) g.c = tint(g.c, col);
    return g;
  }

  // ---- the pieces and how each one comes in
  // letters: from `off` (logo units, from where it ends) along a curve bent towards `ctl`, turned by `rot` and
  // sized `sc` at the start; arcs: turned `orb` round the orbit's plane and a bit wider
  const P = [
    { id: 'ring', box: [48, 137, 1552, 871], t0: .2 },
    { id: 'swr', box: [1206, 212, 1401, 389], orb: 2.6, t0: .5 },
    { id: 'swl', box: [169, 522, 506, 752], orb: 2.2, t0: .54 },
    { id: 'h', box: [140, 32, 845, 945], piv: [330, 470], off: [-250, -1150], ctl: [-800, -350], rot: -.6, sc: 1.5, t0: .2 },
    { id: 'k', box: [731, 82, 1364, 688], piv: [1000, 400], off: [1350, -650], ctl: [350, -950], rot: .55, sc: 1.35, t0: .3 },
    { id: 'a', box: [333, 120, 808, 683], piv: [570, 400], off: [-350, 1150], ctl: [-1050, 350], rot: -1.1, sc: .55, t0: .38 },
    { id: 'u', box: [958, 279, 1330, 555], piv: [1145, 420], off: [550, 1050], ctl: [1200, 450], rot: .8, sc: .6, flip: 1, t0: .46 },
  ];
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
    else p.at = t => { const r = spring(t - .2, 7, .8, .85, 7); return inPlane(2.6 * r, 1 + .25 * r); };
    // the moment a letter first reaches its place (it glitches there)
    if (p.piv) for (let s = 0; s < .6; s += .002) if (spring(s, 13, .72, .58, 6) <= .01) { p.land = p.t0 + s; break; }
    const [x0, y0, x1, y1] = p.box;
    p.probe = [[x0, y0], [x1, y0], [x0, y1], [x1, y1], [(x0 + x1) / 2, (y0 + y1) / 2]];
  }
  // the orbit's stroke being drawn, its head, and the star: from the middle it breaks away to that head
  const ringHead = t => TH0 - (TH0 - TH1) * outCubic((t - .2) / .75);
  const headAt = t => mv(P[0].at(Math.min(t, .95)), ...onOrbit(ringHead(Math.min(t, .95))));
  const starAt = t => { const e = smooth((t - .07) / .13), h = headAt(t); return [OC[0] + (h[0] - OC[0]) * e, OC[1] + (h[1] - OC[1]) * e]; };

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
    for (let j = 0; j < 80; j++) {
      const th = TH1 + (TH0 - TH1) * hash(500 + j), k = 1 + (hash(600 + j) - .5) * .12, p = onOrbit(th, k);
      const n = mv(E, Math.cos(th) * 1.3, Math.sin(th) * 1.3), dx = n[0] - p[0], dy = n[1] - p[1], l = Math.hypot(dx, dy) || 1;
      const sp = 450 + 1200 * hash(700 + j) * hash(800 + j);
      SPARKS.push({ t0: HIT + .05 * hash(900 + j), x: p[0], y: p[1], vx: dx / l * sp, vy: dy / l * sp, life: .35 + .45 * hash(1000 + j),
        c: hash(1100 + j) < .4 ? WHITE : PAL[j % PAL.length], w: 2.2 + 2 * hash(1200 + j) });
    }
  }
  // the four-pointed stars: on the tips when the logo lands, then a few twinkling
  const STARS = [
    { x: 1172, y: 72, t0: HIT + .02, d: .55, R: 90, c: 1 },
    { x: 236, y: 34, t0: HIT + .07, d: .5, R: 64, c: 2 },
    { x: 418, y: 926, t0: HIT + .1, d: .45, R: 56, c: 0 },
    { x: 1552, y: 290, t0: HIT + .05, d: .5, R: 66, c: 1 },
    { x: 1470, y: 172, t0: 1.32, d: .9, R: 34, c: -1 },
    { x: 140, y: 600, t0: 1.62, d: .9, R: 30, c: -1 },
    { x: 990, y: 905, t0: 1.9, d: .8, R: 28, c: -1 },
  ];
  // the star's entrance: light drawn in to it from all round
  const PULL = [...Array(46)].map((_, i) => {
    const ta = .16 + .24 * Math.pow(hash(300 + i), .7);
    return { a: hash(400 + i) * 6.283, r0: 650 + 500 * hash(500 + i), ta, ts: ta - .2 - .08 * hash(600 + i), c: hash(700 + i) < .35 ? -1 : i % 3, w: 3.5 + 4 * hash(800 + i) };
  });

  let whole, wholeCh, glowW, glowM, glowT, scratch, TH = 'dark', GLOW = 1;
  function build() {
    size(); K = L * D; PAL = colours(); CH = ghostCols(PAL);
    TH = ['grey', 'light'].includes(root.dataset.theme) ? root.dataset.theme : 'dark'; GLOW = { dark: 1, grey: .6, light: .45 }[TH];
    ov.dataset.th = TH;
    const d = /\sd="([^"]+)"/.exec(SVG)[1], sub = d.match(/M[^M]+/g);
    const path = (...i) => new Path2D(i.map(j => sub[j]).join(''));
    const poly = list => { const q = new Path2D(); for (const a of list) { q.moveTo(a[0], a[1]); for (let i = 2; i < a.length; i += 2) q.lineTo(a[i], a[i + 1]); q.closePath(); } return q; };
    const rest = path(0, 4, 6), own = { a: path(1, 5), u: path(3), swr: path(2) };
    for (const p of P) {
      p.s = layer(p.box, 3, g => {
        if (own[p.id]) { g.fill(own[p.id], 'evenodd'); return; }
        const n = CUTS.indexOf(p.id);
        if (n >= 0) { g.save(); g.clip(poly(CUT[p.id].concat(MORE[p.id] || []))); }
        g.fill(rest, 'evenodd');
        if (n >= 0) g.restore();
        g.globalCompositeOperation = 'destination-out';
        for (const id of n >= 0 ? CUTS.slice(0, n) : CUTS) g.fill(poly(CUT[id]));
      });
      p.ch = CH.map(c => tint(p.s.c, c));
    }
    whole = layer(VB, 3, g => g.fill(new Path2D(d), 'evenodd'));
    wholeCh = CH.map(c => tint(whole.c, c));
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
  // a piece over the shutter: as many sub-frames as it moves (a few px apart), pink a little behind and cyan a
  // little ahead in time (never more than a few px apart), so its colours part along the motion and come together
  // when it stops; a fast one also leaves ghosts
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
    const f = Math.min(1, 7 * D / Math.max(1e-6, dist * SPLIT / SHUT));
    for (let i = 0; i < n; i++) {
      const ts = n > 1 ? t + SHUT * (.5 - i / (n - 1)) : t, M = p.at(ts);
      if (dist < .4 * D) one(p.s.c, M, ts, a / n);
      else for (let c = 0; c < 2; c++) one(p.ch[c], lerpM(M, p.at(ts + (c - .5) * SPLIT), f), ts, a / n);
    }
    if (!clip && dist > 8 * D) ghosts(p.s, p.ch, p.at, t, a * Math.min(1, (dist / D - 8) / 30));
  }
  // the ghosts: echoes of where it just was, each one further back and its pink and cyan further apart
  function ghosts(s, ch, at, t, a, step = .026, n = 3) {
    for (let j = n; j >= 1; j--) {
      const tb = t - j * step, al = a * .3 * Math.pow(1 - (j - 1) / n, 1.2);
      for (let c = 0; c < 2; c++) put(s, at(tb + (c - .5) * .012 * (j + 1)), al, ch[c]);
    }
  }
  // the landing glitch: torn into strips, its pink and cyan jumping apart in each
  function glitch(p, t, a) {
    const f = Math.floor(t * 60), s = p.s, n = 6, m = mul(CAM, p.at(t));
    ctx.setTransform(m[0], m[1], m[2], m[3], m[4], m[5]); ctx.globalAlpha = a;
    for (let j = 0; j < n; j++) {
      const big = hash(f * 3 + j * 5) > .45, dx = (hash(f * 7 + j) - .5) * (big ? 50 : 10), sp = (big ? 26 : 9) * (hash(f * 11 + j) + .4);
      for (let c = 0; c < 2; c++)
        ctx.drawImage(p.ch[c], 0, j * s.c.height / n, s.c.width, s.c.height / n, s.x + dx + (c - .5) * 2 * sp, s.y + j * s.h / n, s.w, s.h / n);
    }
  }
  function sector(a0, a1) {   // a wedge from the orbit's centre between two of its angles
    const q = new Path2D(), n = Math.max(2, Math.ceil(Math.abs(a1 - a0) / .06));
    q.moveTo(OC[0], OC[1]);
    for (let i = 0; i <= n; i++) { const o = onOrbit(a0 + (a1 - a0) * i / n, 1.75); q.lineTo(o[0], o[1]); }
    q.closePath(); return q;
  }
  // a stroke of light along pt(x), its bright head at `th` and its tail at th + len, thin and fading
  function streak(pt, th, len, w, col, a) {
    if (a <= .01) return;
    ctx.lineCap = 'round';
    const n = 34; let prev = pt(th + len);
    for (let i = 1; i <= n; i++) {
      const s = i / n, q = pt(th + len * (1 - s)), lw = w * (.12 + .88 * Math.pow(s, 1.5));
      ctx.beginPath(); ctx.moveTo(prev[0], prev[1]); ctx.lineTo(q[0], q[1]);
      ctx.globalAlpha = a * s * s * .28; ctx.strokeStyle = css(col); ctx.lineWidth = lw * 4.5; ctx.stroke();
      ctx.globalAlpha = a * s * s; ctx.strokeStyle = css(mix(col, WHITE, Math.pow(s, 3) * .7)); ctx.lineWidth = lw; ctx.stroke();
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
    film(t - PRE);
    if (!done) entrance(t);
  }
  // the star's entrance, before everything: a spark flickers on in the dark, light is drawn in to it from all
  // round, then its rays shoot out (the upright one first) as it snaps round into place, and it pops
  function entrance(t) {
    if (t > PRE + .5) return;
    ctx.setTransform(...CAM); ctx.lineCap = 'round';
    const [x, y] = OC;
    for (const s of PULL) {
      const u = (t - s.ts) / (s.ta - s.ts);
      if (u <= 0 || u >= 1) continue;
      const e = Math.pow(u, 2.2), r = s.r0 * (1 - e), len = 30 + 260 * e, ca = Math.cos(s.a), sa = Math.sin(s.a), c = s.c < 0 ? WHITE : PAL[s.c];
      const g = ctx.createLinearGradient(x + ca * r, y + sa * r, x + ca * (r + len), y + sa * (r + len));
      g.addColorStop(0, css(mix(c, WHITE, .5), Math.min(1, .3 + u * 1.6))); g.addColorStop(.4, css(c, .5 * Math.min(1, u * 2))); g.addColorStop(1, css(c, 0));
      ctx.globalAlpha = 1; ctx.strokeStyle = g; ctx.lineWidth = s.w;
      ctx.beginPath(); ctx.moveTo(x + ca * r, y + sa * r); ctx.lineTo(x + ca * (r + len), y + sa * (r + len)); ctx.stroke();
    }
    // the core: a flickering spark, then a glow that swells as the light comes in, and a flat flare across it
    const charge = smooth((t - .1) / .3), flick = t < .13 ? [1, 0, .9, .15, 1, .4, 1][Math.floor(t / .019)] ?? 1 : 1;
    if (t < PRE) {
      const R = 26 + 120 * charge, g = ctx.createRadialGradient(x, y, 0, x, y, R);
      g.addColorStop(0, css(WHITE, .9 * flick)); g.addColorStop(.25, css(mix(PAL[0], WHITE, .5), .45 * flick)); g.addColorStop(1, css(PAL[0], 0));
      ctx.globalAlpha = 1; ctx.fillStyle = g; ctx.beginPath(); ctx.arc(x, y, R, 0, 6.283); ctx.fill();
      const fl = 40 + 700 * charge, gf = ctx.createLinearGradient(x - fl, y, x + fl, y);
      gf.addColorStop(0, css(PAL[0], 0)); gf.addColorStop(.5, css(mix(PAL[0], WHITE, .6), .8 * flick)); gf.addColorStop(1, css(PAL[0], 0));
      ctx.strokeStyle = gf; ctx.lineWidth = 3 + 3 * charge; ctx.beginPath(); ctx.moveTo(x - fl, y); ctx.lineTo(x + fl, y); ctx.stroke();
    }
    // the rays: upright first, then across, overshooting; the star turns from a quarter round into place
    const qv = (t - .3) / .1, qh = (t - .34) / .1;
    if (qv > 0 && t < PRE) {
      const sy = outBack(qv), sx = qh > 0 ? outBack(qh) : .04, turn = -.785 * spring(t - .3, 16, .45, PRE - .3 + .001);
      ctx.save(); ctx.translate(x, y); ctx.scale(Math.max(.04, sx), Math.max(.04, sy));
      star(0, 0, 300, 1, mix(PAL[0], WHITE, .35), turn, .03);
      ctx.restore();
      star(x, y, 96 * Math.min(sx, sy), 1, WHITE, .4 - turn, .07);
    }
    // the pop: a flash and a ring going out
    const k = (t - PRE + .02) / .45;
    if (k > 0 && k < 1) {
      ctx.globalAlpha = .9 * Math.pow(1 - k, 2); ctx.strokeStyle = css(mix(PAL[0], WHITE, .5)); ctx.lineWidth = 10 * (1 - k);
      ctx.beginPath(); ctx.arc(x, y, 60 + 520 * outCubic(k), 0, 6.283); ctx.stroke();
      const g = ctx.createRadialGradient(x, y, 0, x, y, 420);
      g.addColorStop(0, css(WHITE, .5 * Math.pow(1 - k, 3))); g.addColorStop(1, css(PAL[0], 0));
      ctx.globalAlpha = 1; ctx.fillStyle = g; ctx.beginPath(); ctx.arc(x, y, 420, 0, 6.283); ctx.fill();
    }
  }
  // the film from the star's pop on (t can be below 0: nothing has started yet, the camera waits)
  function film(t) {
    if (t >= OUT) { leave(t); return; }
    CAM = camera(t);
    const h = t - HIT;
    // a little of the colour lights the dark behind, more on the landing
    const energy = GLOW * GLOW * .35 * clamp(t / .4) * (h < 0 ? 1 : .4) + (h > 0 ? .9 * Math.exp(-h / .25) : 0);
    const cx = W * D / 2, cy = H * D / 2, R0 = Math.max(W, H) * D * .7, gb = ctx.createRadialGradient(cx, cy, 0, cx, cy, R0);
    gb.addColorStop(0, css(PAL[0], .16 * energy)); gb.addColorStop(.5, css(PAL[1], .05 * energy)); gb.addColorStop(1, css(PAL[1], 0));
    ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.globalAlpha = 1; ctx.fillStyle = gb; ctx.fillRect(0, 0, cvs.width, cvs.height);
    ctx.setTransform(...CAM);
    for (const r of RIB) {
      const R = ribbon(r, t);
      if (R) streak(R.pt, R.th, r.len, 12 * (.6 + .4 * R.k), r.c < 0 ? WHITE : PAL[r.c % PAL.length], R.a);
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
    // the star: it breaks away from the middle, swings out round the logo drawing the orbit behind it, and lands
    // on the orbit's thin end (that landing is the hit)
    ctx.setTransform(...CAM);
    const ha = clamp((t - .2) / .06) * (1 - smooth((t - .95) / .06));
    if (ha > 0) { const M = P[0].at(t); streak(th => mv(M, ...onOrbit(th)), ringHead(t), 1.1, 11, mix(PAL[0], WHITE, .3), ha); }
    if (t >= 0 && t < 1.3) {
      const sp = starAt(t), land = t - .95;
      if (t > .09 && t < .28) streak(starAt, t, -.06, 16, mix(PAL[0], WHITE, .4), smooth((t - .09) / .04) * (1 - smooth((t - .2) / .06)));
      const R = t < .1 ? 300 : land < 0 ? 300 - 215 * outCubic((t - .1) / .1) : 85 * (1 + 1.2 * outCubic(land / .08)) * (1 - smooth(land / .3));
      const a = land < 0 ? 1 : 1 - smooth((land - .1) / .2);
      star(sp[0], sp[1], R, a, mix(PAL[0], WHITE, .35), t * 5, land < 0 ? .03 + .03 * smooth((t - .1) / .14) : .06);
      star(sp[0], sp[1], R * .32, a, WHITE, -t * 7 + .4, .07);
    }
    if (joined > 0) {
      // the logo, white, its two ghost colours still a little apart at its edges, and glowing
      const sp = (2.4 * Math.exp(-h * 5) + .5) / (L * CAMS), b = joined * (.9 + .1 * Math.exp(-h * 4));
      put(whole, I, b); put(whole, tr(-sp, 0), b, wholeCh[0]); put(whole, tr(sp, 0), b, wholeCh[1]);
    }
    if (h > 0) {
      put(glowW, I, GLOW * (.8 * Math.exp(-h / .22) + .26 + .05 * Math.sin(t * 5)));
      put(glowT, I, GLOW * (.9 * Math.exp(-h / .1) + .16));
      // the ring running out along the orbit
      ctx.setTransform(...CAM);
      for (const [d, k, col] of [[0, .55, PAL[0]], [.05, .32, WHITE]]) {
        const q = (h - d) / .6;
        if (q <= 0 || q >= 1) continue;
        const kk = 1 + k * outCubic(q);
        ctx.globalAlpha = .85 * (1 - q) * (1 - q); ctx.strokeStyle = css(col); ctx.lineWidth = 13 * Math.pow(1 - q, 1.5);
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
      ctx.globalAlpha = Math.pow(1 - tau / s.life, 1.5); ctx.strokeStyle = css(mix(s.c, WHITE, .3)); ctx.lineWidth = s.w;
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
  // a band of light crossing the logo: the logo brightens under it and blooms at its edges (on grey: two hard
  // glints, the second fainter, a little behind)
  function sweep(t) {
    const q = (t - SWEEP) / .65;
    if (q <= 0 || q >= 1) return;
    const grey = TH === 'grey', g = scratch.getContext('2d'), w = scratch.width, hh = scratch.height, band = w * (grey ? .045 : .1);
    for (const off of grey ? [0, -.09] : [0]) {
      const x = -w * .25 + (q + off) * w * 1.5, a = Math.sin(Math.PI * q) * (grey ? off ? .75 : 1.5 : 1);
      g.globalCompositeOperation = 'source-over'; g.clearRect(0, 0, w, hh); g.drawImage(glowM.c, 0, 0);
      g.globalCompositeOperation = 'source-in';
      const gr = g.createLinearGradient(x - band, -band * .4, x + band, band * .4);
      gr.addColorStop(0, 'rgba(255,255,255,0)'); gr.addColorStop(.5, 'rgba(255,255,255,1)'); gr.addColorStop(1, 'rgba(255,255,255,0)');
      g.fillStyle = gr; g.fillRect(0, 0, w, hh);
      put(glowM, I, a, scratch); put(glowM, I, .7 * a, scratch);
    }
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
        const c = m ? [+m[1], +m[2], +m[3]] : WHITE;
        // shortcut: on light the canvas is inverted, so it lands as the inverse; exact for grey logos, a coloured one comes out a little off in hue
        toImg = tint(whole.c, TH === 'light' ? c.map(v => 255 - v) : c);
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
    // white while it flies, its own colour as it lands; pink and cyan apart and ghosts while it is fast
    const cf = to ? smooth((x - .3) / .6) : 0, fade = to ? 1 : 1 - smooth(x / .8), wa = fade * (1 - cf);
    const f = Math.min(1, 7 * D / Math.max(1e-6, dist * SPLIT / SHUT));
    for (let i = 0; i < n; i++) {
      const xs = n > 1 ? x + SHUT / FLY * (.5 - i / (n - 1)) : x, M = at(xs);
      if (dist < .4 * D) put(whole, M, wa / n);
      else for (let c = 0; c < 2; c++) put(whole, lerpM(M, at(xs + (c - .5) * SPLIT / FLY), f), wa / n, wholeCh[c]);
      if (cf > 0) put(whole, M, cf / n, toImg);
    }
    if (dist > 8 * D) ghosts(whole, wholeCh, s => at(s / FLY), x * FLY, wa * Math.min(1, (dist / D - 8) / 30) * .8, .022, 4);
    put(glowW, A, .3 * (1 - smooth(x / .45)) * fade);
  }

  // ---- running it
  let SVG = null, ready = false, started = false, done = false, raf = 0, last = 0, T = 0;
  function frame(now) {
    if (done) return;
    if (cvs.width !== Math.round(innerWidth * (devicePixelRatio || 1)) || cvs.height !== Math.round(innerHeight * (devicePixelRatio || 1))) size();
    // a frame that came late (the window still settling as it opens) holds the film rather than skipping it on
    T += Math.min(now - (last || now), 34) / 1000; last = now;
    const t = FREEZE ? +FREEZE : T;
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
    setTimeout(() => root.classList.remove('intro-out'), 1800);   // after the page's own coming in (style.css)
  }
  function go() {
    if (started || done || !SVG || !ready) return;
    if (stillMotion()) { finish(); return; }
    try {
      build();
      // the first draw of each layer is slow (the browser readies it then): drawn once through the film while the
      // window is still dark, so the film itself does not stall in its first frames
      for (let t = 0; t < PRE + OUT; t += .15) render(t);
      ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.clearRect(0, 0, cvs.width, cvs.height);
    } catch (e) { console.error(e); finish(); return; }
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
