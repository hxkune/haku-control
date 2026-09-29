// SPDX-License-Identifier: GPL-3.0-only
'use strict';
// Small QR code generator for the phone link (byte mode, error correction level M, versions 1-6 = up to 106
// bytes, plenty for "http://192.168.137.1:8723/#pin=123456"). Follows ISO/IEC 18004; the structure mirrors
// Project Nayuki's reference implementation. qrMatrix(text) -> array of rows of booleans (true = dark).
const qrMatrix = (() => {
  // version -> [data codewords per block, blocks, EC codewords per block] at level M
  const M = [null, [16, 1, 10], [28, 1, 16], [44, 1, 26], [32, 2, 18], [43, 2, 24], [27, 4, 16]];

  const mul = (x, y) => {   // GF(256), polynomial 0x11D
    let z = 0;
    for (let i = 7; i >= 0; i--) { z = (z << 1) ^ ((z >>> 7) * 0x11D); z ^= ((y >>> i) & 1) * x; }
    return z & 0xFF;
  };
  const divisor = degree => {
    const r = new Array(degree).fill(0); r[degree - 1] = 1;
    let root = 1;
    for (let i = 0; i < degree; i++) {
      for (let j = 0; j < r.length; j++) { r[j] = mul(r[j], root); if (j + 1 < r.length) r[j] ^= r[j + 1]; }
      root = mul(root, 2);
    }
    return r;
  };
  const remainder = (data, div) => {
    const r = new Array(div.length).fill(0);
    for (const b of data) {
      const f = b ^ r.shift(); r.push(0);
      div.forEach((c, i) => { r[i] ^= mul(c, f); });
    }
    return r;
  };

  function codewords(bytes, ver) {
    const [dpb, nb, ecb] = M[ver], cap = dpb * nb;
    const bits = [];
    const put = (v, n) => { for (let i = n - 1; i >= 0; i--) bits.push((v >>> i) & 1); };
    put(4, 4); put(bytes.length, 8); bytes.forEach(b => put(b, 8));
    put(0, Math.min(4, cap * 8 - bits.length));
    while (bits.length % 8) bits.push(0);
    const data = [];
    for (let i = 0; i < bits.length; i += 8) data.push(bits.slice(i, i + 8).reduce((a, b) => a * 2 + b, 0));
    for (let p = 0xEC; data.length < cap; p ^= 0xEC ^ 0x11) data.push(p);
    const div = divisor(ecb), blocks = [], ecs = [];
    for (let b = 0; b < nb; b++) { const d = data.slice(b * dpb, (b + 1) * dpb); blocks.push(d); ecs.push(remainder(d, div)); }
    const out = [];
    for (let i = 0; i < dpb; i++) blocks.forEach(d => out.push(d[i]));
    for (let i = 0; i < ecb; i++) ecs.forEach(e => out.push(e[i]));
    return out;
  }

  function build(bytes, ver, mask) {
    const size = ver * 4 + 17;
    const m = Array.from({ length: size }, () => new Array(size).fill(false));
    const fn = Array.from({ length: size }, () => new Array(size).fill(false));
    const set = (x, y, d) => { m[y][x] = d; fn[y][x] = true; };
    for (let i = 0; i < size; i++) { set(6, i, i % 2 === 0); set(i, 6, i % 2 === 0); }
    for (const [cx, cy] of [[3, 3], [size - 4, 3], [3, size - 4]])
      for (let dy = -4; dy <= 4; dy++) for (let dx = -4; dx <= 4; dx++) {
        const x = cx + dx, y = cy + dy, d = Math.max(Math.abs(dx), Math.abs(dy));
        if (x >= 0 && x < size && y >= 0 && y < size) set(x, y, d !== 2 && d !== 4);
      }
    if (ver >= 2) {   // versions 2-6 have one alignment pattern
      const a = ver * 4 + 10;
      for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) set(a + dx, a + dy, Math.max(Math.abs(dx), Math.abs(dy)) !== 1);
    }
    // format information (level M = 0b00)
    let rem = mask;
    for (let i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >>> 9) * 0x537);
    const f = ((mask << 10) | rem) ^ 0x5412, bit = i => ((f >>> i) & 1) === 1;
    for (let i = 0; i <= 5; i++) set(8, i, bit(i));
    set(8, 7, bit(6)); set(8, 8, bit(7)); set(7, 8, bit(8));
    for (let i = 9; i < 15; i++) set(14 - i, 8, bit(i));
    for (let i = 0; i < 8; i++) set(size - 1 - i, 8, bit(i));
    for (let i = 8; i < 15; i++) set(8, size - 15 + i, bit(i));
    set(8, size - 8, true);
    // data in the zigzag, then the mask on non-function modules
    const cw = codewords(bytes, ver);
    let k = 0;
    for (let right = size - 1; right >= 1; right -= 2) {
      if (right === 6) right = 5;
      for (let v = 0; v < size; v++) for (let j = 0; j < 2; j++) {
        const x = right - j, y = ((right + 1) & 2) === 0 ? size - 1 - v : v;
        if (!fn[y][x] && k < cw.length * 8) { m[y][x] = ((cw[k >>> 3] >>> (7 - (k & 7))) & 1) === 1; k++; }
      }
    }
    const MASKS = [
      (x, y) => (x + y) % 2 === 0, (x, y) => y % 2 === 0, (x, y) => x % 3 === 0, (x, y) => (x + y) % 3 === 0,
      (x, y) => (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0, (x, y) => x * y % 2 + x * y % 3 === 0,
      (x, y) => (x * y % 2 + x * y % 3) % 2 === 0, (x, y) => ((x + y) % 2 + x * y % 3) % 2 === 0];
    for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) if (!fn[y][x] && MASKS[mask](x, y)) m[y][x] = !m[y][x];
    return m;
  }

  // penalty rules 1, 2 and 4 of the standard: enough to avoid masks that are hard to scan
  function penalty(m) {
    const n = m.length; let p = 0, dark = 0;
    for (let pass = 0; pass < 2; pass++) for (let a = 0; a < n; a++) {
      let run = 1;
      for (let b = 1; b <= n; b++) {
        const cur = b < n ? (pass ? m[b][a] : m[a][b]) : null, prev = pass ? m[b - 1][a] : m[a][b - 1];
        if (cur === prev) run++; else { if (run >= 5) p += run - 2; run = 1; }
      }
    }
    for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
      if (m[y][x]) dark++;
      if (x < n - 1 && y < n - 1 && m[y][x] === m[y][x + 1] && m[y][x] === m[y + 1][x] && m[y][x] === m[y + 1][x + 1]) p += 3;
    }
    return p + Math.floor(Math.abs(dark * 20 - n * n * 10) / (n * n)) * 10;
  }

  const make = text => {
    const bytes = Array.from(new TextEncoder().encode(text));
    const ver = M.findIndex((v, i) => i && v[0] * v[1] - 2 >= bytes.length);
    if (ver < 0) return null;
    let best = null, bestP = Infinity;
    for (let mask = 0; mask < 8; mask++) {
      const m = build(bytes, ver, mask), p = penalty(m);
      if (p < bestP) { best = m; bestP = p; }
    }
    return best;
  };
  make.codewords = codewords; make.build = build; make.rs = (d, n) => remainder(d, divisor(n));   // for tests
  return make;
})();

// SVG markup: dark modules on white with the 4-module quiet zone the standard asks for
function qrSvg(text) {
  const m = qrMatrix(text);
  if (!m) return '';
  const n = m.length + 8;
  let d = '';
  m.forEach((row, y) => row.forEach((on, x) => { if (on) d += `M${x + 4} ${y + 4}h1v1h-1z`; }));
  return `<svg viewBox="0 0 ${n} ${n}" shape-rendering="crispEdges" role="img"><rect width="${n}" height="${n}" fill="#fff"/><path d="${d}" fill="#000"/></svg>`;
}
if (typeof module !== 'undefined') module.exports = { qrMatrix, qrSvg };
