// Scenarios (haku Pro): their list and editor in the profile menu's sheet. A plain script loaded before app.js: at the
// top level only declarations (they use app.js's helpers and state when called, never while loading); rulesBind() at the end.

// ---- scenarios (haku Pro, pro/rules.c): [rule.N], the order is the priority. A profile and the brightness switched by
// what the PC is doing. The core checks and keeps them (rule_save / rule_on / rule_move / rule_delete; setCfg on them is
// dropped) and says which hold in the status (rules: {on, win, by, list}); the open build has none ("none": 1).
const RULE_MAX = 16, RULE_C = 6, RULE_A = 8;
const RULES = { edit: null, req: 0, wait: 0, waitT: 0, err: '', sure: 0, sig: '', focus: null, after: null };
const ruleSec = n => 'rule.' + n;
function rulesHere() { return !!(S.rules && !S.rules.none); }
function ruleHas() { return rulesHere() && Array.from({ length: 16 }, (_, i) => cv('rule.' + (i + 1), 'name', '')).some(Boolean); }   // (called by renderProfiles, maybe before this runs)
const ruleInt = (v, lo, hi) => /^\d+$/.test(String(v).trim()) && +v >= lo && +v <= hi;
const ruleClean = s => String(s || '').replace(/[;#\[\]=\x00-\x1f]/g, ' ').replace(/\s+/g, ' ').trim();
// "game, Steam.exe" -> ["game.exe", "Steam.exe"]: the core compares whole exe names
const ruleApps = s => profApps(ruleClean(s)).map(a => /\.[a-z0-9]{1,4}$/i.test(a) ? a : a + '.exe');
function ruleItem(v) {
  const m = v.trim().match(/^(\S+)\s*(.*)$/) || ['', '', ''], w = m[1].toLowerCase(), r = m[2].trim(), n = r.split(/\s+/);
  if (w === 'run' || w === 'fg') return { t: w, apps: r };
  if (w === 'time') { const x = r.match(/^(\d{1,2}):(\d\d)\s+(\d{1,2}):(\d\d)\s*([1-7]*)$/); if (x) return { t: w, from: x[1].padStart(2, '0') + ':' + x[2], to: x[3].padStart(2, '0') + ':' + x[4], days: x[5] }; }
  if (w === 'idle') return { t: w, min: n[0] };
  if (w === 'lock') return { t: w, on: r === '1' };
  if (w === 'full') return { t: w };
  if (w === 'profile') return { t: w, id: +n[0] || 0 };
  if (w === 'bright') return { t: w, pct: n[0] };
  if (w === 'fade') return { t: w, pct: n[0], sec: n[1] };
  if (w === 'wait') return { t: w, sec: n[0] };
  return { t: '?' };
}
const ruleStr = i => i.t === 'run' || i.t === 'fg' ? `${i.t} ${ruleApps(i.apps).join(', ')}` : i.t === 'time' ? `time ${i.from} ${i.to}${i.days ? ' ' + i.days : ''}`
  : i.t === 'idle' ? `idle ${+i.min}` : i.t === 'lock' ? `lock ${i.on ? 1 : 0}` : i.t === 'full' ? 'full' : i.t === 'profile' ? `profile ${i.id}`
  : i.t === 'bright' ? `bright ${+i.pct}` : i.t === 'fade' ? `fade ${+i.pct} ${+i.sec}` : `wait ${+i.sec}`;
function ruleParse(n) {
  const g = k => cv(ruleSec(n), k, '');
  const r = { n, name: g('name'), on: g('on') !== '0', any: g('mode').toLowerCase() === 'any', restore: g('restore') === '1', c: [], a: [] };
  for (let k = 1; k <= RULE_C; k++) if (g('c' + k)) r.c.push(ruleItem(g('c' + k)));
  for (let k = 1; k <= RULE_A; k++) if (g('a' + k)) r.a.push(ruleItem(g('a' + k)));
  return r;
}
const rulesAll = () => Array.from({ length: RULE_MAX }, (_, i) => i + 1).filter(n => cv(ruleSec(n), 'name', '')).map(ruleParse);
// in words, for the list
const ruleDays = d => d && d.length < 7 ? ' ' + t('rules.c.days', [...d].map(x => dayName(x).slice(0, 2)).join(' ')) : '';
function ruleCondText(c) {
  const apps = () => profApps(c.apps).map(a => a.replace(/\.exe$/i, '')).join(', ');
  if (c.t === 'run' || c.t === 'fg') return t('rules.c.' + c.t + '.t', apps());
  if (c.t === 'time') return t('rules.c.time.t', c.from, c.to) + ruleDays(c.days);
  if (c.t === 'idle') return t('rules.c.idle.t', +c.min);
  if (c.t === 'lock') return t(c.on ? 'rules.c.lock.t' : 'rules.c.unlock.t');
  if (c.t === 'full') return t('rules.c.full.t');
  return t('rules.bad.item');
}
function ruleStepText(a) {
  if (a.t === 'profile') { const p = profiles().find(q => q.id === a.id); return p ? t('rules.a.profile.t', p.name) : t('rules.a.gone'); }
  if (a.t === 'bright') return t('rules.a.bright.t', +a.pct);
  if (a.t === 'fade') return t('rules.a.fade.t', +a.pct, +a.sec);
  if (a.t === 'wait') return t('rules.a.wait.t', +a.sec);
  return t('rules.bad.item');
}
// why it is on or not: [text, class]
function ruleWhy(r) {
  const R = S.rules, s = (R.list || [])[r.n - 1] || { cv: [] };
  if (!R.on) return [t(proLocked() ? 'rules.st.pro' : 'rules.st.paused'), 'off'];
  if (!r.on) return [t('rules.st.off'), 'off'];
  if (s.bad) return [t('rules.st.bad'), 'warn'];
  if (s.err) return [t('rules.st.err'), 'warn'];
  if (R.win === r.n) return [t('rules.st.win') + (R.by === r.n ? ' ' + t('rules.st.by') : ''), 'on'];
  if (s.act && s.yield) return [t('rules.st.yield'), ''];
  if (s.act) return [t('rules.st.above'), ''];
  if (r.any) return [t('rules.st.none'), ''];
  return [t('rules.st.wait', r.c.filter((c, k) => !s.cv[k]).map(ruleCondText).join(', ')), ''];
}
// the list (drawn again only when something in it changed, keeping the focus) or the editor
function renderRules(force) {
  const here = rulesHere();
  $('#rules-open').classList.toggle('hidden', !here);
  if (!here) { if (SHEET.kind === 'rules') closeSheet(); return; }
  if (RULES.edit || (!force && SHEET.kind !== 'rules')) return;   // closed: drawn when opened   // the editor is drawn by its own actions, the status does not touch what is typed
  const L = rulesAll(), lock = proLocked(), dis = lock ? ' disabled' : '';
  const sig = JSON.stringify([L, S.rules, lock, profiles().map(p => [p.id, p.name]), LANG, RULES.err, RULES.sure, RULES.wait]);
  if (!force && sig === RULES.sig) return;
  RULES.sig = sig;
  const el = $('#rules-panel'), a = document.activeElement, keep = RULES.focus || (el.contains(a) && a.dataset.act ? [a.dataset.act, a.dataset.n] : null);
  RULES.focus = null;
  const join = r => r.c.map(ruleCondText).join(t(r.any ? 'rules.or' : 'rules.and'));
  el.innerHTML = `<h3 id="rules-h">${t('rules.title')}</h3><p class="hint rules-note">${t('rules.note')}</p>
    ${lock ? `<div class="rules-lock"><div><b>${t('rules.lock')}</b><p class="muted">${t('rules.lock.text')}</p></div><div class="btn-row"><button class="btn small primary" type="button" data-act="buy">${t('pro.buy')}</button><button class="btn small ghost" type="button" data-act="key">${t('pro.lock.key')}</button></div></div>` : ''}
    ${S.rules.on || lock || !L.length ? '' : `<p class="hint warn">${t('rules.paused')}</p>`}
    <div class="rules-list">${L.map((r, i) => { const [why, cls] = ruleWhy(r), nm = esc(r.name); return `
      <div class="rule${r.on ? '' : ' off'}${cls === 'on' ? ' win' : ''}" data-n="${r.n}">
        <div class="rule-head">
          <label class="check"><input type="checkbox" data-act="on" data-n="${r.n}"${r.on ? ' checked' : ''}${lock && !r.on ? ' disabled' : ''}><span></span><em><b>${nm}</b></em></label>
          <span class="grow"></span>
          <button class="btn small" type="button" data-act="edit" data-n="${r.n}" aria-label="${t('rules.edit')}: ${nm}"${dis}><span>${t('rules.edit')}</span></button>
          <button class="btn small rule-mv" type="button" data-act="up" data-n="${r.n}" title="${t('rules.up')}" aria-label="${t('rules.up')}: ${nm}"${lock || !i ? ' disabled' : ''}>↑</button>
          <button class="btn small rule-mv" type="button" data-act="down" data-n="${r.n}" title="${t('rules.down')}" aria-label="${t('rules.down')}: ${nm}"${lock || i === L.length - 1 ? ' disabled' : ''}>↓</button>
          <button class="btn small danger" type="button" data-act="del" data-n="${r.n}" aria-label="${t(RULES.sure === r.n ? 'rules.del.sure' : 'rules.del')}: ${nm}"><span>${t(RULES.sure === r.n ? 'rules.del.sure' : 'rules.del')}</span></button>
        </div>
        <p class="rule-txt"><span>${t('rules.when')}</span> ${esc(join(r))}</p>
        <p class="rule-txt"><span>${t('rules.then')}</span> ${esc(r.a.map(ruleStepText).join(' → '))}${r.restore ? esc(' · ' + t('rules.restore.t')) : ''}</p>
        <p class="rule-st ${cls}">${esc(why)}</p>
      </div>`; }).join('') || `<p class="hint">${t('rules.empty')}</p>`}</div>
    ${RULES.err ? `<p class="hint warn" role="alert">${esc(RULES.err)}</p>` : ''}
    <div class="rules-new">
      <button class="btn small primary" type="button" data-act="new"${lock || L.length >= RULE_MAX ? ' disabled' : ''}><span>+ ${t('rules.new')}</span></button>
      <span class="muted">${t('rules.ex')}</span>
      ${['game', 'away', 'work'].map(x => `<button class="btn small ghost" type="button" data-act="ex" data-n="${x}"${lock || L.length >= RULE_MAX ? ' disabled' : ''}><span>${t('rules.ex.' + x)}</span></button>`).join('')}
    </div>
    ${L.length >= RULE_MAX ? `<p class="hint">${t('rules.full', RULE_MAX)}</p>` : ''}`;
  // a move button that went off at the top or the bottom passes the focus to the other one
  const at = a => $(`#rules-panel [data-act="${a}"][data-n="${keep[1]}"]:not(:disabled)`);
  const f = keep && (at(keep[0]) || (keep[0] === 'up' || keep[0] === 'down') && at(keep[0] === 'up' ? 'down' : 'up') || $('#rules-panel [data-act="new"]'));
  if (f && !f.disabled) f.focus({ preventScroll: true });
}
// ---- the editor: what is typed is read back into RULES.edit before anything is drawn again
const RULE_CT = ['run', 'fg', 'time', 'idle', 'lock', 'full'], RULE_AT = ['profile', 'bright', 'fade', 'wait'];
const ruleDef = k => ({ t: k, ...{ run: { apps: '' }, fg: { apps: '' }, time: { from: '18:00', to: '23:00', days: '' }, idle: { min: 10 }, lock: { on: true }, full: {},
  profile: { id: (profiles()[0] || { id: 0 }).id }, bright: { pct: 100 }, fade: { pct: 50, sec: 3 }, wait: { sec: 5 } }[k] });
function ruleEdRead() {
  const E = RULES.edit, ed = $('#rules-panel .rule-ed'); if (!E || !ed) return;
  E.name = $('.re-name', ed).value; E.restore = $('.re-restore', ed).checked;
  const mode = $('.re-mode', ed); if (mode) E.any = mode.value === 'any';
  $$('.re-c', ed).forEach((row, k) => {
    const c = E.c[k], f = s => ($(s, row) || {}).value ?? '';
    if (c.t === 'run' || c.t === 'fg') c.apps = f('.re-apps');
    else if (c.t === 'time') { c.from = f('.re-from'); c.to = f('.re-to'); }
    else if (c.t === 'idle') c.min = f('.re-min');
    else if (c.t === 'lock') c.on = f('.re-lock') === '1';
  });
  $$('.re-a', ed).forEach((row, k) => {
    const a = E.a[k], f = s => ($(s, row) || {}).value ?? '';
    if (a.t === 'profile') a.id = +f('.re-prof') || 0;
    else if (a.t === 'bright') a.pct = f('.re-pct');
    else if (a.t === 'fade') { a.pct = f('.re-pct'); a.sec = f('.re-sec'); }
    else if (a.t === 'wait') a.sec = f('.re-sec');
  });
}
function renderRuleEd(focusSel) {
  const E = RULES.edit, el = $('#rules-panel'); if (!E) return;
  const P = profiles(), opt = (v, cur, txt) => `<option value="${v}"${String(v) === String(cur) ? ' selected' : ''}>${esc(txt)}</option>`;
  const n = (cls, v, lo, hi, lbl) => `<input type="number" class="${cls}" min="${lo}" max="${hi}" step="1" value="${esc(v)}" aria-label="${lbl}">`;
  const cond = (c, k) => {
    const lbl = t('rules.c.n', k + 1);
    let f = '';
    if (c.t === 'run' || c.t === 'fg') f = `<input type="text" class="re-apps" spellcheck="false" maxlength="240" value="${esc(c.apps)}" placeholder="game.exe, steam.exe" aria-label="${t('rules.c.apps')}">`;
    else if (c.t === 'time') f = `<input type="time" class="re-from" value="${esc(c.from)}" aria-label="${t('rules.c.from')}"><span>–</span><input type="time" class="re-to" value="${esc(c.to)}" aria-label="${t('rules.c.to')}">`;
    else if (c.t === 'idle') f = n('re-min', c.min, 1, 1440, t('rules.c.min')) + `<span>${t('rules.min')}</span>`;
    else if (c.t === 'lock') f = `<select class="select re-lock" aria-label="${t('rules.c.lock')}">${opt(1, c.on ? 1 : 0, t('rules.c.lock.1'))}${opt(0, c.on ? 1 : 0, t('rules.c.lock.0'))}</select>`;
    const days = c.t !== 'time' ? '' : `<div class="prof-days" role="group" aria-label="${t('prof.days')}">${[1, 2, 3, 4, 5, 6, 7].map(d => `<button class="prof-day" type="button" data-act="day" data-n="${k}" data-d="${d}" aria-pressed="${!c.days || c.days.includes(d)}">${esc(dayName(d))}</button>`).join('')}</div>`;
    return `<div class="re-row re-c"><select class="select re-ct" data-n="${k}" aria-label="${lbl}">${RULE_CT.map(x => opt(x, c.t, t('rules.c.' + x))).join('')}</select>${f && `<span class="re-v">${f}</span>`}
      <button class="x" type="button" data-act="c-del" data-n="${k}" title="${t('rules.remove')}" aria-label="${t('rules.remove')}: ${lbl}">×</button>${days}</div>`;
  };
  const step = (a, k) => {
    const lbl = t('rules.a.n', k + 1), gone = !P.some(p => p.id === a.id);
    let f = '';
    if (a.t === 'profile') f = `<select class="select re-prof" aria-label="${t('rules.a.prof')}">${gone ? opt(0, 0, t(P.length ? 'rules.a.pick' : 'rules.a.noprof')) : ''}${P.map(p => opt(p.id, a.id, p.name)).join('')}</select>`;
    else if (a.t === 'bright') f = n('re-pct', a.pct, 5, 100, t('rules.a.pct')) + '<span>%</span>';
    else if (a.t === 'fade') f = n('re-pct', a.pct, 5, 100, t('rules.a.pct')) + `<span>%, ${t('rules.a.over')}</span>` + n('re-sec', a.sec, 1, 3600, t('rules.a.sec')) + `<span>${t('rules.s')}</span>`;
    else if (a.t === 'wait') f = n('re-sec', a.sec, 1, 3600, t('rules.a.sec')) + `<span>${t('rules.s')}</span>`;
    return `<div class="re-row re-a"><select class="select re-at" data-n="${k}" aria-label="${lbl}">${RULE_AT.map(x => opt(x, a.t, t('rules.a.' + x))).join('')}</select>${f && `<span class="re-v">${f}</span>`}
      <button class="x" type="button" data-act="a-del" data-n="${k}" title="${t('rules.remove')}" aria-label="${t('rules.remove')}: ${lbl}">×</button></div>`;
  };
  el.innerHTML = `<h3 id="rules-h">${t(E.n ? 'rules.ed.edit' : 'rules.ed.new')}</h3>
    <div class="rule-ed">
      <label class="re-fld"><span>${t('rules.name')}</span><input type="text" class="re-name" maxlength="64" spellcheck="false" value="${esc(E.name)}"></label>
      <fieldset><legend>${t('rules.when')}</legend>
        ${E.c.length > 1 ? `<select class="select re-mode" aria-label="${t('rules.mode')}">${opt('all', E.any ? 'any' : 'all', t('rules.mode.all'))}${opt('any', E.any ? 'any' : 'all', t('rules.mode.any'))}</select>` : ''}
        ${E.c.map(cond).join('')}
        <button class="btn small ghost" type="button" data-act="c-add"${E.c.length >= RULE_C ? ' disabled' : ''}><span>+ ${t('rules.c.add')}</span></button>
        <small class="hint">${t('rules.c.note')}</small>
      </fieldset>
      <fieldset><legend>${t('rules.then')}</legend>
        ${E.a.map(step).join('')}
        <button class="btn small ghost" type="button" data-act="a-add"${E.a.length >= RULE_A ? ' disabled' : ''}><span>+ ${t('rules.a.add')}</span></button>
      </fieldset>
      <label class="check"><input type="checkbox" class="re-restore"${E.restore ? ' checked' : ''}><span></span><em>${t('rules.restore')}</em></label>
      <p class="hint warn re-err" role="alert">${esc(RULES.err)}</p>
      <div class="btns">
        <button class="btn small" type="button" data-act="cancel"><span>${t('cancel')}</span></button>
        <span class="grow"></span>
        <button class="btn primary small" type="button" data-act="save"${RULES.wait ? ' disabled aria-busy="true"' : ''}><span>${t('rules.save')}</span></button>
      </div>
    </div>`;
  const f = focusSel && $('#rules-panel ' + focusSel);
  if (f) f.focus();
}
function ruleEdit(r) {
  RULES.edit = r ? JSON.parse(JSON.stringify(r)) : { n: 0, name: '', on: true, any: false, restore: true, c: [ruleDef('run')], a: [ruleDef('profile')] };
  RULES.edit.c = RULES.edit.c.filter(x => x.t !== '?'); RULES.edit.a = RULES.edit.a.filter(x => x.t !== '?');
  RULES.err = ''; RULES.sure = 0;
  renderRuleEd('.re-name');
}
function ruleEdClose() { RULES.edit = null; RULES.err = ''; renderRules(true); }
// the examples only fill the editor: nothing is kept before Save
function ruleExample(x) {
  const P = profiles(), like = re => (P.find(p => re.test(p.name)) || P[0] || null);
  const prof = p => p ? [{ t: 'profile', id: p.id }] : [{ t: 'bright', pct: 100 }];
  const ex = {
    game: { name: t('rules.ex.game'), any: false, c: [{ t: 'full' }, { t: 'time', from: '18:00', to: '02:00', days: '' }], a: [...prof(like(/gam|игр|jeu/i)), { t: 'fade', pct: 100, sec: 2 }] },
    away: { name: t('rules.ex.away'), any: true, c: [{ t: 'idle', min: 10 }, { t: 'lock', on: true }], a: [{ t: 'fade', pct: 10, sec: 5 }] },
    work: { name: t('rules.ex.work'), any: false, c: [{ t: 'time', from: '09:00', to: '18:00', days: '12345' }], a: [...prof(like(/work|раб|trav/i)), { t: 'bright', pct: 60 }] },
  }[x];
  ruleEdit({ n: 0, on: true, restore: true, ...ex });
}
// checked here so the field says what is wrong; the core checks again (the page is not trusted with the format)
function ruleEdSave() {
  ruleEdRead();
  const E = RULES.edit, ed = $('#rules-panel .rule-ed');
  $$('input, select', ed).forEach(i => i.setCustomValidity(''));
  let bad = null;
  const no = (el, key) => { if (!bad && el) { el.setCustomValidity(t(key)); bad = el; } };
  if (!ruleClean(E.name)) no($('.re-name', ed), 'rules.v.name');
  const C = $$('.re-c', ed), A = $$('.re-a', ed);
  E.c.forEach((c, k) => {
    const row = C[k];
    if (c.t === 'run' || c.t === 'fg') { const L = ruleApps(c.apps); if (!L.length || L.length > 8 || L.some(a => /[\\/:*?"<>|]/.test(a)) || ruleStr(c).length > 250) no($('.re-apps', row), 'rules.v.apps'); }
    if (c.t === 'time' && !(/^\d\d:\d\d$/.test(c.from) && /^\d\d:\d\d$/.test(c.to) && c.from !== c.to)) no($('.re-to', row), 'rules.v.time');
    if (c.t === 'idle' && !ruleInt(c.min, 1, 1440)) no($('.re-min', row), 'rules.v.idle');
  });
  E.a.forEach((a, k) => {
    const row = A[k];
    if (a.t === 'profile' && !profiles().some(p => p.id === a.id)) no($('.re-prof', row), 'rules.v.prof');
    if ((a.t === 'bright' || a.t === 'fade') && !ruleInt(a.pct, 5, 100)) no($('.re-pct', row), 'rules.v.pct');
    if ((a.t === 'fade' || a.t === 'wait') && !ruleInt(a.sec, 1, 3600)) no($('.re-sec', row), 'rules.v.sec');
  });
  if (!bad && !E.c.length) no($('[data-act="c-add"]', ed), 'rules.v.nocond');
  if (!bad && !E.a.length) no($('[data-act="a-add"]', ed), 'rules.v.nostep');
  if (bad) { $('.re-err', ed).textContent = bad.validationMessage || ''; bad.focus(); return; }
  const kv = [`name=${ruleClean(E.name).slice(0, 64)}`, `on=${E.on ? 1 : 0}`, `mode=${E.any && E.c.length > 1 ? 'any' : 'all'}`, `restore=${E.restore ? 1 : 0}`,
    ...E.c.map((c, k) => `c${k + 1}=${ruleStr(c)}`), ...E.a.map((a, k) => `a${k + 1}=${ruleStr(a)}`)];
  ruleSend({ cmd: 'rule_save', n: String(E.n || 0), kv: kv.join('\n') });
  renderRuleEd();
}
// one change at a time; no answer in 5 s counts as "not saved"
function ruleSend(o) {
  if (RULES.wait) return;
  RULES.wait = ++RULES.req; RULES.err = '';
  clearTimeout(RULES.waitT); RULES.waitT = setTimeout(() => rulesDone({ req: String(RULES.wait), err: 'save', n: 0 }), 5000);
  send({ ...o, req: String(RULES.wait) });
}
function rulesDone(m) {
  if (!RULES.wait || String(m.req) !== String(RULES.wait)) return;
  RULES.wait = 0; clearTimeout(RULES.waitT);
  RULES.err = m.err ? t('rules.err.' + m.err) : '';
  if (!m.err && RULES.after) RULES.focus = RULES.after;   // the moved rule keeps the focus in its new place
  RULES.after = null;
  if (RULES.edit) {
    if (!m.err) { RULES.focus = ['edit', String(m.n)]; ruleEdClose(); }
    else renderRuleEd('[data-act="save"]');
    return;
  }
  renderRules(true);
}
// from the profile menu or the Home line: the list, its first button focused
function rulesOpen(r) {
  openSheet({ k: 'rules' }, r.left + r.width / 2, r.top);
  RULES.edit = null; RULES.err = ''; RULES.sure = 0; renderRules(true);
  queueMicrotask(() => { const f = $('#rules-panel [data-act="new"]:not(:disabled)') || $('#rules-panel button:not(:disabled)'); if (f) f.focus(); });
}

// the panel's and the profile menu's handlers: app.js calls this once its helpers are there
function rulesBind() {
  $('#rules-panel').addEventListener('click', e => {
    const b = e.target.closest('[data-act]'); if (!b || b.disabled || b.tagName === 'INPUT') return;
    const act = b.dataset.act, n = b.dataset.n;
    if (act === 'buy') { send({ cmd: 'open', what: 'pro' }); return; }
    if (act === 'key') { closeSheet(); proGoKey(); return; }
    if (RULES.edit) {
      ruleEdRead();
      const E = RULES.edit;
      if (act === 'cancel') { RULES.focus = E.n ? ['edit', String(E.n)] : ['new', undefined]; ruleEdClose(); return; }
      if (act === 'save') { ruleEdSave(); return; }
      if (act === 'c-add') { E.c.push(ruleDef('run')); renderRuleEd(`.re-ct[data-n="${E.c.length - 1}"]`); return; }
      if (act === 'a-add') { E.a.push(ruleDef('bright')); renderRuleEd(`.re-at[data-n="${E.a.length - 1}"]`); return; }
      if (act === 'c-del') { E.c.splice(+n, 1); renderRuleEd(E.c.length ? `.re-ct[data-n="${Math.min(+n, E.c.length - 1)}"]` : '[data-act="c-add"]'); return; }
      if (act === 'a-del') { E.a.splice(+n, 1); renderRuleEd(E.a.length ? `.re-at[data-n="${Math.min(+n, E.a.length - 1)}"]` : '[data-act="a-add"]'); return; }
      if (act === 'day') {   // at least one day; all seven are written as none (every day)
        const c = E.c[+n], was = c.days || '1234567', on = [...'1234567'].filter(d => was.includes(d) !== (d === b.dataset.d));
        if (!on.length) return;
        c.days = on.length === 7 ? '' : on.join(''); renderRuleEd(`.prof-day[data-n="${n}"][data-d="${b.dataset.d}"]`);
      }
      return;
    }
    if (act === 'new') { ruleEdit(null); return; }
    if (act === 'ex') { ruleExample(n); return; }
    const r = rulesAll().find(x => x.n === +n); if (!r) return;
    if (act === 'edit') { ruleEdit(r); return; }
    if (RULES.wait) return;   // one change at a time: the next waits for the core's answer
    if (act === 'up' || act === 'down') { RULES.sure = 0; RULES.after = [act, String(r.n + (act === 'up' ? -1 : 1))]; ruleSend({ cmd: 'rule_move', n, d: act === 'up' ? '-1' : '1' }); renderRules(true); return; }
    if (act === 'del') {
      if (RULES.sure !== r.n) { RULES.sure = r.n; renderRules(true); return; }
      RULES.sure = 0; RULES.focus = ['new', undefined]; ruleSend({ cmd: 'rule_delete', n }); renderRules(true);
    }
  });
  $('#rules-panel').addEventListener('change', e => {
    const s = e.target;
    if (s.dataset.act === 'on') { ruleSend({ cmd: 'rule_on', n: s.dataset.n, on: s.checked ? '1' : '0' }); renderRules(true); return; }
    if (!RULES.edit || !s.matches('.re-ct, .re-at')) return;
    ruleEdRead();
    const k = +s.dataset.n, L = s.matches('.re-ct') ? RULES.edit.c : RULES.edit.a;
    L[k] = ruleDef(s.value);
    renderRuleEd(`.${s.matches('.re-ct') ? 're-ct' : 're-at'}[data-n="${k}"]`);
  });
  $('#rules-panel').addEventListener('input', e => { if (e.target.setCustomValidity) e.target.setCustomValidity(''); });
  $('#rules-panel').addEventListener('keydown', e => {
    if (e.key === 'Escape' && RULES.edit && !e.target.matches('select')) { e.stopPropagation(); RULES.focus = RULES.edit.n ? ['edit', String(RULES.edit.n)] : ['new', undefined]; ruleEdClose(); return; }   // back to the list, not out of the window
    if (e.key === 'Enter' && RULES.edit && e.target.matches('input[type=text], input[type=number], input[type=time]')) { e.preventDefault(); ruleEdSave(); }
  });
  $('#rules-open').addEventListener('click', e => { const r = e.currentTarget.getBoundingClientRect(); profMenu(false); $('#prof-btn').focus(); rulesOpen(r); });
}
