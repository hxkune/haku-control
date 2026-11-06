// SPDX-License-Identifier: GPL-3.0-only
// How long the constructor's picture takes: ui/fxpreview.js comp.frame (the 2D preview, 64 x 36 as the editor draws
// it) for a few compositions, in node. A measurement, not a test: the numbers depend on the machine.
//   node tools\sim\preview_bench.js [warm=50] [frames=300] [fxpreview.js] [size=64x36]   (96x54: the enlarged view)
'use strict';
const fs = require('fs'), path = require('path');
const [warm = 50, runs = 300] = process.argv.slice(2, 4).map(Number);
const file = process.argv[4] || path.join(__dirname, '..', '..', 'ui', 'fxpreview.js');
// loaded in this context: in a vm context every Math.* goes through the sandbox's global, some 20 times slower
const [W, H] = (process.argv[5] || '64x36').split('x').map(Number);
const c = new Function(fs.readFileSync(file, 'utf8') + '\nreturn fxPreview;')().comp, pal = [[255, 0, 80], [0, 160, 255], [255, 200, 0]];
const SETS = {
  'standard (the reactor)': c.DEFAULT,
  '8 spark layers': Array(8).fill('s:spark c:p n:8 life:1.2 tail:12 v:35 w:6 b:add'),
  '8 mixed, modulated': ['s:fill c:#000a30', 's:grad c:p v:20 a:30 ap:a aa:90 at:5', 's:wave c:p0 sz:200 v:-15 b:screen', 's:noise c:p sz:80 v:8 o:40 mp:o ma:60 ms:level',
    's:ring c:#ffffff dur:0.6 per:0.9 w:8 sf:80 mp:o ma:100 ms:beat b:add', 's:dots n:12 w:35 sf:40 b:mask', 's:spark c:p n:6 life:1.2 tail:12 v:35 w:6 b:add', 's:wave c:p2 b:mul o:50'],
};
const pct = (a, q) => a[Math.min(a.length - 1, Math.floor(q * a.length))];
console.log(`node ${process.version}, ${W}x${H}, ${warm} warm-up + ${runs} measured frames, t from 0 in steps of 1/30 s`);
for (const [name, lines] of Object.entries(SETS)) {
  const L = lines.map(c.parse), ms = [];
  for (let i = 0; i < warm + runs; i++) {
    const t0 = process.hrtime.bigint(); c.frame(L, pal, W, H, i / 30); const d = Number(process.hrtime.bigint() - t0) / 1e6;
    if (i >= warm) ms.push(d);
  }
  ms.sort((a, b) => a - b);
  console.log(`${name.padEnd(24)} median ${pct(ms, 0.5).toFixed(3)} ms   p95 ${pct(ms, 0.95).toFixed(3)} ms   max ${ms[ms.length - 1].toFixed(3)} ms`);
}
