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
    F.f.forEach(f => Object.assign(f, { em: '', own: 0, want: -1, out: -1, ok: 1 }));
    Object.keys(STATE.cfg.fans || {}).forEach(k => { STATE.cfg.fans[k] = ''; });
    if (SHEET.kind === 'fan') closeSheet();
    await fanEmit(); showTab('fans'); FAN.back = 0; FAN.sig = ''; updateFans();
  };
  const fanSheetOf = async i => { $(`#fan-panel [data-f="${i}"]`).click(); await wait(120); };

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
    setCfg('fans', 'fan1_curve', '30:30,50:45,70:75,85:100'); updateFans();
    ok($('#fs-bad').classList.contains('hidden'), 'the warning stays on a good curve');
    FAN.sel = 1; updateFans();
    const x = $('#fs-pt-x'), y = $('#fs-pt-y');
    x.value = '999'; x.dispatchEvent(new Event('change')); y.value = '5'; y.dispatchEvent(new Event('change')); await wait(400);
    ok(cv('fans', 'fan1_curve', '') === '30:30,69:20,70:75,85:100', 'saved: ' + cv('fans', 'fan1_curve', ''));
    x.value = ''; x.dispatchEvent(new Event('change'));
    ok(x.value === '69', 'an empty field did not come back: ' + x.value);
    // the keyboard: right goes to the next point, up adds five
    const c = $('#fs-cv'); c.focus(); key(c, 'ArrowRight'); key(c, 'ArrowUp'); await wait(400);
    ok(cv('fans', 'fan1_curve', '') === '30:30,69:20,70:80,85:100', 'keys: ' + cv('fans', 'fan1_curve', ''));
    // another kind of source starts from its own default
    $('#fs-src [data-v="t1"]').click();
    ok(cv('fans', 'fan1_src', '') === 't1' && cv('fans', 'fan1_curve', '') === '', 'the GPU curve went to the water sensor');
    ok($('#fs-src [data-v="t1"]').textContent.includes('31'), 'the source shows no temperature: ' + $('#fs-src [data-v="t1"]').textContent);
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
    $('#fan-panel [data-g="1"]').click(); await wait(120);
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
    ok(tile.scrollWidth <= tile.clientWidth + 1 && nm.scrollWidth > nm.clientWidth, 'the name spills out of the tile');
    STATE.fans.log = [{ t: '12:00:01', m: 'fan 1: read-back "differs"', n: 3 }]; await fanEmit();
    ok(!$('#fans-log-d').classList.contains('hidden') && $('#fans-log').textContent.includes(String.fromCharCode(215) + '3'), 'the error log');
    await fanReset();
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
