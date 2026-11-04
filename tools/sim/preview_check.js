// SPDX-License-Identifier: GPL-3.0-only
// The window's effect previews (ui/fxpreview.js) against the engine (src/effects.c): the same colours on a 28-LED
// strip for every effect the preview ports, with several settings and moments, and FX_PARAMS with the same keys,
// defaults and ranges as effects.c FXP.
//   effects_test.exe dump %TEMP%\fxdump.json && node tools\sim\preview_check.js %TEMP%\fxdump.json
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');
const root = path.join(__dirname, '..', '..');
const ctx = {}; vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(root, 'ui', 'fxpreview.js'), 'utf8') + '\nthis.FX_PARAMS = FX_PARAMS; this.fxPreview = fxPreview; this.fxParamValue = fxParamValue;', ctx);
const { FX_PARAMS, fxPreview, fxParamValue } = ctx;

let fails = 0;
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) fails++; };

// ---- FX_PARAMS = FXP
const src = fs.readFileSync(path.join(root, 'src', 'effects.c'), 'utf8');
const fxp = {};
for (const m of src.matchAll(/\{\s*FX_(\w+),\s*"(\w+)",\s*([-\d.]+)f?,\s*([-\d.]+)f?,\s*([-\d.]+)f?\s*\}/g)) {
  const id = m[1] === 'INTERF' ? 'interference' : m[1].toLowerCase();
  (fxp[id] = fxp[id] || []).push([m[2], +m[3], +m[4], +m[5]]);
}
check(Object.keys(fxp).length >= 10, 'the FXP table is found in effects.c (' + Object.keys(fxp).length + ' effects)');
let same = true;
for (const id of new Set([...Object.keys(fxp), ...Object.keys(FX_PARAMS)])) {
  const a = (fxp[id] || []).map(p => p.join(' ')).sort().join('; '), b = (FX_PARAMS[id] || []).map(p => p.slice(0, 4).join(' ')).sort().join('; ');
  if (a !== b) { same = false; console.log('     ' + id + ': effects.c ' + a + ' | fxpreview.js ' + b); }
}
check(same, 'FX_PARAMS has the keys, defaults and ranges of effects.c FXP');

// ---- the constructor: its words, limits, shapes, blends and the reactor as in effects.c
const comp = fxPreview.comp, tbl = s => src.match(new RegExp(s))[1];
const lpd = [...tbl('LPD\\[LP_NUM\\] = \\{([\\s\\S]*?)\\n\\};').matchAll(/\{\s*"(\w+)",\s*([-\d.]+)f?,\s*([-\d.]+)f?,\s*([-\d.]+)f?\s*\}/g)].map(m => [m[1], +m[2], +m[3], +m[4]].join(' '));
const names = s => [...tbl(s).matchAll(/"(\w+)"/g)].map(m => m[1]).join(' ');
check(lpd.length === comp.LPD.length && lpd.join('; ') === comp.LPD.map(d => d.join(' ')).join('; '), `the layer settings match effects.c LPD (${lpd.length})`);
check(names('CS_NAME\\[CS_N\\] = \\{([^}]*)\\}') === comp.SHAPES.join(' ') && names('CB_NAME\\[CB_N\\] = \\{([^}]*)\\}') === comp.BLENDS.join(' '), 'the shapes and blends match effects.c');
check(JSON.stringify([...tbl('COMP_DEFAULT\\[\\] = \\{([^}]*)\\}').matchAll(/"([^"]*)"/g)].map(m => m[1])) === JSON.stringify(comp.DEFAULT), 'the reactor (the constructor with nothing made) matches effects.c COMP_DEFAULT');
check(+tbl('#define LP_MOD (\\d+)') === comp.MOD && +tbl('#define COMP_MAX (\\d+)') === comp.MAX, 'the animated settings and the layer count match');
// a layer written back to words reads the same, and stays a config line
let rt = true;
for (const s of [...comp.DEFAULT, 's:dots c:#12ab9F n:12 w:35 sf:40 b:mask inv:1 o:55', 's:wave c:p7 a:270 sz:400 v:-200 ap:sz aa:-400 at:0.2 mp:o ma:80 ms:beat b:screen',
                 's:spark x:1e9 n:0 life:abc tail:-3 hide:1 c:#zzz b:what', 's:grad v:12.345678']) {
  const a = comp.parse(s), b = comp.parse(comp.line(a));   // (written with 2 decimals: read once, then the same)
  if (comp.line(a) !== comp.line(b) || JSON.stringify(b) !== JSON.stringify(comp.parse(comp.line(b))) || comp.line(a).length > 240) { rt = false; console.log('     ' + s + ' -> ' + comp.line(a)); }
}
check(rt && comp.parse('x:5') === null && comp.parse('') === null, 'a layer written back to words reads the same; no shape, no layer');

// ---- colours
const file = process.argv[2];
if (!file) { console.log('usage: node preview_check.js <dump.json from effects_test.exe dump>'); process.exit(2); }
const cases = JSON.parse(fs.readFileSync(file, 'utf8'));
const TOL = 0.004;   // 1/255: float (C) against double (JS) rounding, nothing more
const worst = {};
for (const k of cases) {
  const P = k.id === 'wave' ? (key => key === 'dir' ? k.p.dir || 'right' : +(k.p.size || 2)) : (key => fxParamValue(k.id, key, k.p[key]));
  const js = fxPreview(k.id, k.pal.map(c => c.map(v => v * 255)), Math.fround(k.t), k.c.length, P);
  let d = 0, at = -1;
  k.c.forEach((c, i) => c.forEach((v, j) => { const e = Math.abs(v - js[i][j] / 255); if (e > d) { d = e; at = i; } }));
  const name = k.id + (Object.keys(k.p).length ? ' ' + JSON.stringify(k.p) : '');
  if (!worst[name] || d > worst[name].d) worst[name] = { d, at, t: k.t };
}
let bad = 0;
for (const [name, w] of Object.entries(worst)) if (w.d > TOL) { bad++; console.log(`     ${name}: off by ${w.d.toFixed(4)} at LED ${w.at}, t=${w.t}`); }
const all = Object.values(worst).reduce((m, w) => Math.max(m, w.d), 0);
check(!bad, `the previews show the engine's colours (${Object.keys(worst).length} cases x 4 moments, worst ${all.toFixed(5)})`);
process.exit(fails ? 1 : 0);
