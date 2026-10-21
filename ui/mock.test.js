// SPDX-License-Identifier: GPL-3.0-only
// Regression checks on the design preview (not shipped: build.cmd drops mock.*). Open index.html?mock&test; the
// results show in a box at the top and in the console, and as window.TEST = { done, failed, log }.
(async function () {
  const sent = [], bridge = window.chrome.webview, post = bridge.postMessage;
  bridge.postMessage = s => { sent.push(JSON.parse(s)); post.call(bridge, s); };
  const wait = ms => new Promise(r => setTimeout(r, ms));
  const log = [], R = window.TEST = { done: false, failed: 0, log };
  const ok = (c, msg) => { if (!c) throw new Error(msg); };
  const sets = (s, k) => sent.filter(o => o.cmd === 'set' && o.s === s && o.k === k);
  const key = (el, k, ctrl) => { const e = new KeyboardEvent('keydown', { key: k, ctrlKey: !!ctrl, bubbles: true, cancelable: true }); el.dispatchEvent(e); return e; };
  await wait(400);   // the first state
  const tests = [];
  const test = (name, fn) => tests.push([name, fn]);
  const fresh = async () => { UNDO.length = 0; sent.length = 0; await wait(80); };

  test('undo after a profile switch leaves the new profile alone', async () => {
    await fresh();
    const br = $('#bright'); br.value = 40; br.dispatchEvent(new Event('input', { bubbles: true }));
    profGo(2); await wait(200);
    sent.length = 0;
    key(document.body, 'z', true); await wait(80);
    ok(!sent.some(o => o.cmd === 'brightness' || o.cmd === 'set'), 'Ctrl+Z after the switch sent ' + JSON.stringify(sent));
    ok($('#undo').classList.contains('hidden'), 'the undo button still shows');
  });

  test('a switch made by the core (clock, program) also starts undo over', async () => {
    await fresh();
    setCfg('flow', 'speed', '8');
    STATE.cfg.general.profile = '3'; bridge.postMessage(JSON.stringify({ cmd: 'get' })); await wait(120);
    sent.length = 0; undo(); await wait(80);
    ok(!sets('flow', 'speed').length, 'undo wrote into the new profile');
  });

  test('undo drops a drag still waiting to be sent', async () => {
    await fresh();
    const before = cv('flow', 'speed', '');
    setCfgSoon('flow', 'speed', '9'); undo(); await wait(120);
    const last = sets('flow', 'speed').pop();
    ok(last && last.v === before, 'last value sent: ' + JSON.stringify(last) + ', want ' + before);
    ok(cv('flow', 'speed', '') === before, 'the page shows ' + cv('flow', 'speed', ''));
  });

  test('a group change goes back on every device, each to its own value', async () => {
    await fresh();
    const n = groupNew(); groupAdd(n, 'zone.gpu'); groupAdd(n, 'zone.dev1');
    setCfg('zone.gpu', 'palette', '#111111'); setCfg('zone.dev1', 'palette', '#222222');
    await wait(20);
    setCfg(groupSec(n), 'palette', '#ABCDEF');
    ok(cv('zone.gpu', 'palette') === '#ABCDEF' && cv('zone.dev1', 'palette') === '#ABCDEF', 'the group did not spread');
    undo(); await wait(80);
    ok(cv('zone.gpu', 'palette') === '#111111', 'gpu: ' + cv('zone.gpu', 'palette'));
    ok(cv('zone.dev1', 'palette') === '#222222', 'dev1: ' + cv('zone.dev1', 'palette'));
    ok(STATE.cfg['zone.dev1'].palette === '#222222', 'the core has ' + STATE.cfg['zone.dev1'].palette);
    setMembers(n, []);
  });

  test('Cancel in the group window: nothing comes back later, nothing left to undo', async () => {
    await fresh();
    const n = groupNew(); groupDialog(n, true); groupAdd(n, 'zone.gpu');
    const name = $('#gdlg-name'); name.value = 'Temp'; name.dispatchEvent(new Event('input', { bubbles: true }));
    $('#gdlg-cancel').click(); await wait(120);
    ok(cv(groupSec(n), 'name', '') === '' && (STATE.cfg[groupSec(n)] || {}).name !== 'Temp', 'the name came back after Cancel');
    ok(!groupOf('zone.gpu'), 'the device stayed in the group');
    ok(!UNDO.length, 'undo has ' + UNDO.length + ' steps');
  });

  test('Done in the group window is one undo step', async () => {
    await fresh();
    const n = groupNew(); groupDialog(n, true); groupAdd(n, 'zone.gpu'); await wait(20); groupAdd(n, 'zone.dev1');
    $('#gdlg-form').requestSubmit(); await wait(60);
    ok(UNDO.length === 1, 'undo has ' + UNDO.length + ' steps');
    undo(); await wait(80);
    ok(!groupOf('zone.gpu') && !groupOf('zone.dev1'), 'devices still grouped');
  });

  test('Ctrl+Z in a text field stays the field\'s own', async () => {
    await fresh();
    setCfg('flow', 'speed', '6');
    const inp = document.createElement('input'); document.body.appendChild(inp); inp.focus();
    const e = key(inp, 'z', true); inp.remove();
    ok(!e.defaultPrevented && UNDO.length === 1, 'the page took Ctrl+Z from a text field');
  });

  test('a window keeps Tab inside and gives focus back when it closes', async () => {
    await fresh();
    const opener = document.createElement('button'); opener.textContent = 'opener'; document.body.appendChild(opener); opener.focus();
    const w = $('#report-dlg'); reportDialog(); await wait(80);
    ok(w.contains(document.activeElement), 'focus did not move into the window');
    const f = [...w.querySelectorAll('button, input, textarea, select, [href]')].filter(x => !x.disabled && x.offsetParent);
    f[f.length - 1].focus(); ok(key(f[f.length - 1], 'Tab').defaultPrevented && document.activeElement === f[0], 'Tab left the window');
    const e = new KeyboardEvent('keydown', { key: 'Tab', shiftKey: true, bubbles: true, cancelable: true }); f[0].dispatchEvent(e);
    ok(e.defaultPrevented && document.activeElement === f[f.length - 1], 'Shift+Tab left the window');
    key(document.body, 'Escape'); await wait(60);
    ok(w.classList.contains('hidden'), 'Escape did not close it');
    ok(document.activeElement === opener, 'focus is on ' + (document.activeElement.id || document.activeElement.tagName));
    opener.remove();
  });

  test('the days of a profile: each click flips that day only, the last one stays', async () => {
    await fresh();
    setCfg('profile.3', 'at', '22:00'); setCfg('profile.3', 'days', ''); profMenu(true);
    $('#prof-list .prof-row[data-id="3"] .prof-ren').click();
    const day = d => $(`#prof-panel .prof-day[data-d="${d}"]`).click(), days = () => cv('profile.3', 'days', '');
    day(7); ok(days() === '123456', 'Sunday off: ' + days());
    day(6); ok(days() === '12345', 'Saturday off: ' + days());
    day(7); day(6); ok(days() === '', 'all back is every day: ' + days());
    for (const d of '123456') day(d);
    day(7); ok(days() === '7', 'the last day went: ' + days());
    closeSheet();
  });

  test('two profiles at the same minute and day show the clash; the clock switched off does not', async () => {
    await fresh();
    setCfg('profile.2', 'at', '07:30'); setCfg('profile.2', 'days', '12345'); setCfg('profile.3', 'at', '07:30'); setCfg('profile.3', 'days', '5');
    setCfg('profile.3', 'at_off', '');
    profMenu(true); $('#prof-list .prof-row[data-id="3"] .prof-ren').click();
    ok($('#prof-panel .prof-edit .hint.warn'), 'no clash shown');
    $('#prof-panel .prof-day[data-d="6"]').click(); $('#prof-panel .prof-day[data-d="5"]').click();   // Saturday only
    ok(!$('#prof-panel .prof-edit .hint.warn'), 'a clash on days not in common');
    setCfg('profile.3', 'days', '5'); setCfg('profile.2', 'at_off', '1'); renderProfiles();
    ok(!$('#prof-panel .prof-edit .hint.warn'), 'a clash with a clock switched off');
    setCfg('profile.2', 'at', ''); setCfg('profile.3', 'at', ''); closeSheet();
  });

  test('pause shows on the switcher and is cleared by resume; Duplicate asks the core for a copy', async () => {
    await fresh();
    setCfg('profile.1', 'apps', 'cs2.exe'); profMenu(true);
    $('#prof-state .prof-pause').click();
    ok(cv('general', 'auto_pause', '') === '1' && !$('#prof-paused').classList.contains('hidden'), 'not paused');
    $('#prof-state .prof-pause').click();
    ok(cv('general', 'auto_pause', '') === '' && $('#prof-paused').classList.contains('hidden'), 'not resumed');
    $('#prof-list .prof-row[data-id="2"] .prof-ren').click(); sent.length = 0; $('#prof-panel .prof-dup').click();
    ok(sent.some(o => o.cmd === 'profile_dup' && o.id === '2' && o.name), 'sent ' + JSON.stringify(sent));
    setCfg('profile.1', 'apps', ''); closeSheet();
  });

  test('a phone keeps the kinds of devices in a row on Devices; the AI offer waits for a try or a download', async () => {
    await fresh();
    showTab('nano');
    ok($('#dev-jump').classList.contains('on') && $('#dev-jump [aria-current="true"]')?.dataset.tab === 'nano', 'row not on nano');
    ok($('#nav [data-tab="devices"]').classList.contains('sub'), 'Devices not marked');
    showTab('effects');
    ok(!$('#dev-jump').classList.contains('on') && !$('#nav [data-tab="devices"]').classList.contains('sub'), 'row left on Effects');
    const was = S.ollama;
    S.ollama = { exe: 0, model: 0, stage: 0 }; MOOD.err = ''; updateAi();
    ok($('#mood-setup').classList.contains('hidden'), 'offer shown before a try');
    MOOD.err = 'noollama'; updateAi();
    ok(!$('#mood-setup').classList.contains('hidden'), 'no offer after a try');
    MOOD.err = ''; S.ollama = { exe: 0, model: 0, stage: 5, pct: 40 }; updateAi();
    ok(!$('#mood-setup').classList.contains('hidden'), 'a download hidden');
    S.ollama = was; updateAi();
  });

  test('every switch and tick box can be reached with Tab, has a name and says if it is on', async () => {
    await fresh();
    showTab('devices');
    const all = [...document.querySelectorAll('.switch input, .check input')];
    ok(all.length > 10, 'only ' + all.length + ' found');
    for (const i of all) {
      const where = i.id || i.dataset.toggle || i.className || i.closest('label').textContent.trim();
      ok(getComputedStyle(i).display !== 'none' && i.tabIndex === 0, where + ' not reachable');
      const name = i.getAttribute('aria-labelledby') ? document.getElementById(i.getAttribute('aria-labelledby'))?.textContent.trim() : i.closest('label').textContent.trim();
      ok(name, where + ' has no name');
      if (i.closest('.switch')) ok(i.getAttribute('role') === 'switch', where + ' not a switch');
    }
    const names = [...document.querySelectorAll('.dev-on')].map(i => document.getElementById(i.getAttribute('aria-labelledby')).textContent);
    ok(S.ext.devs.every(d => names.includes(d.name)), 'device switches not named after them: ' + names);
  });

  test('the pencil opens the profile in its own panel: the menu closes, Enter keeps a new name, Esc keeps the old one', async () => {
    await fresh();
    showTab('effects');   // the profile button lives here: focus comes back to it
    const name = cv('profile.2', 'name', '');
    profMenu(true); $('#prof-list .prof-row[data-id="2"] .prof-ren').click(); await wait(40);
    ok($('#prof-menu').classList.contains('hidden'), 'the menu stayed open');
    ok($('#sheet-body #prof-panel .prof-edit[data-id="2"]') && !$('#sheet').classList.contains('hidden'), 'not in the sheet');
    ok(!$('#prof-list .prof-edit'), 'the editor is still in the menu');
    ok(document.activeElement === $('#prof-panel .prof-name'), 'the name is not focused');
    ok($('#sheet .sheet').getAttribute('aria-labelledby') === 'prof-panel-h' && $('#sheet-page').classList.contains('hidden'), 'the sheet is not named or shows Open page');
    $('#prof-panel .prof-name').value = name + ' x'; key($('#prof-panel .prof-name'), 'Escape'); await wait(60);
    ok(cv('profile.2', 'name', '') === name && $('#sheet').classList.contains('hidden'), 'Esc did not keep the old name and close');
    ok(document.activeElement === $('#prof-btn'), 'focus is on ' + document.activeElement.id);
    profMenu(true); $('#prof-list .prof-row[data-id="2"] .prof-ren').click(); await wait(40);
    $('#prof-panel .prof-name').value = name + ' x'; key($('#prof-panel .prof-name'), 'Enter'); await wait(60);
    ok(cv('profile.2', 'name', '') === name + ' x' && $('#sheet').classList.contains('hidden'), 'Enter did not keep the name and close');
    ok($('#prof-panel').closest('#prof') && !$('#prof-panel .prof-edit'), 'the panel is not back in place and empty');
    setCfg('profile.2', 'name', name); renderProfiles();
  });

  test('the PC tab: the light cards come first, the found hardware folds once haku lights some, what needs a step stays out', async () => {
    await fresh();
    const g = $('#tab-pc .pc-grid'), kids = [...g.children];
    ok(kids.indexOf($('#ram-card')) < kids.indexOf($('.hw-card')) && kids.indexOf($('#gpu-card')) < kids.indexOf($('.hw-card')), 'the hardware list is above the light cards');
    const was = S.hw;
    S.hw = { busy: 0, done: 1, items: [{ cat: 'ram', maker: 'G.Skill', name: 'F4', id: '' }, { cat: 'gpu', maker: 'NVIDIA', name: 'RTX 4070', id: '' }, { cat: 'usb', maker: 'Corsair', name: 'Hub', id: '1B1C0C3F' }] };
    const o = S.orgb; S.orgb = { state: 4, ctls: [] };
    updateHw();
    const lit = S.hw.items.map(hwRoute).filter(r => r.cls === 'on').length;
    ok(lit, 'the memory sticks are not lit by haku in the preview');
    ok(!$('#hw-more').classList.contains('hidden') && $$('#hw-rest .found-row').length === 2 && !$('#hw-more').open, 'the rest is not folded');
    ok($$('#hw-list .found-row').length === 1 && $('#hw-list [data-hw]'), 'the row that needs a step is not in sight with its button');
    S.hw = Object.assign({}, S.hw, { done: 0 }); updateHw();
    ok($('#hw-more').classList.contains('hidden') && $$('#hw-list .found-row').length === 3, 'not everything shows before the first look is done');
    S.hw = was; S.orgb = o; updateHw();
  });

  test('OpenRGB is fetched unasked, once, when something here lights only through it', async () => {
    await fresh();
    const was = S.hw, o = S.orgb, n = () => sent.filter(x => x.cmd === 'orgb_setup').length, n0 = n();
    S.hw = { busy: 0, done: 1, items: [{ cat: 'usb', maker: 'Corsair', name: 'Hub', id: '1B1C0C3F' }] };
    S.orgb = { state: 4, ctls: [], auto: 0, exe: 0, setup: 0 }; updateHw();
    ok(n() === n0, 'not with Auto start off');
    S.orgb = { state: 4, ctls: [], auto: 1, exe: 0, setup: 0 }; updateHw();
    S.orgb = Object.assign({}, S.orgb, { setup: 0 }); updateHw();
    ok(n() === n0 + 1, 'once: ' + (n() - n0));
    S.hw = was; S.orgb = o; orgbFetched = false; updateHw();
  });

  test('OpenRGB: two sticks of one kit share a name, the second is not taken as added with the first', async () => {
    await fresh();
    const o = S.orgb, devs = S.ext.devs, N = 'Corsair Vengeance RGB DDR5';
    const a = { i: 0, name: N, type: 1 }, b = { i: 1, name: N, type: 1 };
    S.orgb = { state: 4, ctls: [a, b] };
    S.ext.devs = [{ id: 91, kind: 'openrgb', host: '127.0.0.1', sub: 0 }]; setCfgLocal('dev.91', 'match', N);
    ok(orgbDev(a) && orgbDev(a).id === 91 && !orgbAdded(b), 'the second stick counts as added with the first');
    S.ext.devs.push({ id: 92, kind: 'openrgb', host: '127.0.0.1', sub: 5 }); setCfgLocal('dev.92', 'match', N);
    ok(orgbDev(b) && orgbDev(b).id === 92, 'a stick whose number moved is not found by its rank');
    delete S.cfg['dev.91']; delete S.cfg['dev.92']; S.ext.devs = devs; S.orgb = o;
  });

  test('iCUE LINK: hidden until found, off until asked, then the PSU with its watts and a warning while iCUE runs', async () => {
    await fresh();
    const K = S.link;
    S.link = { st: 'off', found: 0 }; updateLink();
    ok($('#link-card').classList.contains('hidden'), 'shown with no hub');
    S.link = K; updateLink();
    ok(!$('#link-card').classList.contains('hidden') && !$('#link-on').checked && !$('#link-list').children.length, 'read before it was switched on');
    $('#link-on').click(); await wait(60);
    ok(cv('link', 'on', '') === '1', 'the switch did not reach the settings');
    const rows = [...$('#link-list').children].map(r => r.textContent.replace(/\s+/g, ' '));
    ok(rows.length === 5 && rows[0].includes('25.6'), 'hub + 4 devices: ' + rows.length);
    ok(rows.some(r => r.includes('HX1000i') && r.includes(t('link.w', 157)) && r.includes('12.03 V 10.25 A')), 'the PSU row: ' + rows.join(' | '));
    ok(rows.some(r => r.includes('TITAN 360 AIO') && r.includes('35.2')), 'the AIO row');
    S.link = Object.assign({}, K, { st: 'app', app: 'iCUE.exe' }); updateLink();
    ok($('#link-status').textContent === t('link.st.app', 'iCUE.exe') && !$('#link-list').children.length, 'no warning or stale rows while iCUE runs');
    setCfg('link', 'on', '0'); S.link = K; updateLink();
  });

  test('programs in the way: Close while it runs, Each time into [conflicts] stop, Remove opens Installed apps', async () => {
    await fresh();
    const rows = () => [...$('#cf-list').children], btn = (r, a) => r.querySelector(`[data-act="${a}"]`);
    ok(!$('#cf-card').classList.contains('hidden') && rows().length === 2 && $('#cf-status').textContent === t('cf.st.run', 1), 'card, rows or status');
    ok(btn(rows()[0], 'close') && !btn(rows()[1], 'close') && btn(rows()[1], 'del'), 'Close only on the running one, Remove on the installed one');
    btn(rows()[1], 'auto').click(); await wait(20);
    ok(cv('conflicts', 'stop', '') === 'icue' && btn(rows()[1], 'auto').checked, 'Each time did not reach the settings');
    btn(rows()[0], 'auto').click(); btn(rows()[1], 'auto').click(); await wait(20);
    ok(cv('conflicts', 'stop', '') === 'armoury', 'the list kept the wrong ones: ' + cv('conflicts', 'stop', ''));
    btn(rows()[0], 'close').click(); await wait(60);
    ok(rows()[0].textContent.includes(t('cf.busy')) && !btn(rows()[0], 'close'), 'no Closing state');
    await wait(700);
    ok(rows()[0].textContent.includes(t('cf.inst')) && $('#cf-status').textContent === t('cf.st.idle'), 'not closed after the core said so');
    setCfg('conflicts', 'stop', '');
    const a = S.cf; S.cf = { busy: '', a: [] }; updateConflicts();
    ok($('#cf-card').classList.contains('hidden'), 'shown with nothing in the way');
    S.cf = a; updateConflicts();
  });

  test('the offline list: the chip is a button, each name opens its device card, Esc goes back to the chip', async () => {
    await fresh();
    showTab('effects'); updateChips();
    const c = $('#chips .chip.has-off');
    ok(c && c.tagName === 'BUTTON', 'no offline chip, or not a button');
    c.focus(); c.click(); await wait(20);
    const row = $('#chip-pop .off-row');
    ok(!$('#chip-pop').classList.contains('hidden') && document.activeElement === row, 'the list is not open with focus in it');
    key(row, 'Escape'); await wait(20);
    ok($('#chip-pop').classList.contains('hidden') && document.activeElement === $('#chips .chip.has-off'), 'Esc did not go back to the chip');
    $('#chips .chip.has-off').click(); await wait(20);
    const h = JSON.parse($('#chip-pop .off-row').dataset.h);
    $('#chip-pop .off-row').click(); await wait(60);
    ok(!$('#sheet').classList.contains('hidden') && $('#sheet-body .card') && SHEET.kind === h.k, 'the name did not open its card');
    closeSheet(); await wait(20);
  });

  test('the theme code is folded behind a small Code button', async () => {
    await fresh();
    showTab('settings');
    ok($('#code').classList.contains('hidden') && !$('#code-open').classList.contains('hidden'), 'the code field shows by itself');
    $('#code-open').click();
    ok(!$('#code').classList.contains('hidden') && document.activeElement === $('#code-in') && $('#code-open').classList.contains('hidden'), 'Code did not open the field');
    key($('#code-in'), 'Escape'); await wait(20);
    ok($('#code').classList.contains('hidden') && document.activeElement === $('#code-open'), 'Esc did not fold it back');
    showTab('effects');
  });

  test('a phone opens the effect settings in the sheet: the same card, kept up to date, back in place on close', async () => {
    await fresh();
    showTab('effects'); $('#fx-tune').style.display = 'flex'; // shown on phones only: shown here so focus has a place to come back to
    $('#fx-tune').focus(); $('#fx-tune').click(); await wait(80);
    ok($('#sheet-body .fx-side') && !$('#sheet').classList.contains('hidden'), 'not in the sheet');
    ok($$('#fx-name').length === 1, 'the settings card is there twice');
    ok($('#sheet-page').classList.contains('hidden'), 'Open page shown for the page already open');
    const was = S.effect; S.effect = was === 'wave' ? 'flow' : 'wave'; renderEffectSide();
    ok($('#sheet-body #fx-name').textContent === fxName(S.effect) && $('#fx-tune-name').textContent === fxName(S.effect), 'not updated');
    key(document.body, 'Escape'); await wait(60);
    ok($('#sheet').classList.contains('hidden') && $('.fx-layout > .fx-side'), 'not back in place');
    ok(document.activeElement === $('#fx-tune'), 'focus is on ' + document.activeElement.id);
    S.effect = was; renderEffectSide(); $('#fx-tune').style.display = '';
  });

  test('a setting changed while a device is unreachable says it is saved, only while that is true', async () => {
    await fresh();
    const d = S.ext.devs.find(x => x.id === 3), w = S.ext.devs.find(x => x.id === 1), ck = $('#dev-chk-3');
    const set = (id, v) => { const s = $(`#dev-chk-${id}`).closest('.card').querySelector('.dev-type'); s.value = v; s.dispatchEvent(new Event('change', { bubbles: true })); };
    set(1, 'tv'); ok($('#dev-chk-1').textContent === '', 'an online device says saved for later');
    set(3, 'lamp'); ok(ck.textContent === t('dev.saved'), 'no note: ' + ck.textContent);
    d.enabled = 0; updateDevices(); ok(ck.textContent === '', 'a switched-off device would not apply it');
    d.enabled = 1; d.online = 1; updateDevices(); ok(ck.textContent === '', 'still shown once online');
    d.online = 0; updateDevices(); ok(ck.textContent === '', 'came back after it applied');
    setCfg('dev.1', 'type', 'auto'); setCfg('dev.3', 'type', 'auto'); w.online = 1;
  });

  // fans: the browser has no core, so nothing here reaches a QUADRO; the mock stands in for what fans.c reports
  const fanEmit = async () => { bridge.postMessage(JSON.stringify({ cmd: 'get' })); await wait(120); };
  const fanReset = async () => {
    const F = STATE.fans; Object.assign(F, { link: 1, age: 400, rfail: 0, log: [] });
    F.f.forEach(f => Object.assign(f, { em: '', own: 0, want: -1, out: -1, ok: 1, err: '' }));
    Object.keys(STATE.cfg.fans || {}).forEach(k => { STATE.cfg.fans[k] = ''; });
    if (SHEET.kind === 'fan') closeSheet();
    FAN.draft = null; FAN.open = null; FAN.fold = {};
    await fanEmit(); showTab('fans'); FAN.back = 0; FAN.sig = ''; updateFans();
  };
  const fanSheetOf = async i => { $(`#fan-panel [data-f="${i}"]`).click(); await wait(120); };

  test('fans: a card looks like its fan: picked in its settings, shared by a group, a pump round; iCUE LINK and Lian Li on their own', async () => {
    await fresh(); await fanReset();
    const ic = i => $(`#fan-panel [data-f="${i}"] .fanic`), lit = el => el.querySelectorAll('.fi-lit > *').length;
    ok(ic(0) && !lit(ic(0)), 'a plain fan by default');
    await fanSheetOf(0);
    ok($$('#fs-look button').length === 6 && $('#fs-look [data-v=""]').classList.contains('on'), 'the picker');
    $('#fs-look [data-v="inf"]').click(); await wait(60);
    ok(STATE.cfg.fans.fan1_look === 'inf' && lit(ic(0)) === 3 && $('#fs-look [data-v="inf"]').getAttribute('aria-pressed') === 'true', 'picked: ' + STATE.cfg.fans.fan1_look);
    $('#fs-kind [data-v="pump"]').click(); await wait(60);
    ok($('#fs-look-f').classList.contains('hidden') && ic(0).querySelector('circle.fi-fr'), 'a pump: round, no picker');
    setCfg('fans', 'fan1_kind', ''); setCfg('fans', 'g1_name', 'Front'); setCfg('fans', 'g1', '1,2'); setCfg('fans', 'fan2_look', 'inf'); FAN.sig = ''; updateFans();
    ok(lit($('#fan-panel [data-g="1"] .fanic')) === 3, 'a group of the same look shows it');
    setCfg('fans', 'fan2_look', 'sl'); FAN.sig = ''; updateFans();
    ok(!lit($('#fan-panel [data-g="1"] .fanic')), 'a mixed group shows a plain one');
    updateLL(); const ll = $$('#ll-list .found-row');
    ok(ll.length === 2 && ll.every(r => r.querySelectorAll('.fi-lit rect').length === 2), 'Lian Li groups: lit edges');
    await fanReset();
  });

  test('fans: a lost link keeps the page and says so; the readings show as old', async () => {
    await fresh(); await fanReset();
    STATE.fans.link = 0; STATE.fans.age = 12000; await fanEmit();
    ok(NAV_NEED.fans(), 'the tab went away');
    ok($('#fans-status').textContent === t('fans.nolink', 12), 'status: ' + $('#fans-status').textContent);
    ok($('#fan-panel').classList.contains('stale') && $$('#fan-panel [data-live-f]').length === 4, 'the tiles are gone or not marked old');
    ok($('#fan-panel [data-live-f]').textContent.includes(t('fans.st.stale')), 'a tile does not say the reading is old');
    await fanReset();
  });

  test('fans: a picked mode is not shown as done until the QUADRO confirms it; the emergency reason shows', async () => {
    await fresh(); await fanReset(); await fanSheetOf(0);
    $('#fs-mode [data-v="fix"]').click();
    const row = () => $('#fan-panel [data-live-f="0"]').textContent;
    ok(row().includes(t('fans.st.pend')) && !row().includes(t('fans.st.ok')), 'before the core answers: ' + row());
    await fanEmit(); ok(row().includes(t('fans.st.ok')), 'after the read-back: ' + row());
    STATE.fans.f[0].ok = 0; await fanEmit(); ok(row().includes(t('fans.st.unconf')), 'not kept: ' + row());
    STATE.fans.f[0].ok = -1; await fanEmit(); ok(row().includes(t('fans.st.fail')), 'write failed: ' + row());
    STATE.fans.f[0].em = 'nosensor'; await fanEmit(); ok(row().includes(t('fans.em.nosensor')), 'emergency: ' + row());
    await fanReset();
  });

  test('fans: a curve that does not read is flagged; typed point values stay between the neighbours and 20..100', async () => {
    await fresh(); await fanReset();
    setCfg('fans', 'fan1', 'curve'); setCfg('fans', 'fan1_curve', '50:40,30:60'); await fanEmit(); await fanSheetOf(0);
    ok(!$('#fs-bad').classList.contains('hidden'), 'no warning for a curve going backwards');
    ok(!fanParse('50:40,30:60') && !fanParse('30:40') && !fanParse('30:40,50:101') && !fanParse('30:40,a:50') && fanParse('30:30,50:45'), 'fanParse');
    setCfg('fans', 'fan1_curve', '30:30,50:45,70:75,85:100'); FAN.draft = null; updateFans();
    ok($('#fs-bad').classList.contains('hidden'), 'the warning stays on a good curve');
    FAN.sel = 1; updateFans();
    const x = $('#fs-pt-x'), y = $('#fs-pt-y');
    x.value = '999'; x.dispatchEvent(new Event('change')); y.value = '5'; y.dispatchEvent(new Event('change')); await wait(400);
    ok(cv('fans', 'fan1_curve', '') === '30:30,50:45,70:75,85:100', 'an edit reached the core before Apply: ' + cv('fans', 'fan1_curve', ''));
    ok(fanSer(FAN.draft.pts) === '30:30,69:20,70:75,85:100' && !$('#fs-draft').classList.contains('hidden'), 'the draft: ' + fanSer(FAN.draft.pts));
    $('#fs-apply').click(); await wait(60);
    ok(cv('fans', 'fan1_curve', '') === '30:30,69:20,70:75,85:100' && $('#fs-draft').classList.contains('hidden'), 'saved: ' + cv('fans', 'fan1_curve', ''));
    x.value = ''; x.dispatchEvent(new Event('change'));
    ok(x.value === '69', 'an empty field did not come back: ' + x.value);
    // the keyboard: right goes to the next point, up adds five
    const c = $('#fs-cv'); c.focus(); key(c, 'ArrowRight'); key(c, 'ArrowUp'); await wait(400);
    $('#fs-apply').click(); await wait(60);
    ok(cv('fans', 'fan1_curve', '') === '30:30,69:20,70:80,85:100', 'keys: ' + cv('fans', 'fan1_curve', ''));
    // another kind of source starts from its own default; the GPU curve waits for the GPU
    $('#fs-src [data-v="t1"]').click();
    ok(cv('fans', 'fan1_src', 'gpu') === 'gpu', 'a source change went out before Apply');
    $('#fs-apply').click(); await wait(60);
    ok(cv('fans', 'fan1_src', '') === 't1' && cv('fans', 'fan1_curve_gpu', '') === '30:30,69:20,70:80,85:100' && cv('fans', 'fan1_curve', '') === fanScale('t1').def,
      'the GPU curve went to the water sensor: ' + cv('fans', 'fan1_curve', '') + ' / ' + cv('fans', 'fan1_curve_gpu', ''));
    ok($('#fs-src [data-v="t1"]').textContent.includes('31'), 'the source shows no temperature: ' + $('#fs-src [data-v="t1"]').textContent);
    $('#fs-src [data-v="gpu"]').click();
    ok(fanSer(FAN.draft.pts) === '30:30,69:20,70:80,85:100', 'back on the GPU, its curve is gone: ' + fanSer(FAN.draft.pts));
    // Cancel sends nothing; the reading keeps coming and leaves the draft and the focus alone
    key(c, 'ArrowUp'); const d = fanSer(FAN.draft.pts); c.focus(); await fanEmit(); await fanEmit();
    ok(fanSer(FAN.draft.pts) === d && document.activeElement === c, 'a reading reset the draft or the focus');
    const n = sent.length; $('#fs-cancel').click(); await wait(60);
    ok(sent.length === n && cv('fans', 'fan1_src', '') === 't1' && !fanDirty(FAN.draft), 'Cancel sent ' + (sent.length - n));
    // Reset puts the source's default into the draft, to be applied like any edit
    $('#fs-reset').click();
    ok(fanSer(FAN.draft.pts) === fanSer(fanParse(fanScale('t1').def)), 'reset: ' + fanSer(FAN.draft.pts));
    await fanReset();
  });

  test('fans: the chart is what fans.c sets: the curve first, then the floor', async () => {
    await fresh(); await fanReset();
    const p = fanParse('20:20,40:100');
    ok(fanOut(p, 30, 50) === 60 && fanOut(p, 22, 50) === 50 && fanOut(p, 40, 50) === 100 && fanAt(p, 30) === 60, 'fanOut: ' + fanOut(p, 30, 50));
    // a group whose fans have different floors draws one applied line per floor
    setCfg('fans', 'g1_name', 'Front'); setCfg('fans', 'g1', '1,3'); setCfg('fans', 'fan1', 'curve'); setCfg('fans', 'fan3', 'curve');
    setCfg('fans', 'fan1_min', '50'); setCfg('fans', 'fan3_min', '35'); await fanEmit();
    $('#fan-panel [data-g="1"] .fan-hd').click(); await wait(120);
    ok(FAN.open.g === 1 && FAN.open.i == null && !$('#fs-ctl').classList.contains('hidden'), 'the group did not open');
    await fanReset();
  });

  test('fans: the originals could not be saved, or the temperature went and the write failed: each says so', async () => {
    await fresh(); await fanReset();
    setCfg('fans', 'fan1', 'fix'); Object.assign(STATE.fans.f[0], { own: 0, ok: -1, err: 'save', want: -1, out: -1 }); await fanEmit();
    const row = () => $('#fan-panel [data-live-f="0"]').textContent;
    ok(row().includes(t('fans.st.save')) && !row().includes(t('fans.st.ok')), 'save failed: ' + row());
    Object.assign(STATE.fans.f[0], { own: 1, ok: -1, err: 'write', em: 'nosensor', want: 100 }); await fanEmit();
    ok(row().includes(t('fans.em.nosensor')) && row().includes(t('fans.em.fail', 100)) && row().includes(t('fans.err.write')) && !row().includes(t('fans.em.ok', 100)), 'emergency: ' + row());
    Object.assign(STATE.fans.f[0], { ok: 1, err: '' }); await fanEmit();
    ok(row().includes(t('fans.em.ok', 100)), 'emergency confirmed: ' + row());
    await fanReset();
  });

  test('fans: a fan opened inside its group keeps its own name and limits; the shared ones stay with the group', async () => {
    await fresh(); await fanReset();
    setCfg('fans', 'g1_name', 'Front'); setCfg('fans', 'g1', '1,3'); setCfg('fans', 'fan1', 'curve'); setCfg('fans', 'fan3', 'fix'); await fanEmit();
    $('#fan-panel [data-g="1"] .fan-hd').click(); await wait(120);
    ok(!$('#fs-diff').classList.contains('hidden') && $('#fs-diff').textContent.includes(t('fans.diff.mode')), 'the mode differs and nothing says so');
    $('#fan-panel [data-gm="2"]').click(); await wait(120);
    ok(FAN.open.g === 1 && FAN.open.i === 2 && !$('#fs-crumb').classList.contains('hidden') && !$('#fs-shared').classList.contains('hidden') && $('#fs-ctl').classList.contains('hidden'), 'the member view');
    const m = $('#fs-min'); m.value = '40'; m.dispatchEvent(new Event('change'));
    ok(cv('fans', 'fan3_min', '') === '40' && cv('fans', 'fan1_min', '') === '', 'the floor went to the wrong fan');
    $('#fs-up').click(); await wait(60);
    ok(FAN.open.g === 1 && FAN.open.i == null, 'back to the group');
    await fanReset();
  });

  test('fans: the history takes no old readings, marks a lost link once and keeps each source to itself', async () => {
    await fresh(); await fanReset();
    setCfg('fans', 'fan1', 'curve'); setCfg('fans', 'fan1_curve', '20:20,40:100'); await fanEmit(); await fanSheetOf(0);
    const F = STATE.fans, h = () => FAN.hist[0], older = () => h().forEach(p => { p.at -= 3000; });
    FAN.hist[0] = [];
    fanSample({ ...F, age: 9000 });
    ok(!h().length, 'a reading 9 s old went in as now');
    fanSample({ ...F, gage: 9000 });
    ok(h().length === 1 && h()[0].t == null && h()[0].src === 'gpu', 'an old GPU temperature went in: ' + JSON.stringify(h()));
    older(); Object.assign(F.f[0], { own: 1, want: 60, pct: 58, em: '' }); fanSample({ ...F, gage: 600 });
    ok(h().length === 2 && h()[1].t === 41 && h()[1].w === 60 && h()[1].p === 58, 'the sample: ' + JSON.stringify(h()[1]));
    fanSample({ ...F, link: 0 }); fanSample({ ...F, link: 0 });
    ok(h().filter(p => p.lost).length === 1, 'the lost link: ' + h().filter(p => p.lost).length);
    setCfg('fans', 'fan1_src', 't1'); F.f[0].em = 'nosensor'; fanSample({ ...F });
    const l = h()[h().length - 1];
    ok(l.src === 't1' && l.t === 31.4 && l.em === 'nosensor', 'after the change: ' + JSON.stringify(l));
    const s = fanHistText(h()[1], 65000, 't1');
    ok(s.includes(t('fans.gpu')) && s.includes('1:05') && s.includes(t('fans.hist.w', 60)) && s.includes(t('fans.hist.p', 58)), 'an older GPU reading: ' + s);
    ok(fanHistText(l, 0, 't1').includes(t('fans.em.nosensor')) && fanHistText(h()[2], 0, 't1') === t('fans.hist.lost'), 'the marks: ' + fanHistText(l, 0, 't1'));
    FAN.sig = ''; updateFans(); drawFanHist(0);
    ok($('#fs-legend').textContent.includes(fanDeg(31.4)) && !$('#fs-legend').textContent.includes(t('fans.gpu')), 'the legend: ' + $('#fs-legend').textContent);
    const b = $('#fs-hl [data-l="t"]'); b.click();
    ok(!FAN.hl.t && b.getAttribute('aria-pressed') === 'false', 'the line did not go off'); b.click();
    ok(FAN.hl.t && b.getAttribute('aria-pressed') === 'true', 'the line did not come back');
    F.f[0].em = ''; FAN.hist[0] = []; await fanReset();
  });

  test('fans: a saved curve gives the draft its points only, and is offered for its kind of source', async () => {
    await fresh(); await fanReset();
    ['1', '2'].forEach(n => setCfg('fans', 'fan' + n, 'curve')); setCfg('fans', 'fan1_min', '45'); setCfg('fans', 'fan2_min', '30');
    setCfg('fans', 'fan1_curve', '30:40,60:80,80:100'); await fanEmit(); await fanSheetOf(0);
    const nm = $('#fs-sv-name'), sv = $('#fs-sv');
    $('#fs-sv-save').click();
    ok(cv('fans', 'c1_name', '') === '' && $('#fs-sv-msg').textContent === t('fans.sv.noname'), 'saved without a name');
    nm.value = ' Quiet;er '; $('#fs-sv-save').click();
    ok(cv('fans', 'c1', '') === '30:40,60:80,80:100' && cv('fans', 'c1_k', '') === 'gpu' && cv('fans', 'c1_name', '') === 'Quieter', 'the save: ' + cv('fans', 'c1_name', ''));
    await fanSheetOf(1);
    ok(sv.options.length === 2 && sv.options[1].textContent === 'Quieter', 'not offered on fan 2');
    sv.value = '1'; sv.dispatchEvent(new Event('change'));
    ok(fanSer(FAN.draft.pts) === '30:40,60:80,80:100' && cv('fans', 'fan2_curve', '') === '', 'the draft: ' + fanSer(FAN.draft.pts));
    $('#fs-apply').click(); await wait(60);
    ok(cv('fans', 'fan2_curve', '') === '30:40,60:80,80:100' && cv('fans', 'fan2_min', '') === '30' && cv('fans', 'fan1_min', '') === '45', 'the limits moved');
    $('#fs-src [data-v="t1"]').click();
    ok(sv.options.length === 1 && sv.disabled, 'a GPU curve offered for the water');
    $('#fs-src [data-v="gpu"]').click(); sv.value = '1'; sv.dispatchEvent(new Event('change')); $('#fs-sv-del').click();
    ok(cv('fans', 'c1_name', '') === '' && cv('fans', 'c1', '') === '' && sv.options.length === 1, 'not deleted');
    await fanReset();
  });

  test('fans: limits are kept in 20..100 per fan; the group window keeps focus and says what a fan gets', async () => {
    await fresh(); await fanReset(); await fanSheetOf(1);
    $('#fs-lim').open = true;
    const m = $('#fs-min'); m.value = '5'; m.dispatchEvent(new Event('change'));
    ok(cv('fans', 'fan2_min', '') === '20' && m.value === '20', 'min: ' + cv('fans', 'fan2_min', ''));
    m.value = '35'; m.dispatchEvent(new Event('change'));
    closeSheet(); await wait(80);
    setCfg('fans', 'g1_name', 'Front'); setCfg('fans', 'g1', '1'); setCfg('fans', 'fan1', 'fix'); setCfg('fans', 'fan1_pct', '60'); FAN.sig = ''; updateFans();
    $('#fan-panel [data-g="1"]').click(); await wait(120); $('#fs-lim').open = true;
    const b2 = () => $('#fs-members [data-m="1"]');
    ok(b2().textContent.includes(t('fans.join', fanMode(0))) && b2().textContent.includes(t('fans.join.min', 35)), 'no preview: ' + b2().textContent);
    b2().focus(); const el = document.activeElement; updateFans(); updateFans();
    ok(document.activeElement === el && el.isConnected, 'a status took the focus away');
    b2().click(); await wait(60);
    ok(cv('fans', 'fan2', '') === 'fix' && cv('fans', 'fan2_pct', '') === '60' && cv('fans', 'fan2_min', '') === '35', 'joined: mode/pct copied, its own floor kept');
    ok(document.activeElement && document.activeElement.dataset.m === '1', 'focus lost after the click');
    await fanReset();
  });

  test('fans: "give every channel back" says what happened; long names stay inside the tile', async () => {
    await fresh(); await fanReset();
    setCfg('fans', 'fan1', 'fix'); setCfg('fans', 'fan3', 'curve'); setCfg('fans', 'fan3_name', 'A really long name for the pump'); await fanEmit();
    $('#fans-back').click();
    ok(cv('fans', 'fan1', '') === '' && cv('fans', 'fan3', '') === '', 'not every channel cleared');
    ok($('#fans-back-msg').textContent === t('fans.back.wait'), 'before the core: ' + $('#fans-back-msg').textContent);
    STATE.fans.rfail = 1; await fanEmit();
    ok($('#fans-back-msg').textContent === t('fans.back.fail', 1), 'a failure: ' + $('#fans-back-msg').textContent);
    STATE.fans.rfail = 0; await fanEmit();
    ok($('#fans-back-msg').textContent === t('fans.back.ok'), 'done: ' + $('#fans-back-msg').textContent);
    const tile = $('#fan-panel [data-f="2"]'), nm = tile.querySelector('b');
    ok(tile.scrollWidth <= tile.clientWidth + 1 && (nm.scrollWidth <= nm.clientWidth || getComputedStyle(nm).textOverflow === 'ellipsis') && nm.title === 'A really long name for the pump', 'the name spills out of the tile');
    STATE.fans.log = [{ t: '12:00:01', m: 'fan 1: read-back "differs"', n: 3 }]; await fanEmit();
    ok(!$('#fans-log-d').classList.contains('hidden') && $('#fans-log').textContent.includes(String.fromCharCode(215) + '3'), 'the error log');
    await fanReset();
  });

  test('fans: the board\'s channels follow the QUADRO\'s, and stay on their own without one', async () => {
    await fresh(); await fanReset();
    const F = STATE.fans, q = F.f.slice();
    const b = () => ({ b: 1, rpm: 900, pct: 40, own: 0, want: -1, out: -1, ok: 1, em: '', err: '' });
    F.chip = 'NCT6799D'; F.f.push(b(), Object.assign(b(), { rpm: -1, pct: null })); await fanEmit(); updateFans();
    ok($$('#fan-panel [data-live-f]').length === 6, 'tiles: ' + $$('#fan-panel [data-live-f]').length);
    const tile = $('#fan-panel [data-f="4"]').textContent, dead = $('#fan-panel [data-live-f="5"]').textContent;
    ok(tile.includes(t('fans.bch') + ' 1') && tile.includes(t('fans.asis.b')), 'the board tile: ' + tile);
    ok(dead.includes('—'), 'an unread board fan: ' + dead);
    ok($('#fans-h').textContent === 'QUADRO · NCT6799D', 'heading: ' + $('#fans-h').textContent);
    F.q = 0; delete F.link; F.f.splice(0, 4, null, null, null, null); await fanEmit(); updateFans();
    ok($$('#fan-panel [data-live-f]').length === 2 && !$('#fan-panel').classList.contains('stale'), 'without a QUADRO: ' + $$('#fan-panel [data-live-f]').length);
    ok($('#fans-h').textContent === 'NCT6799D', 'heading: ' + $('#fans-h').textContent);
    F.f.splice(0, F.f.length, ...q); F.q = 1; delete F.chip;
    await fanReset();
  });

  test('fans: the CPU is a source where it reads, never the unset one; no QUADRO, no QUADRO in the words', async () => {
    await fresh(); await fanReset();
    const F = STATE.fans;
    setCfg('fans', 'fan1', 'curve'); await fanEmit(); await fanSheetOf(0);
    ok(fanSrc(0) === 'gpu' && !$('#fs-src [data-v="cpu"]'), 'no CPU reading: ' + fanSrc(0));
    F.cpuok = 1; await fanEmit(); FAN.draft = null; updateFans();
    ok(fanSrc(0) === 'gpu' && $('#fs-src [data-v="cpu"]'), 'the CPU offered, the GPU kept: ' + fanSrc(0));
    setCfg('fans', 'fan1_src', 'cpu'); F.cpu = 52; await fanEmit(); FAN.draft = null; updateFans();
    const b = $('#fs-src [data-v="cpu"]');
    ok(fanSrc(0) === 'cpu' && b.classList.contains('on') && b.textContent.includes('52'), 'the CPU: ' + b.textContent);
    ok(fanCurveRaw(0) === fanScale('cpu').def && fanTemp('cpu') === 52, 'its own default curve: ' + fanCurveRaw(0));
    F.q = 0; await fanEmit(); updateFans();
    ok($('#fans-back').textContent === t('fans.back.all.b') && $('#fans-note').textContent === t('fans.note.b'), 'the board wording: ' + $('#fans-back').textContent);
    F.q = 1; delete F.cpu; delete F.cpuok; setCfg('fans', 'fan1_src', ''); await fanEmit(); updateFans();
    ok($('#fans-back').textContent === t('fans.back.all'), 'the QUADRO wording back: ' + $('#fans-back').textContent);
    await fanReset();
  });

  test('fans: Apply sends a group\'s curves and sources as one change, and Undo takes it back as one', async () => {
    await fresh(); await fanReset();
    setCfg('fans', 'g1_name', 'Front'); setCfg('fans', 'g1', '1,2'); ['1', '2'].forEach(n => setCfg('fans', 'fan' + n, 'curve')); await fanEmit();
    $('#fan-panel [data-g="1"] .fan-hd').click(); await wait(120);
    $('#fs-src [data-v="t1"]').click(); const n = sent.length; $('#fs-apply').click(); await wait(60);
    const out = sent.slice(n), fs = out.filter(o => o.cmd === 'fans_set');
    ok(fs.length === 1 && ['fan1_curve=', 'fan1_src=t1', 'fan2_curve=', 'fan2_src=t1', 'fan1_curve_gpu=', 'fan2_curve_gpu='].every(x => fs[0].kv.includes(x)), 'one change: ' + JSON.stringify(fs));
    ok(!out.some(o => o.cmd === 'set' && o.s === 'fans' && /_(curve|src)/.test(o.k)), 'a curve or a source went alone');
    ok(cv('fans', 'fan2_src', '') === 't1' && STATE.cfg.fans.fan2_src === 't1' && STATE.cfg.fans.fan1_curve === fanScale('t1').def, 'the core: ' + STATE.cfg.fans.fan1_curve);
    const m = sent.length; undo(); await wait(60);
    const b = sent.slice(m).filter(o => o.cmd === 'fans_set');
    ok(b.length === 1 && b[0].kv.split('\n').includes('fan1_src=') && cv('fans', 'fan2_src', 'gpu') === 'gpu' && STATE.cfg.fans.fan2_src === '', 'undo: ' + JSON.stringify(b));
    await fanReset();
  });

  test('fans: the tab is there for any fan controller, a Lian Li receiver alone too, and never on a phone', async () => {
    await fresh(); await fanReset();
    const F = STATE.fans, L = STATE.ll, keep = { on: F.on, q: F.q, st: L.st }, vis = s => !$(s).classList.contains('hidden') && !!$(s).offsetParent;
    // [the QUADRO or the board found, the QUADRO, the board chip, Lian Li state] -> tab
    for (const [on, q, chip, st, want] of [[0, 0, '', 'none', 0], [0, 0, '', 'ok', 1], [0, 0, '', 'err', 1], [0, 0, '', 'busy', 1], [1, 1, '', 'none', 1], [1, 0, 'NCT6799D', 'none', 1], [1, 1, 'NCT6799D', 'ok', 1]]) {
      Object.assign(F, { on, q }); if (chip) F.chip = chip; else delete F.chip; L.st = st; await fanEmit();
      const c = JSON.stringify([on, q, chip, st]);
      ok(NAV_NEED.fans() === !!want, 'the tab: ' + c);
      if (want) { showTab('fans'); await wait(30);
        ok(vis('#ll-card') === (st !== 'none') && vis('#fan-main') === !!on && vis('#tab-fans .fans-top') === !!on, 'the page: ' + c);
        if (!on) ok($('#ll-status').textContent === (st === 'ok' ? t('ll.st.ok', L.g.length) : t('ll.st.' + st, L.err || 0)), 'the receiver: ' + $('#ll-status').textContent);
      }
    }
    document.documentElement.classList.add('remote');
    ok(!NAV_NEED.fans(), 'a phone got the fans'); document.documentElement.classList.remove('remote');
    Object.assign(F, keep); delete F.chip; L.st = keep.st; await fanReset();
  });

  test('fans: a curve edit left for Fixed keeps Apply / Cancel in sight when another fan is picked; nothing goes out by itself', async () => {
    for (const grp of [0, 1]) {
      await fresh(); await fanReset();
      if (grp) { setCfg('fans', 'g1_name', 'Front'); setCfg('fans', 'g1', '1,2'); }
      ['1', '2'].forEach(n => setCfg('fans', 'fan' + n, 'curve')); await fanEmit();
      if (grp) { $('#fan-panel [data-g="1"] .fan-hd').click(); await wait(120); } else await fanSheetOf(0);
      const c = $('#fs-cv'), was = cv('fans', 'fan1_curve', ''); c.focus(); key(c, 'ArrowUp'); const d = fanSer(FAN.draft.pts);
      $('#fs-mode [data-v="fix"]').click(); await wait(60);
      const n = sent.length, o = JSON.stringify(FAN.open);
      $('#fan-panel [data-f="2"]').click(); await wait(120);
      const w = grp ? 'group: ' : 'fan: ';
      ok(JSON.stringify(FAN.open) === o && FAN.draft && fanSer(FAN.draft.pts) === d && fanDirty(FAN.draft), w + 'the draft or the open fan changed');
      ok(!$('#fs-draft').classList.contains('hidden') && !!$('#fs-draft').offsetParent && document.activeElement === $('#fs-apply'), w + 'Apply is not in sight');
      ok(!sent.slice(n).some(x => /curve|src/.test(x.k || x.kv || '')) && cv('fans', 'fan1_curve', '') === was && cv('fans', 'fan1', '') === 'fix', w + 'something went out');
      $('#fs-cancel').click(); await wait(60);
      ok(!fanDirty(FAN.draft) && $('#fs-draft').classList.contains('hidden') && cv('fans', 'fan1_curve', '') === was, w + 'Cancel');
      $('#fan-panel [data-f="2"]').click(); await wait(120);
      ok(FAN.open && FAN.open.i === 2, w + 'the other fan did not open after Cancel');
    }
    await fanReset();
  });

  test('fans: a reading that is not there stays a gap in the history, rpm and the read-back each on its own', async () => {
    await fresh(); await fanReset(); await fanSheetOf(0);
    const F = STATE.fans, f = F.f[0], keep = { rpm: f.rpm, pct: f.pct }, h = () => FAN.hist[0], older = () => h().forEach(p => { p.at -= 3000; });
    FAN.hist[0] = [];
    for (const [rpm, pct] of [[-1, null], [800, null], [0, 35], [900, 40]]) { older(); Object.assign(f, { rpm, pct }); fanSample({ ...F }); }
    ok(JSON.stringify(h().map(p => p.r)) === '[null,800,0,900]' && JSON.stringify(h().map(p => p.p)) === '[null,null,35,40]', 'the samples: ' + JSON.stringify(h()));
    const s = fanHistText(h()[0], 0, 'gpu');
    ok(s.includes('— ' + t('fans.rpmu')) && s.includes(t('fans.hist.p', '—')) && fanHistText(h()[2], 0, 'gpu').includes('0 ' + t('fans.rpmu')), 'the legend: ' + s);
    drawFanHist(0);
    Object.assign(f, keep); FAN.hist[0] = []; await fanReset();
  });

  test('fans: saved curves keep to their kind, one name in each; old ones without a kind are offered apart and get it by hand', async () => {
    await fresh(); await fanReset();
    const F = STATE.fans, sv = $('#fs-sv'), k = $('#fs-sv-k'), nm = $('#fs-sv-name'), pick = (e, v) => { e.value = v; e.dispatchEvent(new Event('change')); };
    [['40:30,70:100', 't', 'Old'], ['30:30,60:100', '', 'NoKind'], ['30:40,80:100', 'gpu', 'Gpu one']].forEach(([c, kk, n], j) => { setCfg('fans', `c${j + 1}`, c); setCfg('fans', `c${j + 1}_k`, kk); setCfg('fans', `c${j + 1}_name`, n); });
    F.cpuok = 1; F.cpu = 50; setCfg('fans', 'fan1', 'curve'); setCfg('fans', 'fan1_src', 'cpu'); await fanEmit(); await fanSheetOf(0);
    const names = () => [...sv.options].slice(1).map(o => o.parentNode.label + ': ' + o.textContent);
    ok(JSON.stringify(names()) === JSON.stringify([t('fans.sv.k.none') + ': Old', t('fans.sv.k.none') + ': NoKind']), 'on the CPU: ' + names());
    nm.value = 'Same'; $('#fs-sv-save').click();
    $('#fs-src [data-v="t1"]').click(); nm.value = 'Same'; $('#fs-sv-save').click();
    ok(cv('fans', 'c4_name', '') === 'Same' && cv('fans', 'c4_k', '') === 'cpu' && cv('fans', 'c5_name', '') === 'Same' && cv('fans', 'c5_k', '') === 'liq' && cv('fans', 'c4', '') !== cv('fans', 'c5', ''), 'one overwrote the other: ' + cv('fans', 'c5_k', ''));
    ok(names()[0] === t('fans.sv.k.liq') + ': Same' && names().length === 3, 'on the water: ' + names());
    pick(sv, '1');
    ok(!k.classList.contains('hidden') && fanSer(FAN.draft.pts) === '40:30,70:100', 'an old one: no category choice, or not taken');
    pick(k, 'liq');
    ok(cv('fans', 'c1_k', '') === 'liq' && k.classList.contains('hidden') && sv.value === '1' && $('#fs-sv-msg').textContent === t('fans.sv.k.done', 'Old', t('fans.sv.k.liq')), 'set: ' + cv('fans', 'c1_k', ''));
    setCfg('fans', 'c6', '35:30,50:100'); setCfg('fans', 'c6_name', 'Same'); fanSavedList(FAN.draft); pick(sv, '6'); pick(k, 'cpu');
    ok(cv('fans', 'c6_k', '') === '' && $('#fs-sv-msg').textContent === t('fans.sv.k.taken', 'Same', t('fans.sv.k.cpu')), 'two of one name in one kind: ' + cv('fans', 'c6_k', ''));
    $('#fs-src [data-v="gpu"]').click();
    ok(names()[0] === t('fans.sv.k.gpu') + ': Gpu one' && !names().some(x => /Same|Old/.test(x) && !x.startsWith(t('fans.sv.k.none'))), 'on the GPU: ' + names());
    delete F.cpuok; delete F.cpu; await fanReset();
  });

  test('fans: the chart runs as fans.c does past its range; a point out there is kept, picked by keys, never by the edge', async () => {
    await fresh(); await fanReset();
    const p = fanParse('25:30,35:70,110:100'), sc = fanScale('t1');
    ok(fanAt(p, 55) === 78 && fanOut(p, 20, 50) === 50 && fanOut(p, 55, 50) === 78 &&JSON.stringify(fanVis(p, sc)) === '[[15,30],[25,30],[35,70],[55,78]]', 'fanVis: ' + JSON.stringify(fanVis(p, sc)));
    ok(JSON.stringify(fanVis(fanParse('0:40,110:100'), fanScale('cpu'))) === '[[20,51],[100,95]]' && JSON.stringify(fanVis(fanParse('20:20,100:100'), fanScale('cpu'))) === '[[20,20],[100,100]]', 'the ends');
    setCfg('fans', 'fan1', 'curve'); setCfg('fans', 'fan1_src', 't1'); setCfg('fans', 'fan1_curve', '25:30,35:70,110:100'); setCfg('fans', 'fan1_min', '50'); await fanEmit(); await fanSheetOf(0);
    const c = $('#fs-cv'), g = fanGeo(c, sc), r = c.getBoundingClientRect(), ev = (n, x, y) => c.dispatchEvent(new PointerEvent(n, { clientX: r.left + g.px(x), clientY: r.top + g.py(y), pointerId: 1, bubbles: true }));
    c.setPointerCapture = () => {};
    ev('pointerdown', 55, 100);
    ok(!FAN.drag, 'the point at 110 was picked at the edge');
    ev('pointerdown', 35, 70); ev('pointermove', 40, 80); ev('pointerup', 40, 80);
    ok(fanSer(FAN.draft.pts) === '25:30,40:80,110:100', 'a drag moved another point: ' + fanSer(FAN.draft.pts));
    key(c, 'ArrowRight'); key(c, 'ArrowDown');
    ok(FAN.sel === 2 && fanSer(FAN.draft.pts) === '25:30,40:80,110:95' && $('#fs-pt-x').value === '110', 'the keys: ' + fanSer(FAN.draft.pts));
    $('#fs-src [data-v="gpu"]').click(); $('#fs-src [data-v="t1"]').click();
    ok(fanSer(FAN.draft.pts) === '25:30,40:80,110:95', 'a source switch changed it: ' + fanSer(FAN.draft.pts));
    $('#fs-apply').click(); await wait(60);
    ok(STATE.cfg.fans.fan1_curve === '25:30,40:80,110:95', 'applied: ' + STATE.cfg.fans.fan1_curve);
    delete c.setPointerCapture; await fanReset();
  });

  test('a group: Colour and its swatch keep the group window open, the colour goes to every member', async () => {
    await fresh();
    const n = groupNew(); groupDialog(n, true);
    const zs = heroItems(true).slice(0, 2).map(i => i.zone); zs.forEach(z => groupAdd(n, z)); gdlgDevs();
    $('#gdlg-zone .seg button[data-m="static"]').click();
    ok(!$('#gdlg').classList.contains('hidden'), 'Colour closed the group window');
    $('#gdlg-zone .swatches .sw').click();
    ok(!$('#gdlg').classList.contains('hidden') && !pk.classList.contains('hidden'), 'the swatch closed the group window or opened no picker');
    PK.onChange('#112233');
    ok(zs.every(z => cv(z, 'palette', '').startsWith('#112233') && cv(z, 'mode', '') === 'static'), 'members: ' + zs.map(z => cv(z, 'mode', '') + ' ' + cv(z, 'palette', '')).join(' / '));
    $('#gdlg-cancel').click();
  });

  test('TRYX: your own picture opens the builder on the picked file, its sending shows in the status', async () => {
    await fresh();
    STATE.ext.devs.push({ id: 14, kind: 'tryx', title: 'TRYX PANORAMA', name: 'TRYX PANORAMA', host: '18D1:2D03', sub: 0, leds: 1, per_led: 0, online: 1, enabled: 1, type: 'screen', info: 'TRYX PANORAMA · COM5 · USB' });
    STATE.cfg['dev.14'] = { kind: 'tryx' }; await fanEmit();
    const card = () => $('#dev-st-14').closest('.card'), sel = card().querySelector('.dev-theme');
    ok([...sel.options].some(o => o.value === '-1'), 'no option for your own picture');
    sel.value = '-1'; sel.dispatchEvent(new Event('change', { bubbles: true }));
    ok(!$('#txb').classList.contains('hidden') && TXB.shape === '16:9' && !sets('dev.14', 'theme').length && sel.value !== '-1', 'picking "your own" without a file: ' + JSON.stringify(sent));
    txbClose(); Object.assign(STATE.cfg['dev.14'], { media: 'C:\\Pictures\\cat.gif', theme: '-1' }); await fanEmit();
    ok(card().querySelector('.dev-media-name').textContent === 'cat.gif' && card().querySelector('.dev-theme').value === '-1', 'the picked file: ' + card().querySelector('.dev-media-name').textContent);
    STATE.ext.devs[STATE.ext.devs.length - 1].info += ' · upload 42%'; await fanEmit();
    ok($('#dev-st-14').textContent === t('tx.up', 42), 'the status while sending: ' + $('#dev-st-14').textContent);
    STATE.ext.devs[STATE.ext.devs.length - 1].info = 'TRYX PANORAMA · COM5 · USB · upload failed · no adb'; await fanEmit();
    ok($('#dev-st-14').textContent === t('tx.up.noadb'), 'the status without adb: ' + $('#dev-st-14').textContent);
    ok(!card().querySelector('.live, .zone, .dev-type, .dev-cal, .dev-ident'), 'the screen card has no light settings');
    STATE.ext.devs.pop(); delete STATE.cfg['dev.14']; await fanEmit();
  });

  test('TRYX: the screen builder makes a JPEG from a still with a text over it and hands it to the core', async () => {
    await fresh();
    STATE.ext.devs.push({ id: 14, kind: 'tryx', title: 'TRYX PANORAMA', name: 'TRYX PANORAMA', host: '18D1:2D04', sub: 0, leds: 1, per_led: 0, online: 1, enabled: 1, type: 'screen', info: 'TRYX PANORAMA · COM5 · USB' });
    STATE.cfg['dev.14'] = { kind: 'tryx' }; await fanEmit();
    const card = () => $('#dev-st-14').closest('.card');
    card().querySelector('.dev-build').click();
    ok(!$('#txb').classList.contains('hidden') && document.activeElement === $('#txb-src') && TXB.shape === '2:1' && $('#txb-shape').value === '2:1', 'the builder did not open in 2:1');
    const c = document.createElement('canvas'); c.width = 400; c.height = 400; c.getContext('2d').fillStyle = '#c33'; c.getContext('2d').fillRect(0, 0, 400, 400);
    await txbLoad(new File([await new Promise(r => c.toBlob(r, 'image/png'))], 'red.png', { type: 'image/png' }));
    ok(TXB.src && !TXB.src.dur && $('#txb-msg').textContent === '', 'the still: ' + $('#txb-msg').textContent);
    txbMove(.5, 0); ok(TXB.cx === .5, 'a square in 2:1 moved sideways: ' + TXB.cx);
    txbMove(0, .5); ok(TXB.cy < .5 && TXB.cy > .2, 'it did not move up within its edge: ' + TXB.cy);
    $('#txb-text').click(); const ti = $('#txb-layers .txb-text');
    ok(TXB.layers.length === 1 && document.activeElement === ti, 'no text row');
    ti.value = 'CPU'; ti.dispatchEvent(new Event('input', { bubbles: true }));
    ok(TXB.layers[0].text === 'CPU' && TXB.sel === 0, 'the text: ' + TXB.layers[0].text);
    txbMove(.1, 0); ok(Math.abs(TXB.layers[0].x - .6) < 1e-9, 'the chosen text did not move');
    sent.length = 0; await txbMake();
    const o = sent.find(x => x.cmd === 'tryx_build');
    ok(o && o.id === '14' && o.ext === 'jpg' && o.data.startsWith('/9j/'), 'sent: ' + JSON.stringify(o && { ...o, data: o.data.slice(0, 8) }));
    ok($('#txb').classList.contains('hidden') && !TXB, 'the builder stayed open');
    await wait(150); await fanEmit();
    ok(card().querySelector('.dev-media-name').textContent === 'tryx-14.jpg', 'the file: ' + card().querySelector('.dev-media-name').textContent);
    STATE.ext.devs.pop(); delete STATE.cfg['dev.14']; await fanEmit();
  });

  test('OpenRGB: a board whose connectors have 0 LEDs says it needs setup, even switched off, and opens its LED counts', async () => {
    await fresh();
    STATE.ext.devs.push({ id: 15, kind: 'openrgb', title: 'OpenRGB', name: 'X870E', host: '127.0.0.1', sub: 5, leds: 0, per_led: 1, online: 0, enabled: 0, type: 'board', info: 'OpenRGB · set the LEDs on its connectors', zones: 'Addressable RGB Header 1:0:0:120|Addressable RGB Header 2:0:0:120' });
    STATE.cfg['dev.15'] = { kind: 'openrgb', enabled: '0' }; await fanEmit();
    ok($('#dev-st-15').textContent === t('zones.need'), 'the status: ' + $('#dev-st-15').textContent);
    ok($('#dev-st-15').closest('.card').querySelector('details.dev-adv').open, 'the LED counts are not open');
    STATE.ext.devs.pop(); delete STATE.cfg['dev.15']; await fanEmit();
  });

  test('calibrate: each ARGB connector of a board gets its own step, its ends counted from where it starts', async () => {
    await fresh();
    STATE.ext.devs.push({ id: 16, kind: 'openrgb', title: 'OpenRGB', name: 'X870E', host: '127.0.0.1', sub: 6, leds: 88, per_led: 1, online: 1, enabled: 1, type: 'board', info: 'OpenRGB · X870E · motherboard', zones: 'Header 1:40:0:120:8|Header 2:40:0:120:48' });
    STATE.cfg['dev.16'] = { kind: 'openrgb' }; await fanEmit();
    calOpen(S.ext.devs.find(x => x.id === 16));
    ok($('#cal-step').textContent === t('cal.step', 1, 4) && $('#cal-text').textContent === t('cal.zone', 'Header 1'), 'the first step: ' + $('#cal-step').textContent + ' / ' + $('#cal-text').textContent);
    const r = $('#cal-z'); r.value = 24; r.dispatchEvent(new Event('input', { bubbles: true }));
    const cal = () => sent.filter(o => o.cmd === 'calib').pop();
    ok(cal().mode === 3 && cal().from === 8 && cal().n === 24, 'connector 1: ' + JSON.stringify(cal()));
    ok(cv('dev.16', 'zones', '') === 'Header 1=24|Header 2=40', 'saved: ' + cv('dev.16', 'zones', ''));
    $('#cal-next').click();
    ok(cal().mode === 3 && cal().from === 32 && cal().n === 40, 'connector 2 starts after the new count of 1: ' + JSON.stringify(cal()));
    $('#cal-x').click();
    ok(cal().mode === 0, 'closing leaves the pattern on');
    STATE.ext.devs.pop(); delete STATE.cfg['dev.16']; await fanEmit();
  });

  for (const [name, fn] of tests) {
    try { await fn(); log.push('ok   ' + name); }
    catch (e) { R.failed++; log.push('FAIL ' + name + ': ' + e.message); }
  }
  R.done = true;
  const box = document.createElement('pre');
  box.style.cssText = 'position:fixed;z-index:99999;top:0;left:0;right:0;margin:0;padding:8px;font:12px monospace;background:' + (R.failed ? '#400' : '#032') + ';color:#fff;white-space:pre-wrap';
  box.textContent = log.join('\n'); document.body.appendChild(box);
  console.log('TEST:\n' + log.join('\n'));
})();
