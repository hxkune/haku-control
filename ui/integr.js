// Integrations: OpenRGB on this PC, Home Assistant (MQTT) and the phone page. A plain script loaded before app.js: at the
// top level only declarations (they use app.js's helpers and state when called, never while loading); integrBind() at the end.

// ---- other PC hardware: what OpenRGB on this PC offers (S.orgb: state 0 not looked, 1 looking, 2 answers, 3 none).
// Hardware haku drives itself (the MSI board, ENE memory) is shown but not offered, so two programs never fight.
const ORGB_TYPES = { 0: 'board', 1: 'ram', 2: 'gpu', 3: 'fan', 4: 'strip', 5: 'keyboard', 6: 'mouse', 18: 'keyboard' };
// hardware haku drives itself: its memory and MSI board, and devices added directly (their brand in the name)
const orgbOwn = c => (c.type === 1 && S.sticks > 0) || (c.type === 0 && S.msi && /msi|mystic/i.test(c.name)) ||
  S.ext.devs.some(d => d.kind !== 'openrgb' && (d.title || '').split(' ')[0].length >= 3 && c.name.toLowerCase().includes(d.title.split(' ')[0].toLowerCase())) ||
  (nanoOn() && /nanoleaf/i.test(c.name));
// by name, and for controllers sharing one name (two sticks of a kit) by their rank among them, as numbers change
const orgbDev = c => {
  const local = S.ext.devs.filter(d => d.kind === 'openrgb' && /^(127\.0\.0\.1|localhost)(:\d+)?$/.test(d.host));
  const old = local.find(d => !cv('dev.' + d.id, 'match', '') && d.sub === c.i);
  if (old) return old;
  const same = local.filter(d => cv('dev.' + d.id, 'match', '') === c.name), mine = same.find(d => d.sub === c.i);
  return mine || same.filter(d => !(S.orgb.ctls || []).some(x => x.name === c.name && x.i === d.sub))
    [(S.orgb.ctls || []).filter(x => x.name === c.name && x.i < c.i && !same.some(d => d.sub === x.i)).length];
};
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

let qrUrl = '';   // the address the QR code points at (the PC may be on several networks)

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
    const v = cv('mqtt', k, '');
    // a saved password never reaches the page, only "(set)": the field stays empty with dots as a hint
    if (k === 'password') el.placeholder = v === '(set)' ? '••••••••' : '';
    if (document.activeElement !== el) el.value = k === 'password' && v === '(set)' ? '' : v;
  });
  const st = $('#ha-state');
  st.textContent = locked ? t('ha.pro') : !on.checked ? '' : M.state === 'connected' ? t('ha.connected', M.topic) :
    M.state === 'connecting' ? t('ha.connecting') : M.state === 'error' ? t('ha.error', M.err) : '';
  st.className = 'ha-state ' + (M.state === 'connected' && on.checked && !locked ? 'ok' : M.state === 'error' && on.checked ? 'bad' : '');
}

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
  $('#remote-fw').classList.toggle('hidden', !(R.on && R.fw_blocks));   // haku never removes a block rule itself: the button opens the firewall
}

// their handlers: app.js calls this once its helpers are there
function integrBind() {
  $('#orgb-check').addEventListener('click', () => { S.orgb = Object.assign({}, S.orgb, { state: 1 }); updateOrgb(); send({ cmd: 'orgb_check' }); });
  $('#orgb-setup').addEventListener('click', () => { S.orgb = Object.assign({}, S.orgb, { setup: 1, pct: 0 }); updateOrgb(); send({ cmd: 'orgb_setup' }); });
  $('#orgb-locate').addEventListener('click', () => send({ cmd: 'orgb_locate' }));
  $('#orgb-auto').addEventListener('change', e => send({ cmd: 'orgb_auto', v: e.target.checked ? '1' : '0' }));
  $('#orgb-all').addEventListener('click', () => { (S.orgb.ctls || []).filter(c => !orgbOwn(c) && !orgbAdded(c)).forEach(orgbAdd); $('#orgb-all').disabled = true; setTimeout(() => { $('#orgb-all').disabled = false; }, 3000); });
  $('#ha-on').addEventListener('change', e => { setCfg('mqtt', 'on', e.target.checked ? '1' : '0'); updateHa(); });
  [['#ha-host', 'host'], ['#ha-port', 'port'], ['#ha-user', 'user'], ['#ha-pass', 'password']].forEach(([id, k]) =>
    $(id).addEventListener('change', e => setCfg('mqtt', k, e.target.value.trim())));
  $('#remote-on').addEventListener('change', e => { setCfg('remote', 'enabled', e.target.checked ? 1 : 0); updateRemote(); });
  $('#remote-newpin').addEventListener('click', () => send({ cmd: 'remote_pin' }));
  $('#remote-forget').addEventListener('click', () => send({ cmd: 'remote_forget' }));
}
