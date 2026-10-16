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
    const day = d => $(`#prof-list .prof-day[data-d="${d}"]`).click(), days = () => cv('profile.3', 'days', '');
    day(7); ok(days() === '123456', 'Sunday off: ' + days());
    day(6); ok(days() === '12345', 'Saturday off: ' + days());
    day(7); day(6); ok(days() === '', 'all back is every day: ' + days());
    for (const d of '123456') day(d);
    day(7); ok(days() === '7', 'the last day went: ' + days());
    profMenu(false);
  });

  test('two profiles at the same minute and day show the clash; the clock switched off does not', async () => {
    await fresh();
    setCfg('profile.2', 'at', '07:30'); setCfg('profile.2', 'days', '12345'); setCfg('profile.3', 'at', '07:30'); setCfg('profile.3', 'days', '5');
    setCfg('profile.3', 'at_off', '');
    profMenu(true); $('#prof-list .prof-row[data-id="3"] .prof-ren').click();
    ok($('#prof-list .prof-edit .hint.warn'), 'no clash shown');
    $('#prof-list .prof-day[data-d="6"]').click(); $('#prof-list .prof-day[data-d="5"]').click();   // Saturday only
    ok(!$('#prof-list .prof-edit .hint.warn'), 'a clash on days not in common');
    setCfg('profile.3', 'days', '5'); setCfg('profile.2', 'at_off', '1'); renderProfiles();
    ok(!$('#prof-list .prof-edit .hint.warn'), 'a clash with a clock switched off');
    setCfg('profile.2', 'at', ''); setCfg('profile.3', 'at', ''); profMenu(false);
  });

  test('pause shows on the switcher and is cleared by resume; Duplicate asks the core for a copy', async () => {
    await fresh();
    setCfg('profile.1', 'apps', 'cs2.exe'); profMenu(true);
    $('#prof-state .prof-pause').click();
    ok(cv('general', 'auto_pause', '') === '1' && !$('#prof-paused').classList.contains('hidden'), 'not paused');
    $('#prof-state .prof-pause').click();
    ok(cv('general', 'auto_pause', '') === '' && $('#prof-paused').classList.contains('hidden'), 'not resumed');
    $('#prof-list .prof-row[data-id="2"] .prof-ren').click(); sent.length = 0; $('#prof-list .prof-dup').click();
    ok(sent.some(o => o.cmd === 'profile_dup' && o.id === '2' && o.name), 'sent ' + JSON.stringify(sent));
    setCfg('profile.1', 'apps', ''); profMenu(false);
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
