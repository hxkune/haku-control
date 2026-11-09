// SPDX-License-Identifier: GPL-3.0-only
// The tray panel (ui_tray.cpp): a small page of its own, not the window's. It shows what the core sends
// (app_tray_json) and sends the window's commands (ui_dispatch); a command is only "done" once the state says so.
'use strict';
const T = {
  en: { on: 'Lighting is on', off: 'Lighting is off', power: 'Lighting', bri: 'Brightness', prof: 'Profile', profs: 'Profiles',
    allProf: 'All profiles', fx: 'Effect', fxs: 'Effects', presets: 'Your presets', preset: 'preset', devs: 'Devices',
    allDev: 'Show all {0} devices', noDev: 'No devices yet', open: 'Open haku control', more: 'More', back: 'Back',
    settings: 'Settings', pc: 'PC lighting control', pcGive: 'Hand over to the devices\' own apps', pcTake: 'Light with haku',
    pcNone: 'No PC lighting found', pcNow: 'PC devices left to their own apps: {0} of {1}', help: 'Open help', diag: 'Save diagnostics',
    log: 'Open the log', ini: 'Open the settings file', quit: 'Quit haku control', offFx: 'Off', sending: 'Sending…',
    notDone: 'haku did not confirm this. Shown as it really is.', devGone: 'The device disconnected. The change could not be confirmed.', s_on: 'On', s_off: 'Off', s_own: 'Left to its own app',
    s_busy: 'Driven another way in haku', s_pro: 'Pro is needed to control it', s_nosup: 'Not supported here', s_offline: 'Control on · no connection', s_connecting: 'Waiting to connect',
    fxFind: 'Find an effect', favs: 'Favourites', recent: 'Recent', allFx: 'All effects', noMatch: 'Nothing matches “{0}”',
    pin: 'Keep {0} on the first screen', pinHint: 'Pin up to 3 devices to keep them on the first screen.',
    'err.write': 'The profile file could not be written.', 'err.leave': 'Not switched: the changes in “{0}” could not be saved.',
    'err.missing': 'Profile “{0}” was not used: its file is missing.', 'err.bad': 'Profile “{0}” was not used: its file is damaged.',
    'err.cfg': 'Switched, but the settings file could not be written yet.', 'err.full': 'All profiles are taken.' },
  ru: { on: 'Подсветка включена', off: 'Подсветка выключена', power: 'Подсветка', bri: 'Яркость', prof: 'Профиль', profs: 'Профили',
    allProf: 'Все профили', fx: 'Эффект', fxs: 'Эффекты', presets: 'Ваши пресеты', preset: 'пресет', devs: 'Устройства',
    allDev: 'Показать все устройства: {0}', noDev: 'Устройств пока нет', open: 'Открыть haku control', more: 'Ещё', back: 'Назад',
    settings: 'Настройки', pc: 'Управление подсветкой ПК', pcGive: 'Передать управление приложениям устройств', pcTake: 'Управлять через haku',
    pcNone: 'Подсветка ПК не найдена', pcNow: 'Устройств ПК у своих программ: {0} из {1}', help: 'Открыть справку', diag: 'Сохранить диагностику',
    log: 'Открыть журнал', ini: 'Открыть файл настроек', quit: 'Выйти из haku control', offFx: 'Выключено', sending: 'Отправка…',
    notDone: 'haku не подтвердил это. Показано настоящее состояние.', devGone: 'Устройство отключилось. Не удалось подтвердить изменение.', s_on: 'Включено', s_off: 'Выключено', s_own: 'У своей программы',
    s_busy: 'haku управляет им другим путём', s_pro: 'Для управления нужен Pro', s_nosup: 'Здесь не поддерживается', s_offline: 'Управление включено · нет связи', s_connecting: 'Ожидает подключения',
    fxFind: 'Найти эффект', favs: 'Избранное', recent: 'Недавние', allFx: 'Все эффекты', noMatch: 'Ничего не найдено по «{0}»',
    pin: 'Держать {0} на первом экране', pinHint: 'Закрепите до 3 устройств, чтобы они были на первом экране.',
    'err.write': 'Не удалось записать файл профиля.', 'err.leave': 'Не переключено: изменения в «{0}» не удалось сохранить.',
    'err.missing': 'Профиль «{0}» не использован: его файл не найден.', 'err.bad': 'Профиль «{0}» не использован: его файл повреждён.',
    'err.cfg': 'Переключено, но файл настроек пока не записан.', 'err.full': 'Все профили заняты.' },
  fr: { on: 'L’éclairage est allumé', off: 'L’éclairage est éteint', power: 'Éclairage', bri: 'Luminosité', prof: 'Profil', profs: 'Profils',
    allProf: 'Tous les profils', fx: 'Effet', fxs: 'Effets', presets: 'Vos préréglages', preset: 'préréglage', devs: 'Appareils',
    allDev: 'Afficher les {0} appareils', noDev: 'Aucun appareil pour l’instant', open: 'Ouvrir haku control', more: 'Plus', back: 'Retour',
    settings: 'Réglages', pc: 'Contrôle de l’éclairage du PC', pcGive: 'Rendre aux applications des appareils', pcTake: 'Éclairer avec haku',
    pcNone: 'Aucun éclairage du PC trouvé', pcNow: 'Appareils du PC laissés à leurs applications : {0} sur {1}', help: 'Ouvrir l’aide', diag: 'Enregistrer le diagnostic',
    log: 'Ouvrir le journal', ini: 'Ouvrir le fichier de réglages', quit: 'Quitter haku control', offFx: 'Éteint', sending: 'Envoi…',
    notDone: 'haku ne l’a pas confirmé. Affiché tel quel.', devGone: 'L’appareil s’est déconnecté. Le changement n’a pas pu être confirmé.', s_on: 'Allumé', s_off: 'Éteint', s_own: 'Laissé à son application',
    s_busy: 'haku le pilote par un autre chemin', s_pro: 'Pro requis pour le contrôler', s_nosup: 'Non pris en charge ici', s_offline: 'Contrôle activé · pas de connexion', s_connecting: 'En attente de connexion',
    fxFind: 'Chercher un effet', favs: 'Favoris', recent: 'Récents', allFx: 'Tous les effets', noMatch: 'Aucun résultat pour « {0} »',
    pin: 'Garder {0} sur le premier écran', pinHint: 'Épinglez jusqu’à 3 appareils pour les garder sur le premier écran.',
    'err.write': 'Le fichier du profil n’a pas pu être écrit.', 'err.leave': 'Pas de changement : les modifications de « {0} » n’ont pas pu être enregistrées.',
    'err.missing': 'Le profil « {0} » n’a pas été utilisé : son fichier est introuvable.', 'err.bad': 'Le profil « {0} » n’a pas été utilisé : son fichier est endommagé.',
    'err.cfg': 'Changé, mais le fichier de réglages n’a pas encore pu être écrit.', 'err.full': 'Tous les profils sont pris.' },
};
const PROF_ERR = ['', 'write', 'leave', 'missing', 'bad', 'full', 'cfg'];   // profile.c, as app.js
const THEMES = ['dark', 'grey', 'light'], HIDDEN = ['verity', 'bubblegum', 'kyoka', 'akapzz'];
const DEV_MAIN = 3, PIN_MAX = 3, CONFIRM_MS = 5000, BRI_MS = 80, BRI_HOLD = 600;
let L = T.en, S = null, view = 'main', lastH = 0, side = 'b';
// what was sent and not yet seen in the state: key -> { want(state), t, late }. want: true (done), false (not yet) or
// the key of an error that settles it now. Once failed it stays as late: a state that agrees later takes its error away.
const pend = {};
const errs = {};            // a section's last failure, shown under it until the next action there
let profBase = null;        // prof.n when the panel opened: a refusal raises it
let fxQ = '';               // the effects view's search, until the panel closes
let briDrag = 0, briHoldTill = 0, briTimer = 0, briLast = -1;

const $ = (s, r = document) => r.querySelector(s);
const t = (k, ...a) => (L[k] ?? T.en[k] ?? k).replace(/\{(\d)\}/g, (_, i) => a[i]);
const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const bridge = window.chrome && window.chrome.webview;
const send = o => { if (bridge) bridge.postMessage(JSON.stringify(o)); };
const isOn = () => S && S.fx !== S.off;
const parsePal = s => (s || '').match(/#[0-9a-fA-F]{6}/g) || [];
const sw = pal => pal.length ? `<span class="pal" aria-hidden="true">${pal.slice(0, 8).map(c => `<i style="background:${esc(c)}"></i>`).join('')}</span>` : '';
const ICON = {
  power: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 3v8M6.3 6.8a8 8 0 1 0 11.4 0"/></svg>',
  more: '<svg viewBox="0 0 24 24" aria-hidden="true"><circle cx="5" cy="12" r="1.6"/><circle cx="12" cy="12" r="1.6"/><circle cx="19" cy="12" r="1.6"/></svg>',
  next: '<svg class="chev" viewBox="0 0 24 24" aria-hidden="true"><path d="M9 6l6 6-6 6"/></svg>',
  back: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>',
 pin: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M9 4h6l-1 6 3 3H7l3-3zM12 13v7"/></svg>',
  check: '<svg class="tick" viewBox="0 0 24 24" aria-hidden="true"><path d="M5 12.5l4.5 4.5L19 7.5"/></svg>',
};

// ---- what was sent, and whether the state agrees yet
function sent(key, cmd, want, section, id) {
  delete errs[section];
  pend[key] = { want, t: Date.now(), section, id };
  send(cmd);
  render();
  setTimeout(() => { check(); render(); }, CONFIRM_MS + 50);   // one timer per command, not a loop
}
function check() {
  const now = Date.now();
  for (const [k, p] of Object.entries(pend)) {
    const r = S ? p.want(S) : false;
    if (r === true) { if (p.late && errs[p.section] === p.late) delete errs[p.section]; delete pend[k]; }
    else if (p.late) continue;
    else if (r || now - p.t >= CONFIRM_MS) { p.late = errs[p.section] = t(r || 'notDone'); say(p.late); }
  }
}
const busy = k => k in pend && !pend[k].late;
function say(msg) { const l = $('#live'); l.textContent = ''; setTimeout(() => { l.textContent = msg; }, 30); }

// ---- the core's state
function onState(s) {
  const first = !S;
  S = s;
  L = T[s.lang] || T.en;
  document.documentElement.lang = s.lang || 'en';
  const open = THEMES.concat(String(s.unlocked || '').split(',').filter(v => HIDDEN.includes(v)));
  const th = open.includes(s.theme) ? s.theme : 'dark';
  if (th === 'dark') delete document.documentElement.dataset.theme; else document.documentElement.dataset.theme = th;
  applyMotion();
  if (first || profBase === null) profBase = (s.prof || {}).n || 0;
  const pe = s.prof || {};
  if (pe.n > profBase) {   // profile.c refused something since the panel opened: say it, never show it as done
    profBase = pe.n;
    const k = PROF_ERR[pe.err] || 'write', p = (s.profiles || []).find(x => x.id === pe.id);
    errs.prof = t('err.' + k, p ? p.n : '#' + pe.id);
    delete pend.prof;
    say(errs.prof);
  }
  check();
  render();
}
function applyMotion() {
  const m = S ? S.motion : 'sys';
  const still = m === '0' || (m === 'sys' && matchMedia('(prefers-reduced-motion: reduce)').matches);
  document.body.classList.toggle('still', still);
}

// ---- drawing: each part is rebuilt only when its markup changed, so focus and scroll stay where they are
const parts = {};
function put(id, html) {
  const el = document.getElementById(id);
  if (!el || parts[id] === html) return false;
  const f = document.activeElement, key = f && el.contains(f) ? f.dataset.key : null;
  parts[id] = html; el.innerHTML = html;
  if (key) { const n = el.querySelector(`[data-key="${CSS.escape(key)}"]`); if (n) n.focus(); }
  return true;
}
const stLabel = s => t('s_' + s);
const devKey = d => d.k ? 'k:' + d.k : 'id:' + d.id;   // stable: a [layout] key or a [dev.N] number, never the list position
// the pinned devices still listed, in the order they were pinned ([general] tray_pin)
const pinsNow = () => String(S.dpin || '').split(',').filter(p => (S.devs || []).some(d => devKey(d) === p)).slice(0, PIN_MAX);
function devRow(d, pins) {
  const key = devKey(d), on = d.s !== 'off', can = d.s !== 'nosup', b = busy('dev' + key);
  return `<div class="row dev s-${esc(d.s)}${b ? ' busy' : ''}">
    <span class="dot" aria-hidden="true"></span>
    <span class="txt"><span class="name" title="${esc(d.n)}">${esc(d.n)}</span><small>${esc(b ? t('sending') : stLabel(d.s))}</small></span>
    ${can ? `<button class="switch" role="switch" type="button" aria-checked="${on}" aria-label="${esc(d.n)}" data-key="${esc(key)}" data-dev="${esc(key)}"${b ? ' aria-busy="true"' : ''}><i></i></button>` : ''}
    ${pins ? `<button class="pin icon" type="button" data-key="pin:${esc(key)}" data-pin="${esc(key)}" aria-pressed="${pins.includes(key)}" aria-label="${esc(t('pin', d.n))}"${!pins.includes(key) && pins.length >= PIN_MAX ? ' disabled' : ''}${busy('pin') ? ' aria-busy="true"' : ''}>${ICON.pin}</button>` : ''}
  </div>`;
}
const errLine = sec => errs[sec] ? `<p class="err" role="alert">${esc(errs[sec])}</p>` : '';
function profChip(p) {
  const cur = p.id === S.profile, b = busy('prof') && pend.prof.id === p.id;
  return `<button class="chip${cur ? ' on' : ''}${b ? ' busy' : ''}" type="button" data-key="p${p.id}" data-prof="${p.id}" aria-pressed="${cur}" title="${esc(p.n)}"><span>${esc(p.n)}</span></button>`;
}
function fxName() {
  if (!isOn()) return t('offFx');
  const pr = S.preset && (S.presets || []).find(p => p.id === S.preset);
  if (pr) return pr.n;
  const f = (S.fxs || []).find(x => x.id === S.fx);
  return f ? f.t : S.fx;
}

// the effects view: favourite and recent ones first (each effect once), then the rest and the presets; a search
// narrows all of them by name
function fxList(on) {
  const fxBusy = busy('fx') ? pend.fx.id : null, F = S.fxs || [], P = S.presets || [];
  const row = f => {
    const cur = on && f.id === S.fx && !S.preset;
    return `<button class="row pick${cur ? ' on' : ''}${fxBusy === 'f' + f.id ? ' busy' : ''}" type="button" data-key="f${esc(f.id)}" data-fx="${esc(f.id)}" aria-current="${cur}"><span class="name" title="${esc(f.t)}">${esc(f.t)}</span>${cur ? sw(S.pal || []) : ''}${ICON.check}</button>`;
  };
  const preRow = p => {
    const cur = on && p.id === S.preset, f = F.find(x => x.id === p.fx);
    return `<button class="row pick${cur ? ' on' : ''}${fxBusy === 'p' + p.id ? ' busy' : ''}" type="button" data-key="r${p.id}" data-preset="${p.id}" aria-current="${cur}" aria-description="${esc(t('preset') + (f ? ', ' + f.t : ''))}"><span class="txt"><span class="name" title="${esc(p.n)}">${esc(p.n)}</span>${f ? `<small>${esc(f.t)}</small>` : ''}</span>${sw(parsePal(p.pal))}${ICON.check}</button>`;
  };
  const list = (h, a, fn) => a.length ? `${h ? `<h3>${esc(h)}</h3>` : ''}<div class="list">${a.map(fn).join('')}</div>` : '';
  const q = fxQ.trim().toLocaleLowerCase();
  if (q) {
    const hit = s => String(s).toLocaleLowerCase().includes(q), fs = F.filter(f => hit(f.t) || hit(f.id)), ps = P.filter(p => hit(p.n));
    return errLine('fx') + (fs.length || ps.length ? list('', fs, row) + list(t('presets'), ps, preRow) : `<p class="muted">${esc(t('noMatch', fxQ.trim()))}</p>`);
  }
  const ids = s => String(s || '').split(',').map(id => F.find(f => f.id === id)).filter(Boolean);
  const fav = ids(S.fav), rec = ids(S.recent).filter(f => !fav.includes(f)).slice(0, 3), top = fav.concat(rec);
  return errLine('fx') + list(t('favs'), fav, row) + list(t('recent'), rec, row) + list(top.length ? t('allFx') : '', F.filter(f => !top.includes(f)), row) + list(t('presets'), P, preRow);
}

function render() {
  if (!S) return;
  document.body.dataset.view = view;
  const on = isOn(), pb = busy('power');
  put('head', `<span class="logo" role="img" aria-label="haku control"></span>
    <span class="state" id="state-txt">${esc(pb ? t('sending') : t(on ? 'on' : 'off'))}</span>
    <button class="power${on ? ' on' : ''}${pb ? ' busy' : ''}" type="button" data-key="power" id="power" aria-pressed="${on}" aria-label="${esc(t('power'))}" aria-describedby="state-txt">${ICON.power}</button>`);
  // the slider is made once; only its value and label follow the state
  const br = $('#bri');
  if (!briDrag && Date.now() > briHoldTill && +br.value !== S.bri) br.value = S.bri;
  br.setAttribute('aria-label', t('bri'));
  br.setAttribute('aria-valuetext', br.value + '%');
  $('#bri-l').textContent = t('bri');
  $('#bri-v').textContent = br.value + '%';
  put('err-power', errLine('power'));
  // profiles: the pinned ones (at most 3), else the one in use, and the whole list one step further
  const P = S.profiles || [];
  let shown = P.filter(p => p.pin).slice(0, 3);
  if (!shown.length) shown = P.filter(p => p.id === S.profile);
  $('#sec-prof').hidden = !P.length;
  put('prof', `<div class="sec-h"><h2>${esc(t('prof'))}</h2>${P.length > shown.length ? `<button class="link" type="button" data-key="allprof" data-go="profiles">${esc(t('allProf'))}${ICON.next}</button>` : ''}</div>
    <div class="chips">${shown.map(profChip).join('')}</div>${errLine('prof')}`);
  put('fx', `<button class="row go" type="button" data-key="fx" data-go="effects"${busy('fx') ? ' aria-busy="true"' : ''}>
    <span class="txt"><small>${esc(t('fx'))}</small><span class="name" title="${esc(fxName())}">${esc(busy('fx') ? t('sending') : fxName())}</span></span>
    ${on ? sw(S.pal || []) : ''}${ICON.next}</button>${errLine('fx')}`);
  const D = S.devs || [], nOn = D.filter(d => d.s === 'on').length, pins = pinsNow();
  const mainD = pins.length ? pins.map(p => D.find(d => devKey(d) === p)) : D.slice(0, DEV_MAIN);   // the pinned ones, else the first few
  put('devs', `<div class="sec-h"><h2>${esc(t('devs'))}</h2>${D.length ? `<span class="count">${nOn} / ${D.length}</span>` : ''}</div>
    ${D.length ? mainD.map(d => devRow(d)).join('') : `<p class="muted">${esc(t('noDev'))}</p>`}
    ${D.length > mainD.length ? `<button class="link wide" type="button" data-key="alldev" data-go="devices">${esc(t('allDev', D.length))}${ICON.next}</button>` : ''}${errLine('dev')}`);
  put('foot', `<button class="btn" type="button" data-key="open" data-menu="window">${esc(t('open'))}</button>
    <button class="icon" type="button" data-key="more" data-go="more" aria-label="${esc(t('more'))}" aria-haspopup="true">${ICON.more}</button>`);
  // the views one step in
  const sub = (title, key) => `<div class="sub-h"><button class="icon" type="button" data-key="back-${key}" data-back aria-label="${esc(t('back'))}">${ICON.back}</button><h2>${esc(title)}</h2></div>`;
  if (view === 'profiles') put('v-profiles', sub(t('profs'), 'p') + errLine('prof') + `<div class="list">${P.map(p => {
    const cur = p.id === S.profile, b = busy('prof') && pend.prof.id === p.id;
    return `<button class="row pick${cur ? ' on' : ''}${b ? ' busy' : ''}" type="button" data-key="p${p.id}" data-prof="${p.id}" aria-current="${cur}"><span class="name" title="${esc(p.n)}">${esc(p.n)}</span>${b ? `<small>${esc(t('sending'))}</small>` : ''}${ICON.check}</button>`;
  }).join('')}</div>`);
  if (view === 'effects') {   // the search field is made with the view and kept; only the list under it follows the state
    if (put('v-effects', sub(t('fxs'), 'e') + `<input id="fx-q" class="find" type="search" data-key="fx-q" autocomplete="off" spellcheck="false" placeholder="${esc(t('fxFind'))}" aria-label="${esc(t('fxFind'))}" aria-controls="fx-list"><div id="fx-list"></div>`)) {
      delete parts['fx-list']; $('#fx-q').value = fxQ;
    }
    put('fx-list', fxList(on));
  }
  if (view === 'devices') put('v-devices', sub(t('devs'), 'd') + errLine('dev') + (D.length ? `<p class="muted hint">${esc(t('pinHint'))}</p>` : '') +
    `<div class="list">${D.map(d => devRow(d, pins)).join('') || `<p class="muted">${esc(t('noDev'))}</p>`}</div>`);
  if (view === 'more') {
    const [pn, po] = S.pc || [0, 0], all = pn > 0 && po === pn, pcb = busy('pc');
    put('v-more', sub(t('more'), 'm') + `<div class="list">
      <button class="row" type="button" data-key="m-window" data-menu="settings"><span class="name">${esc(t('settings'))}</span></button>
      <div class="group" role="group" aria-labelledby="pc-h"><small id="pc-h">${esc(t('pc'))}</small>
        ${pn ? `<button class="row${pcb ? ' busy' : ''}" type="button" data-key="m-pc" data-pc="${all ? 0 : 1}"><span class="txt"><span class="name">${esc(t(all ? 'pcTake' : 'pcGive'))}</span><small>${esc(pcb ? t('sending') : t('pcNow', po, pn))}</small></span></button>`
             : `<p class="muted">${esc(t('pcNone'))}</p>`}${errLine('pc')}</div>
      <button class="row" type="button" data-key="m-help" data-menu="help"><span class="name">${esc(t('help'))}</span></button>
      <button class="row" type="button" data-key="m-diag" data-menu="diag"><span class="name">${esc(t('diag'))}</span></button>
      <button class="row" type="button" data-key="m-log" data-menu="log"><span class="name">${esc(t('log'))}</span></button>
      <button class="row" type="button" data-key="m-ini" data-menu="ini"><span class="name">${esc(t('ini'))}</span></button>
      <hr><button class="row quit" type="button" data-key="m-quit" data-menu="quit"><span class="name">${esc(t('quit'))}</span></button></div>`);
  }
  height();
}
// the window is as tall as the page (ui_tray.cpp clamps it to the work area; past that the panel scrolls)
function height(force) {
  const v = document.getElementById(view === 'main' ? 'v-main' : 'v-' + view);
  const h = Math.ceil(v.scrollHeight + 2);   // + the panel's border
  if (force || h !== lastH) { lastH = h; send({ cmd: 'height', h }); }
}

function go(v) {
  const from = view;
  view = v;
  if (v === 'effects') fxQ = '';
  for (const id of ['main', 'profiles', 'effects', 'devices', 'more']) document.getElementById('v-' + id).hidden = id !== v;
  render();
  $('#panel').scrollTop = 0;
  const target = v === 'main' ? $(`[data-go="${from}"]`) || $('#power') : $(`#v-${v} [data-back]`);
  if (target) target.focus();
}

// ---- input
document.addEventListener('click', e => {
  const b = e.target.closest('button');
  if (!b || !S || b.disabled) return;
  const d = b.dataset;
  if (d.go) return go(d.go);
  if ('back' in d) return go('main');
  if (d.menu) return send({ cmd: 'menu', do: d.menu });
  if (b.id === 'power') { if (busy('power')) return;   // a toggle: a second one before the answer would undo the first
    const want = !isOn(); return sent('power', { cmd: 'power' }, s => (s.fx !== s.off) === want, 'power'); }
  if (d.prof) {
    const id = +d.prof;
    if (id === S.profile) return;
    profBase = (S.prof || {}).n || 0;
    return sent('prof', { cmd: 'profile', id }, s => s.profile === id, 'prof', id);
  }
  if (d.fx) { const id = d.fx; return sent('fx', { cmd: 'effect', id }, s => s.fx === id && !s.preset, 'fx', 'f' + id); }
  if (d.preset) { const id = +d.preset; return sent('fx', { cmd: 'preset', id }, s => s.preset === id, 'fx', 'p' + id); }
  if (d.pin) {   // pin or unpin a device on the first screen, by its stable key
    if (busy('pin')) return;
    const cur = pinsNow(), v = (cur.includes(d.pin) ? cur.filter(p => p !== d.pin) : cur.concat(d.pin)).slice(0, PIN_MAX).join(',');
    return sent('pin', { cmd: 'set', s: 'general', k: 'tray_pin', v }, s => (s.dpin || '') === v, 'dev');
  }
  if (d.dev) {
    const dv = (S.devs || []).find(x => (x.k ? 'k:' + x.k : 'id:' + x.id) === d.dev);
    if (!dv || busy('dev' + d.dev)) return;   // toggle commands: not sent twice before the answer
    const want = dv.s === 'off', same = s => (s.devs || []).find(x => (x.k ? 'k:' + x.k : 'id:' + x.id) === d.dev);
    const cmd = dv.k ? { cmd: 'toggle', k: dv.k } : { cmd: 'set', s: 'dev.' + dv.id, k: 'enabled', v: want ? '1' : '0' };
    return sent('dev' + d.dev, cmd, s => { const x = same(s); return x ? (x.s !== 'off') === want : 'devGone'; }, 'dev');
  }
  if (d.pc) { const v = d.pc; sent('pc', { cmd: 'pc_own', v }, s => v === '1' ? s.pc[1] === s.pc[0] : s.pc[1] === 0, 'pc'); }
});
document.addEventListener('input', e => { if (e.target.id === 'fx-q') { fxQ = e.target.value; render(); } });
document.addEventListener('keydown', e => {
  const a = document.activeElement, q = view === 'effects' ? $('#fx-q') : null;
  if (e.key === 'Escape') {
    e.preventDefault();
    if (q && a === q && fxQ) { q.value = fxQ = ''; render(); }   // first the search, then the view
    else if (view !== 'main') go('main'); else send({ cmd: 'close' });
    return;
  }
  if (view !== 'effects' && view !== 'profiles') return;
  // the pickers: arrows move through the search and the rows, Enter in the search takes the first match,
  // a letter typed on a row goes to the search
  const items = [...document.querySelectorAll(`#v-${view} #fx-q, #v-${view} button.pick`)];
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    const i = items.indexOf(a), n = items[i < 0 ? 0 : Math.max(0, Math.min(items.length - 1, i + (e.key === 'ArrowDown' ? 1 : -1)))];
    if (n) { e.preventDefault(); n.focus(); }
  } else if (q && a === q && e.key === 'Enter') {
    const f = $('#fx-list button.pick');
    if (f) { e.preventDefault(); f.click(); }
  } else if (q && a !== q && e.key.length === 1 && e.key !== ' ' && !e.ctrlKey && !e.altKey && !e.metaKey) {
    e.preventDefault(); q.focus(); q.value = fxQ += e.key; render();
  }
});
// brightness: at most one command per BRI_MS while dragging, and the last value always (the core saves it, app_save_soon)
function briSend(final) {
  const v = +$('#bri').value;
  clearTimeout(briTimer); briTimer = 0;
  if (v !== briLast || final) { briLast = v; send({ cmd: 'brightness', v }); }
}
function briInput() {
  const br = $('#bri');
  briDrag = 1; briHoldTill = Date.now() + BRI_HOLD;
  $('#bri-v').textContent = br.value + '%';
  br.setAttribute('aria-valuetext', br.value + '%');
  if (!briTimer) briTimer = setTimeout(() => briSend(false), BRI_MS);
}
function briDone() { if (!briDrag) return; briDrag = 0; briHoldTill = Date.now() + BRI_HOLD; briSend(true); }

// ---- the window's messages (ui_tray.cpp)
function onMessage(m) {
  if (!m || !m.type) return;
  if (m.type === 'tray') onState(m);
  else if (m.type === 'open') { profBase = null; for (const k in errs) delete errs[k]; if (S) profBase = (S.prof || {}).n || 0; render(); height(true); }
  else if (m.type === 'shown') {
    side = m.side || 'b';
    const p = $('#panel');
    p.dataset.side = side;
    p.classList.remove('in'); void p.offsetWidth; p.classList.add('in');   // the entrance plays once, then nothing runs
    const f = view === 'main' ? $('#power') : null;
    if (f && !p.contains(document.activeElement)) f.focus({ preventScroll: true });
  }
  else if (m.type === 'closed') {   // next time it opens on its first view, without old "sending" or errors
    briDone();
    for (const k in pend) delete pend[k];
    for (const k in errs) delete errs[k];
    fxQ = '';
    $('#panel').classList.remove('in');
    if (view !== 'main') go('main');
    if (document.activeElement) document.activeElement.blur();
  }
}

function init() {
  const br = $('#bri');
  br.addEventListener('input', briInput);
  br.addEventListener('change', briDone);
  br.addEventListener('pointerup', briDone);
  br.addEventListener('keyup', briDone);
  br.addEventListener('blur', briDone);
  matchMedia('(prefers-reduced-motion: reduce)').addEventListener('change', applyMotion);
  if (bridge) bridge.addEventListener('message', e => onMessage(e.data));
  send({ cmd: 'hello' });
}
init();
