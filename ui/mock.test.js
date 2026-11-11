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

  test('a profile switch the core refuses: said, the one in use stays, its unsaved change kept, closed by hand', async () => {
    const was = cv('general', 'profile', ''), other = profiles().find(p => String(p.id) !== was);
    try {
      await fresh(); profErrShow('');
      setCfg('flow', 'speed', '6');
      window.mockProfFail = 2; profGo(other.id); await wait(30);
      ok($('#prof-btn').classList.contains('busy'), 'not shown as on its way');
      await wait(150);
      const box = $('#prof-err'), cur = profiles().find(p => String(p.id) === was);
      ok(!box.classList.contains('hidden') && $('span', box).textContent === t('prof.err.leave', cur.name), 'the refusal: ' + $('span', box).textContent);
      ok(box.getAttribute('role') === 'alert' && $('.x', box).getAttribute('aria-label') === t('close'), 'not read out or no name on its close');
      ok(cv('general', 'profile', '') === was && $('#prof-cur').textContent === cur.name && !$('#prof-btn').classList.contains('busy'), 'the switcher shows the refused one: ' + $('#prof-cur').textContent);
      ok(cv('flow', 'speed', '') === '6', 'the unsaved change is gone');
      $('.x', box).click();
      ok(box.classList.contains('hidden') && document.activeElement === $('#prof-btn'), 'not closed or the focus lost');
      window.mockProfFail = 4; profGo(other.id); await wait(180);
      ok($('span', box).textContent === t('prof.err.bad', other.name), 'a damaged file: ' + $('span', box).textContent);
      window.mockProfFail = 0; profGo(other.id); await wait(220);
      ok(cv('general', 'profile', '') === String(other.id) && box.classList.contains('hidden'), 'the retry did not switch or the old error stays');
    } finally { window.mockProfFail = 0; profErrShow(''); if (was) { profGo(+was); await wait(220); } }
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

  test('the old pages land on the new ones: a brand or the PC tab opens the catalogue filtered; the AI offer waits for a try or a download', async () => {
    await fresh();
    for (const [p, want] of [['nano', { brand: 'Nanoleaf' }], ['bulbs', { brand: 'AiDot' }], ['pc', { where: 'pc' }], ['brand-divoom', { brand: 'Divoom' }]]) {
      showTab(p);
      const fits = x => Object.entries(want).every(([k, v]) => x[k] === v);
      ok(tab === 'devices' && $('#tab-devices').classList.contains('active') && fits(CAT) && !CAT.q && $('#title').textContent === t('nav.devices'), p + ': ' + tab + ' ' + JSON.stringify(CAT));
      ok(CATV.length && CATV.every(fits) && $$('#cat-grid .cat-tile').length === CATV.length, p + ': not its devices: ' + CATV.map(x => x.name));
    }
    showTab('effects'); ok(tab === 'effects' && $('#tab-effects').classList.contains('active'), 'Lighting moved: ' + tab);
    showTab('fans'); ok(tab === 'fans', 'Cooling moved: ' + tab);
    showTab('nosuch'); ok(tab === 'home', 'an unknown page: ' + tab);
    $('#cat-reset').click();
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

  test('a fan added on Home is a tile there: it opens the fan in the sheet, and the groups never list it', async () => {
    await fresh();
    showTab('home');
    const f = fanWidgets()[0];
    ok(f && !heroItems().some(it => it.k === 'fan'), 'a fan is on Home before it was added');
    setCfg('ui', 'hero_fans', f.key); setCfg('ui', 'hero_order', f.key); drawHero();   // first: the compact preview shows one row
    ok(heroItems().some(it => it.key === f.key) && !heroItems(true).some(it => it.k === 'fan'), 'the fan tile is missing, or the groups list it');
    [...$$('#hero-keys button')].find(b => b.getAttribute('aria-label') === f.name).click(); await wait(60);
    ok(SHEET.kind === 'fan' && $('#sheet #fan-set') && tab === 'home', 'the fan did not open in the sheet on Home');
    closeSheet(); setCfg('ui', 'hero_fans', ''); setCfg('ui', 'hero_order', '');
  });

  test('the pencil opens the profile in its own panel: the menu closes, Enter keeps a new name, Esc keeps the old one', async () => {
    await fresh();
    showTab('home');   // the profile button lives here: focus comes back to it
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

  test('this PC on Devices: the light tiles come first, the found hardware folds once haku lights some, what needs a step stays out', async () => {
    await fresh(); showTab('devices');
    const kids = [...$('#tab-devices').children];
    ok(kids.indexOf($('#cat-grid')) < kids.indexOf($('#pc-tools')) && $('#pc-tools .hw-card') && $('#ram-card').closest('#dev-store') && $('#gpu-card').closest('#dev-store'), 'the hardware list is above the light tiles, or a light card left its store');
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

  test('memory: the card and the hardware row say who lights each stick, from the core\'s ram_src; a maybe is explained', async () => {
    await fresh(); showTab('devices');
    const keep = { src: STATE.ram_src, own: STATE.ram_own }, it = { cat: 'ram', maker: 'G.Skill', name: 'F4', id: '' };
    const at = async (src, own) => { STATE.ram_src = src; STATE.ram_own = own; emit(STATE); await wait(60); };
    try {
      await at([0, 0], 0);
      ok($('#ram-status').textContent.endsWith(t('ram.src.haku')) && $('#ram-unsure').classList.contains('hidden') && hwRoute(it).cls === 'on', 'haku: ' + $('#ram-status').textContent);
      await at([1, 1], 0);
      ok($('#ram-status').textContent.endsWith(t('ram.src.orgb')) && hwRoute(it).text === t('ram.src.orgb'), 'OpenRGB: ' + $('#ram-status').textContent);
      await at([0, 2], 0);
      const x = $('#ram-status').textContent;
      ok(x.includes(t('ram.stick', 1, t('ram.src.haku'))) && x.includes(t('ram.stick', 2, t('ram.src.unsure'))), 'per stick: ' + x);
      ok(!$('#ram-unsure').classList.contains('hidden') && hwRoute(it).cls === 'warn', 'the maybe is not explained');
      await at([0, 0], 1);
      ok($('#ram-status').textContent.endsWith(t('ram.src.other')) && hwRoute(it).cls === 'off', 'given back: ' + $('#ram-status').textContent);
      STATE.ram_src = undefined; await at(undefined, 0);
      ok($('#ram-status').textContent.endsWith(t('ram.src.haku')), 'an older core without ram_src');
    } finally { await at(keep.src, keep.own); }
  });

  test('hardware nothing lights: Ask for support fills the report with the device and the diagnostics, and sends nothing', async () => {
    await fresh(); showTab('devices');
    const was = S.hw, o = S.orgb;
    try {
      S.hw = { busy: 0, done: 1, items: [{ cat: 'usb', maker: 'Lian Li', name: 'Strimer', id: '0CF2:A200' }, { cat: 'board', maker: 'ASRock', name: 'B650', id: '' }] };
      S.orgb = { state: 2, ctls: [] }; updateHw();
      ok(hwRoute(S.hw.items[0]).act === 'ask' && !hwRoute(S.hw.items[1]).act, 'routes: ' + JSON.stringify(S.hw.items.map(hwRoute)));
      const b = $('.hw-card [data-hw="0"]');   // folded under the rest when haku lights something else
      ok(b && b.textContent === t('hw.ask') && !$('.hw-card [data-hw="1"]'), 'the button: ' + (b && b.textContent));
      $('#report-attach').checked = false; sent.length = 0;
      b.click(); await wait(80);
      const x = $('#report-text').value;
      ok(!$('#report-dlg').classList.contains('hidden') && $('#report-attach').checked, 'the report is not open with the diagnostics');
      ok(x.includes('Lian Li Strimer') && x.includes('0CF2:A200') && /\n\[support usb 0CF2:A200\]$/.test(x) && !/hw\.ask/.test(x), 'text: ' + x);
      ok(x.length > 10 && !sent.some(m => m.cmd === 'report'), 'sent on its own: ' + JSON.stringify(sent));
    } finally { closeReport(); $('#report-text').value = ''; S.hw = was; S.orgb = o; updateHw(); }
  });

  test('hardware: a Corsair LCD is haku’s own screen, never asked for; other Corsair devices still go through OpenRGB', async () => {
    await fresh(); showTab('devices');
    const was = S.hw, o = S.orgb, devs = S.ext.devs;
    try {
      const lcd = { cat: 'usb', maker: 'Corsair', name: 'CORSAIR iCUE LINK AIO LCD Screen Module', id: '1B1C:0C4E' }, kb = { cat: 'usb', maker: 'Corsair', name: 'K70', id: '1B1C:1B49' };
      S.hw = { busy: 0, done: 1, items: [lcd, kb] }; S.orgb = { state: 2, ctls: [] }; S.ext.devs = devs.filter(d => d.kind !== 'corsairlcd');
      let r = hwRoute(lcd);
      ok(r.act === 'scan' && r.text === t('hw.haku.add') && hwRoute(kb).act === 'ask', 'not added yet: ' + JSON.stringify([r, hwRoute(kb)]));
      S.ext.devs = devs.concat([{ id: 77, kind: 'corsairlcd', host: '1B1C:0C4E', own: 0, online: 1, enabled: 1 }]);
      r = hwRoute(lcd); ok(r.cls === 'on' && r.text === t('hw.haku') && !r.act, 'added: ' + JSON.stringify(r));
      ok(hwRoute({ cat: 'usb', id: '1B1C:0C42' }).act === 'scan', 'an XC7 on the same PC is its own device');
      // a board's USB light controller: OpenRGB types it motherboard
      const asr = { cat: 'usb', maker: 'ASRock', name: 'LED Controller', id: '26CE:01A2' };
      S.orgb = { state: 2, ctls: [{ i: 0, name: 'ASRock Polychrome USB', type: 0, location: 'HID: x', serial: '' }, { i: 1, name: 'Corsair Vengeance', type: 1, location: 'I2C: x', serial: '' }] };
      r = hwRoute(asr); ok(r.act === 'add' ? r.ctls.length === 1 && r.ctls[0].i === 0 : r.text === t('hw.orgb'), 'ASRock USB: ' + JSON.stringify(r));   // the mock may have it added
      ok(hwRoute(kb).act === 'ask', 'a stick or a board of another maker is not the K70');
    } finally { S.hw = was; S.orgb = o; S.ext.devs = devs; updateHw(); }
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

  test('programs in the way: running is only possible; Leave, Close, End with a warning; Each time not for cooling', async () => {
    await fresh();
    const rows = () => [...$('#cf-list').children], row = id => rows().find(r => r.dataset.id === id), btn = (id, a) => row(id).querySelector(`[data-act="${a}"]`);
    ok(!$('#cf-card').classList.contains('hidden') && rows().length === 3 && $('#cf-status').textContent === t('cf.st.run', 2), 'card, rows or status');
    const arm = row('armoury').textContent;
    ok(arm.includes(t('cf.run')) && arm.includes(t('cf.cool')) && !arm.includes(t('cf.dev.held')), 'a running one claimed as certain: ' + arm);
    ok(btn('armoury', 'close') && btn('armoury', 'end') && btn('armoury', 'leave') && !btn('icue', 'close') && btn('icue', 'del'), 'actions only on the running ones, Remove on the installed');
    ok(btn('armoury', 'auto').disabled && btn('icue', 'auto').disabled && !btn('asrock', 'auto').disabled, 'Each time offered for a cooling program');
    btn('asrock', 'auto').click(); await wait(20);
    ok(cv('conflicts', 'stop', '') === 'asrock' && btn('asrock', 'auto').checked, 'Each time did not reach the settings');
    setCfg('conflicts', 'stop', 'icue,armoury,asrock'); updateConflicts();
    ok(row('icue').textContent.includes(t('cf.auto.paused')) && !btn('icue', 'auto').checked, 'an old rule on a cooling program not shown as paused');
    btn('armoury', 'leave').click(); await wait(20);
    ok(cv('conflicts', 'leave', '') === 'armoury' && cv('conflicts', 'stop', '') === 'icue,asrock', 'Leave did not keep it off the rule: ' + cv('conflicts', 'stop', ''));
    ok(row('armoury').textContent.includes(t('cf.left')) && !btn('armoury', 'close') && btn('armoury', 'unleave'), 'left to it, yet still offered to close');
    btn('armoury', 'unleave').click(); await wait(20);
    ok(cv('conflicts', 'leave', '') === '' && btn('armoury', 'close'), 'Undo');
    setCfg('conflicts', 'stop', '');
    // Close asks; this one only hides in the tray, and the row says so instead of claiming it closed
    btn('asrock', 'close').click(); await wait(60);
    ok(row('asrock').textContent.includes(t('cf.busy')) && !btn('asrock', 'close'), 'no Closing state');
    await wait(700);
    ok(row('asrock').textContent.includes(t('cf.fail.soft')) && btn('asrock', 'end'), 'a program left in the tray shown as closed');
    // End: nothing without a yes; the cooling one's warning says the fans are not taken over
    const conf = window.confirm; let asked = '';
    window.confirm = m => (asked = m, false);
    sent.length = 0; btn('armoury', 'end').click();
    ok(!sent.some(o => o.cmd === 'cf_end') && asked.includes(t('cf.end.q', 'ASUS Armoury Crate / Aura')) && asked.includes(t('cf.end.cool')), 'End went ahead or did not warn: ' + asked);
    window.confirm = m => (asked = m, true);
    btn('asrock', 'end').click(); await wait(20);
    window.confirm = conf;
    ok(sent.some(o => o.cmd === 'cf_end' && o.id === 'asrock') && !asked.includes(t('cf.end.cool')), 'End not sent, or the cooling warning on a lights-only program');
    await wait(700);
    ok(row('asrock').textContent.includes(t('cf.inst')) && $('#cf-status').textContent === t('cf.st.run', 1), 'not ended after the core said so');
    const a = S.cf; S.cf = { busy: '', a: [] }; updateConflicts();
    ok($('#cf-card').classList.contains('hidden'), 'shown with nothing in the way');
    S.cf = a; updateConflicts();
  });

  test('programs in the way: a refused device is confirmed, its program named only while it runs; a stranger is left alone', async () => {
    await fresh();
    const cf = S.cf, ll = S.ll, devs = S.ext.devs;
    const lc = { id: 'lianli', name: 'Lian Li L-Connect', w: 'fans', cool: 1, run: 0, inst: 1, bad: 0, fail: 0 };
    const kn = { id: 'tryx', name: 'TRYX KANALI', w: 'cooler', cool: 1, run: 0, inst: 0, bad: 1, fail: 0 };
    S.cf = { busy: '', a: cf.a.concat(lc, kn) }; S.ll = { st: 'busy', err: 5, g: [] };
    updateConflicts(); updateLL();
    const row = id => [...$('#cf-list').children].find(r => r.dataset.id === id), st = () => $('#ll-status').textContent;
    ok(st() === t('ll.st.busy', 5) + ' · ' + t('cf.why.unk') && !/L-Connect/.test(st()), 'the refusal pinned on a program not running: ' + st());
    ok(row('lianli').textContent.includes(t('cf.dev.held')), 'the refused device not under its program');
    lc.run = 1; updateLL();
    ok(st().includes(t('cf.why.run', 'Lian Li L-Connect')), 'a running L-Connect not named as the likely one: ' + st());
    ok(row('tryx').textContent.includes(t('cf.bad')) && !row('tryx').querySelector('[data-act="close"],[data-act="end"],[data-act="leave"]'), 'a stranger with its name offered to close');
    const d = { i: 77, kind: 'tryx', name: 'TRYX PANORAMA', enabled: 1, online: 0, own: 0, pro: 0, info: 'COM5 busy' };
    ok(devStatus(d) === t('tx.busy') + ' · ' + t('cf.why.unk'), 'TRYX busy: ' + devStatus(d));
    kn.run = 1; kn.bad = 0;
    ok(devStatus(d) === t('tx.busy') + ' · ' + t('cf.why.run', 'TRYX KANALI'), 'TRYX busy with KANALI running: ' + devStatus(d));
    S.ext.devs = devs.concat(d); updateConflicts();
    ok(row('tryx').textContent.includes(t('cf.dev.held')), 'the busy screen not under KANALI');
    S.cf = cf; S.ll = ll; S.ext.devs = devs; updateConflicts(); updateLL();
  });

  test('reconnect: waits grow and say so, Retry now starts over; a device given back to its own lighting is not retried', async () => {
    await fresh();
    const d = S.ext.devs.find(x => x.id === 10);
    Object.assign(d, { online: 0, fails: 4 }); buildDevices(); updateDevices();
    const card = () => $('#dev-st-10').closest('.card'), rt = () => card().querySelector('.dev-retry');
    ok($('#dev-st-10').textContent.includes(t('dev.waits')) && !rt().hidden, 'a device failing again and again: no Retry now or no word that it waits longer');
    d.fails = 1; updateDevices();
    ok(!$('#dev-st-10').textContent.includes(t('dev.waits')) && !rt().hidden, 'one failure already called a long wait');
    d.own = 1; updateDevices();
    ok(rt().hidden && !$('#dev-st-10').textContent.includes(t('dev.waits')), 'a device left to its own lighting offered a retry');
    d.own = 0; updateDevices();
    sent.length = 0; rt().click();
    ok(sent.some(o => o.cmd === 'dev_retry') && rt().disabled, 'Retry now not sent, or clickable again at once');
    await wait(150);
    ok(rt().hidden && S.ext.devs.find(x => x.id === 10).online, 'back online, yet Retry now still shown');
  });

  test('path: one device seen directly and through OpenRGB, the user picks which lights it, the other lets go; kept on restart', async () => {
    await fresh();
    STATE.ext.kinds.push({ kind: 'aura', title: 'ASUS Aura (board)', per_led: 1 });
    STATE.ext.devs.push({ id: 14, kind: 'aura', title: 'ASUS Aura (board)', name: 'ASUS board LEDs', host: 'USB', sub: 0, leds: 8, per_led: 1, online: 1, enabled: 1, type: 'board', info: 'ASUS Aura · motherboard · USB' });
    emit(STATE); await wait(80);
    const sel = id => $('#dev-st-' + id).closest('.card').querySelector('.dev-path'), st = id => $('#dev-st-' + id).textContent;
    const pick = async v => { sent.length = 0; sel(8).value = v; sel(8).dispatchEvent(new Event('change', { bubbles: true })); await wait(120); };
    ok(sel(8) && sel(8).value === '' && [...sel(8).options].some(o => o.value === 'orgb:14'), 'the board through OpenRGB not offered as the ASUS board');
    ok(!sel(7), 'Corsair memory offered as an ASUS device');
    await pick('orgb:14');
    const sets = sent.filter(o => o.cmd === 'set' && o.s === 'dev.8').map(o => o.k);
    ok(sets.indexOf('path') >= 0 && sets.indexOf('path') < sets.indexOf('same'), 'the pair sent before its path: ' + sets);
    ok(devById(14).held && !devById(8).held && st(14).includes(t('path.st.orgb', 'B550-F')), 'the direct one still lights it, or says nothing: ' + st(14));
    ok(!devWaits(devById(14)) && $('#dev-st-14').closest('.card').querySelector('.dev-retry').hidden, 'a device let go on purpose waits for a retry');
    ok(!sel(6) || ![...sel(6).options].some(o => o.value.endsWith(':14')), 'a device already paired offered to another card');
    // the path taken does not answer: the card says so, it does not claim it is lit
    devById(8).online = 0; updateDevices();
    ok(st(14).includes(t('dev.offline.ctl')), 'a failed handover shown as working: ' + st(14));
    // restart: the core reads the pair from the settings again
    STATE.ext.devs.forEach(d => { d.same = d.held = 0; }); window.chrome.webview.postMessage(JSON.stringify({ cmd: 'hello' })); await wait(120);
    ok(devById(14).held && sel(8).value === 'orgb:14', 'the choice lost on restart');
    await pick('direct:14');
    ok(devById(8).held && !devById(14).held && st(8).includes(t('path.st.direct', 'ASUS board LEDs')), 'OpenRGB still lights it: ' + st(8));
    await pick('');
    ok(!devById(8).held && !devById(14).held && !devById(14).same && sel(8).value === '', 'Not the same device left one of them let go');
    STATE.ext.devs.pop(); STATE.ext.kinds.pop(); delete STATE.cfg['dev.8']; emit(STATE); await wait(80);
  });

  test('OpenRGB: the card says what a controller takes, or why it is not lit; a connector of a board added whole says the board lights it', async () => {
    await fresh();
    const d = devById(2), info = d.info, st = id => $('#dev-st-' + id).textContent;
    d.cap = 1; updateDevices();
    ok(st(2).includes(t('drive.1')), 'haku through Direct not said: ' + st(2));
    d.cap = 3; updateDevices();
    ok(st(2).includes(t('drive.3')), 'steady colour only not said: ' + st(2));
    d.cap = 5; d.online = 0; updateDevices();
    ok(st(2) === t('cap.5'), 'a mode that did not switch not said: ' + st(2));
    d.cap = 4; d.info = 'OpenRGB · Keyboard · steady colour for the whole board only'; updateDevices();
    ok(st(2) === t('cap.4b'), 'a connector of a steady-colour board not told to add the board: ' + st(2));
    d.cap = 0; d.online = 1; d.info = info;
    STATE.ext.devs.push({ id: 15, kind: 'openrgb', title: 'OpenRGB', name: 'B550-F header 2', host: '127.0.0.1', sub: 2004, leds: 10, per_led: 1, online: 0, enabled: 1, covered: 8, type: 'strip', info: '' });
    emit(STATE); await wait(80);
    ok(st(15) === t('orgb.covered', 'B550-F') && !devWaits(devById(15)), 'a covered connector shown as not reachable: ' + st(15));
    STATE.ext.devs.pop(); emit(STATE); await wait(80);
  });

  test('about: the components list credits OpenRGB as a separate program, with its licence and sources', async () => {
    await fresh(); showTab('settings');
    const c = $('#comp'); c.open = true; await wait(30);
    const tx = $('#comp-list').textContent;
    ok(tx.includes(t('orgb.credit')) && tx.includes('GPL-2.0') && tx.includes('codeberg.org/OpenRGB/OpenRGB'), 'OpenRGB not credited: ' + tx.slice(0, 120));
    ok(['en', 'ru', 'fr'].every(l => STR[l]['orgb.credit'] && STR[l]['comp.orgb'] && STR[l]['path.note']), 'a language misses the new texts');
    c.open = false;
  });

  test('the offline list: the chip is a button, each name opens its device card, Esc goes back to the chip', async () => {
    await fresh();
    showTab('home'); updateChips();
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
    showTab('settings'); setCat('look');
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
    const F = STATE.fans; Object.assign(F, { link: 1, age: 400, rfail: 0, wd: [0, 0], log: [] });
    F.f.forEach(f => Object.assign(f, { em: '', own: 0, want: -1, out: -1, ok: 1, err: '' }));
    Object.keys(STATE.cfg.fans || {}).forEach(k => { STATE.cfg.fans[k] = ''; });
    if (SHEET.kind === 'fan') closeSheet();
    FAN.draft = null; FAN.open = null; FAN.fold = {};
    await fanEmit(); showTab('fans'); FAN.back = 0; FAN.sig = ''; updateFans();
  };
  const fanSheetOf = async i => { $(`#fan-panel [data-f="${i}"]`).click(); await wait(120); };

  test('fans: a card looks like its fan: picked in its settings, shared by a group, a pump its own; iCUE LINK and Lian Li on their own', async () => {
    await fresh(); await fanReset();
    const ic = i => $(`#fan-panel [data-f="${i}"] .fanic`), lit = el => el.querySelectorAll('.fi-lit > *').length, look = el => el.dataset.look;
    ok(ic(0) && look(ic(0)) === '', 'a plain fan by default (its picture)');
    ok(ic(0).classList.contains('on') && !ic(2).classList.contains('on'), 'lit and turning while it runs, still when stopped');
    await fanSheetOf(0);
    ok($$('#fs-look button').length === 6 && $('#fs-look [data-v=""]').classList.contains('on'), 'the picker');
    $('#fs-look [data-v="inf"]').click(); await wait(60);
    ok(STATE.cfg.fans.fan1_look === 'inf' && look(ic(0)) === 'inf' && $('#fs-look [data-v="inf"]').getAttribute('aria-pressed') === 'true', 'picked: ' + STATE.cfg.fans.fan1_look);
    $('#fs-kind [data-v="pump"]').click(); await wait(60);
    ok($('#fs-look-f').classList.contains('hidden') && ic(0).innerHTML.includes(PUMP_BLADE) && !ic(0).querySelector('.fi-bl'), 'a pump: its block and impeller, no picker');
    setCfg('fans', 'fan1_kind', ''); setCfg('fans', 'g1_name', 'Front'); setCfg('fans', 'g1', '1,2'); setCfg('fans', 'fan2_look', 'inf'); FAN.sig = ''; updateFans();
    ok(look($('#fan-panel [data-g="1"] .fanic')) === 'inf', 'a group of the same look shows it');
    setCfg('fans', 'fan2_look', 'sl'); FAN.sig = ''; updateFans();
    ok(look($('#fan-panel [data-g="1"] .fanic')) === '', 'a mixed group shows a plain one');
    updateLL(); const ll = $$('#ll-list .found-row');
    ok(ll.length === 2 && ll.every(r => r.querySelectorAll('.fi-lit rect').length === 2), 'Lian Li groups: lit edges');
    // lianli.c's give-up after 10 asks: the group says its board ARGB sync would not go off, or back on
    const g0 = S.ll.g[0], sm = () => $('#ll-list .found-row small').textContent;
    ok(!sm().includes(t('ll.sync.1')) && !sm().includes(t('ll.sync.2')), 'a group that synced as asked says nothing: ' + sm());
    g0.sync = 1; updateLL(); ok(sm().includes(t('ll.sync.1')), 'sync would not go off: ' + sm());
    g0.sync = 2; updateLL(); ok(sm().includes(t('ll.sync.2')) && !sm().includes(t('ll.sync.1')), 'sync would not go back on: ' + sm());
    delete g0.sync; updateLL(); ok(!sm().includes(t('ll.sync.2')), 'the note goes with it: ' + sm());
    await fanReset();
  });

  test('fans: a lost link keeps the page and says so; the readings show as old', async () => {
    await fresh(); await fanReset();
    STATE.fans.link = 0; STATE.fans.age = 12000; await fanEmit();
    ok(fansOn(), 'the tab went away');
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

  test('fans: a stalled control loop is said for as long as it lasts; channels that did not go back are a lasting problem', async () => {
    await fresh(); await fanReset();
    const m = $('#fans-back-msg');
    STATE.fans.wd = [1, 0]; await fanEmit();
    ok(m.textContent === t('fans.wd.back', 'QUADRO', 1) && m.classList.contains('warn') && !m.classList.contains('bad'), 'given back: ' + m.textContent);
    STATE.fans.wd = [3, 0]; STATE.fans.rfail = 2; await fanEmit(); await fanEmit();
    ok(m.textContent === t('fans.wd.fail', 'QUADRO', 2) && m.classList.contains('bad'), 'not given back, still said a round later: ' + m.textContent);
    STATE.fans.wd = [3, 2]; await fanEmit();
    ok(m.textContent === t('fans.wd.fail', 'QUADRO', 2), 'the worst one first: ' + m.textContent);
    STATE.fans.wd = [0, 2]; STATE.fans.rfail = 0; await fanEmit();
    ok(m.textContent === t('fans.wd.busy', t('fans.wd.board'), 1) && m.classList.contains('warn'), 'the board busy: ' + m.textContent);
    STATE.fans.wd = [0, 0]; await fanEmit();
    ok(m.textContent === '' && !m.classList.contains('warn') && !m.classList.contains('bad'), 'running again, nothing left: ' + m.textContent);
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

  test('fans: a fan joins a group in one change: its mode, source, curve and the membership together, its own limits and kind kept; Undo takes it all back', async () => {
    await fresh(); await fanReset();
    const cur = '30:30,60:70,80:100';
    setCfg('fans', 'g1_name', 'Front'); setCfg('fans', 'g1', '1'); setCfg('fans', 'g2_name', 'Back'); setCfg('fans', 'g2', '3,2');
    [['fan1', 'curve'], ['fan1_src', 't1'], ['fan1_curve', cur], ['fan2', 'fix'], ['fan2_pct', '40'], ['fan2_src', 'gpu'], ['fan2_min', '35'], ['fan2_kind', 'pump'], ['fan2_name', 'Rear']].forEach(([k, v]) => setCfg('fans', k, v));
    await fanEmit(); FAN.sig = ''; updateFans(); await wait(0); undoReset();
    $('#fan-panel [data-g="1"] .fan-hd').click(); await wait(120);
    const n = sent.length; $('#fs-members [data-m="1"]').click(); await wait(60);
    const out = sent.slice(n), fs = out.filter(o => o.cmd === 'fans_set'), kv = fs.length === 1 ? fs[0].kv.split('\n') : [];
    ok(fs.length === 1 && ['fan2=curve', 'fan2_src=t1', 'fan2_curve=' + cur, 'g1=1,2', 'g2=3'].every(x => kv.includes(x)), 'not one change: ' + JSON.stringify(fs));
    ok(!kv.some(x => /^fan2_(min|kind|name)=/.test(x)) && !out.some(o => o.cmd === 'set' && o.s === 'fans'), 'a key went alone, or its own ones were sent: ' + JSON.stringify(out));
    ok(STATE.cfg.fans.fan2 === 'curve' && STATE.cfg.fans.fan2_src === 't1' && STATE.cfg.fans.g1 === '1,2' && cv('fans', 'fan2_min', '') === '35' && cv('fans', 'fan2_kind', '') === 'pump', 'the core: ' + JSON.stringify(STATE.cfg.fans));
    const m = sent.length; undo(); await wait(60);
    const b = sent.slice(m).filter(o => o.cmd === 'fans_set'), bk = b.length === 1 ? b[0].kv.split('\n') : [];
    ok(b.length === 1 && ['fan2=fix', 'fan2_src=gpu', 'fan2_curve=', 'g1=1', 'g2=3,2'].every(x => bk.includes(x)), 'undo not one change: ' + JSON.stringify(b));
    ok(STATE.cfg.fans.fan2 === 'fix' && STATE.cfg.fans.fan2_src === 'gpu' && STATE.cfg.fans.g1 === '1' && STATE.cfg.fans.g2 === '3,2', 'undo left a mix: ' + JSON.stringify(STATE.cfg.fans));
    await fanReset();
  });

  test('fans: a Home tile says when its readings are old or a fan of its group is in trouble, and never turns on old readings', async () => {
    await fresh(); await fanReset();
    const F = STATE.fans, P = CanvasRenderingContext2D.prototype, fill = P.fillText;
    const drawn = () => { const out = []; P.fillText = function (s, ...a) { out.push(String(s)); return fill.call(this, s, ...a); }; try { drawHero(); } finally { P.fillText = fill; } out.includes = s => out.join(' ').includes(s); return out; };
    const label = k => { const h = HERO.hits.findIndex(h => h.key === k), b = $(`#hero-keys button[data-i="${h}"]:not([data-pw])`); return b ? b.getAttribute('aria-label') : null; };
    const spun = async k => { drawHero(); const a = (FAN_SPIN[k] || {}).a; await wait(60); drawHero(); return FAN_SPIN[k].a !== a; };
    const rpm = `${F.f[0].rpm} ${t('fans.rpmu')}`;
    setCfg('ui', 'hero_fans', 'fan0'); setCfg('ui', 'hero_order', 'fan0'); showTab('home'); await wait(30);
    ok(!fanTileSt([0]) && drawn().some(s => s.startsWith(rpm)) && label('fan0') === fanName(0) && await spun('fan0'), 'fresh: ' + label('fan0'));
    F.link = 0; await fanEmit();
    ok(drawn().includes(t('fans.tile.nolink')) && !drawn().some(s => s.startsWith(rpm)) && label('fan0') === fanName(0) + ', ' + t('fans.st.stale'), 'the link lost: ' + label('fan0'));
    ok(!await spun('fan0'), 'it turns on readings from before the link went');
    F.link = 1; F.age = 30000; await fanEmit();
    ok(drawn().includes(t('fans.tile.old')) && label('fan0').includes(t('fans.st.old')) && !await spun('fan0') && fanState(0)[1] === 'bad', 'old readings: ' + label('fan0'));
    F.age = 400; await fanEmit();
    ok(!fanTileSt([0]) && drawn().some(s => s.startsWith(rpm)) && label('fan0') === fanName(0) && await spun('fan0'), 'not back once the readings are: ' + label('fan0'));
    F.f[0].rpm = -1; await fanEmit();
    ok(drawn().some(s => s.startsWith('— ' + t('fans.rpmu'))), 'an unread rpm shown as a number');
    F.f[0].rpm = 1180;
    // a group: its second fan in trouble, the first one fine
    setCfg('fans', 'g1_name', 'Front'); setCfg('fans', 'g1', '1,2'); setCfg('fans', 'fan2', 'fix');
    Object.assign(F.f[1], { own: 1, want: 50, ok: -1 }); await fanEmit();
    setCfg('ui', 'hero_fans', 'fang1'); setCfg('ui', 'hero_order', 'fang1'); drawHero();
    ok(fanState(0)[1] === '' && drawn().includes(t('fans.tile.err')) && label('fang1').includes(fanName(1) + ': ' + t('fans.st.fail')), 'the error on the second fan: ' + label('fang1'));
    F.f[1].em = 'stale'; await fanEmit();
    ok(drawn().includes(t('fans.tile.em')) && label('fang1').includes(fanName(1) + ': ' + t('fans.em.stale')), 'its emergency: ' + label('fang1'));
    F.link = 0; await fanEmit();
    ok(drawn().includes(t('fans.tile.nolink')), 'no connection comes first');
    setCfg('ui', 'hero_fans', ''); setCfg('ui', 'hero_order', ''); await fanReset();
  });

  test('fans: a Home tile warns when a fan of it reads no rpm while driven (the second of a group too), worse states first, gone once rpm is back', async () => {
    await fresh(); await fanReset();
    const F = STATE.fans, keep = F.f.map(f => ({ rpm: f.rpm, pct: f.pct })), P = CanvasRenderingContext2D.prototype, fill = P.fillText;
    const drawn = () => { const out = []; P.fillText = function (s, ...a) { out.push(String(s)); return fill.call(this, s, ...a); }; try { drawHero(); } finally { P.fillText = fill; } out.includes = s => out.join(' ').includes(s); return out; };
    const label = k => { const h = HERO.hits.findIndex(h => h.key === k), b = $(`#hero-keys button[data-i="${h}"]:not([data-pw])`); return b ? b.getAttribute('aria-label') : null; };
    const samples = async n => { for (let k = 0; k < n; k++) await fanEmit(); };
    try {
      // a group: the first fan fine at 50%, the second driven at 50% and reading 0 rpm, several readings in a row
      setCfg('fans', 'g1_name', 'Front'); setCfg('fans', 'g1', '1,2'); setCfg('fans', 'fan1', 'fix'); setCfg('fans', 'fan2', 'fix');
      Object.assign(F.f[0], { own: 1, want: 50, ok: 1, pct: 50, rpm: 1100 }); Object.assign(F.f[1], { own: 1, want: 50, ok: 1, pct: 50, rpm: 0 });
      setCfg('ui', 'hero_fans', 'fang1'); setCfg('ui', 'hero_order', 'fang1'); showTab('home');
      await samples(1); drawHero();
      ok(!fanTileSt([0, 1]), 'one reading is not yet a warning');
      await samples(2); drawHero();
      ok(fanNoRpm(1) && !fanNoRpm(0) && fanState(1)[1] === '', 'the fan\'s own state: ' + fanNoRpm(1) + ' ' + fanState(1));
      ok(drawn().includes(t('fans.tile.norpm')), 'the tile shows no warning: ' + drawn().join(' | '));
      ok(label('fang1').includes(fanName(1) + ': ' + t('fans.norpm')), 'the name for a screen reader: ' + label('fang1'));
      ok(fanTileSt([0, 1])[2] === true, 'marked as worse than a warning');
      // worse states of the group come first: a control error on the first fan, then an emergency on the second
      F.f[0].ok = -1; await fanEmit(); drawHero();
      ok(drawn().includes(t('fans.tile.err')) && !drawn().includes(t('fans.tile.norpm')) && label('fang1').includes(fanName(0) + ': ' + t('fans.st.fail')) && !fanTileSt([0, 1])[2], 'the error first: ' + label('fang1'));
      F.f[0].ok = 1; F.f[1].em = 'stale'; await fanEmit(); drawHero();
      ok(drawn().includes(t('fans.tile.em')) && label('fang1').includes(fanName(1) + ': ' + t('fans.em.stale')), 'the emergency first: ' + label('fang1'));
      F.f[1].em = ''; F.link = 0; await fanEmit(); drawHero();
      ok(drawn().includes(t('fans.tile.nolink')), 'no connection first');
      F.link = 1; await samples(3); drawHero();
      ok(fanTileSt([0, 1])?.[0] === t('fans.tile.norpm'), 'the warning after the link is back: ' + fanTileSt([0, 1]));
      // the rpm back: gone at once
      F.f[1].rpm = 900; await fanEmit(); drawHero();
      ok(!fanTileSt([0, 1]) && !label('fang1').includes(t('fans.norpm')), 'still warned once rpm is back: ' + label('fang1'));
      ok(!drawn().includes(t('fans.tile.norpm')), 'still drawn once rpm is back');
      // “No speed signal” ticked: no warning
      F.f[1].rpm = 0; await samples(3); ok(fanTileSt([0, 1]), 'the warning not back');
      setCfg('fans', 'fan2_notach', '1'); drawHero();
      ok(!fanTileSt([0, 1]) && !drawn().includes(t('fans.tile.norpm')), 'warned with “No speed signal” ticked');
      // a fan on its own tile
      setCfg('fans', 'fan2_notach', ''); setCfg('fans', 'g1', ''); setCfg('fans', 'g1_name', '');
      setCfg('ui', 'hero_fans', 'fan1'); setCfg('ui', 'hero_order', 'fan1'); await fanEmit(); drawHero();
      ok(drawn().includes(t('fans.tile.norpm')) && label('fan1') === fanName(1) + ', ' + t('fans.norpm'), 'a fan alone: ' + label('fan1'));
      F.f[1].rpm = 900; await fanEmit(); drawHero();
      ok(!drawn().includes(t('fans.tile.norpm')) && label('fan1') === fanName(1), 'a fan alone, rpm back: ' + label('fan1'));
    } finally {
      F.f.forEach((f, i) => Object.assign(f, keep[i])); setCfg('ui', 'hero_fans', ''); setCfg('ui', 'hero_order', ''); await fanReset();
    }
  });

  test('fans: Cooling is there on a PC, empty until a fan controller or a Lian Li receiver alone is found, and never on a phone', async () => {
    await fresh(); await fanReset();
    const F = STATE.fans, L = STATE.ll, keep = { on: F.on, q: F.q, st: L.st }, vis = s => !$(s).classList.contains('hidden') && !!$(s).offsetParent;
    // [the QUADRO or the board found, the QUADRO, the board chip, Lian Li state] -> tab
    for (const [on, q, chip, st, want] of [[0, 0, '', 'none', 0], [0, 0, '', 'ok', 1], [0, 0, '', 'err', 1], [0, 0, '', 'busy', 1], [1, 1, '', 'none', 1], [1, 0, 'NCT6799D', 'none', 1], [1, 1, 'NCT6799D', 'ok', 1]]) {
      Object.assign(F, { on, q }); if (chip) F.chip = chip; else delete F.chip; L.st = st; await fanEmit();
      const c = JSON.stringify([on, q, chip, st]);
      ok(fansOn() === !!want, 'the tab: ' + c);
      showTab('fans'); await wait(30);
      ok(tab === 'fans' && $('#nav [data-tab="fans"]').offsetParent && vis('#fans-none') === !want, 'Cooling, or its empty state: ' + c);
      if (want) {
        ok(vis('#ll-card') === (st !== 'none') && vis('#fan-main') === !!on && vis('#tab-fans .fans-top') === !!on, 'the page: ' + c);
        if (!on) ok($('#ll-status').textContent === (st === 'ok' ? t('ll.st.ok', L.g.length) : t('ll.st.' + st, L.err || 0) + (st === 'busy' ? cfWhy('lianli') : '')), 'the receiver: ' + $('#ll-status').textContent);
      }
    }
    document.documentElement.classList.add('remote'); showTab('fans');
    ok(!fansOn() && tab === 'home' && !$('#nav [data-tab="fans"]').offsetParent, 'a phone got the fans: ' + tab); document.documentElement.classList.remove('remote');
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
    // a still picture: drawn again only on a change (the loop driven by hand: a background page gets no rAF)
    txbLoop(); let d0 = TXB.draws; txbLoop(); txbLoop(); ok(TXB.draws === d0, 'a still, unchanged: not drawn again: ' + (TXB.draws - d0));
    ok(!TXB.raf, 'a still: the loop runs on with nothing to draw');
    txbSel(-1); ok(TXB.raf, 'a change of the chosen layer asks for no frame'); txbLoop(); ok(!TXB.raf && TXB.draws === d0 + 1, 'the choice: drawn once, then stopped'); d0 = TXB.draws;
    txbSel(0); txbLoop(); d0 = TXB.draws;
    txbMove(-.1, 0); txbLoop(); txbLoop(); ok(TXB.draws === d0 + 1, 'moved: drawn once: ' + (TXB.draws - d0));
    ti.value = 'GPU'; ti.dispatchEvent(new Event('input', { bubbles: true })); txbLoop(); ok(TXB.draws === d0 + 2, 'new text: drawn: ' + (TXB.draws - d0));
    document.body.classList.add('idle'); await wait(20); ti.value = 'CPU'; ti.dispatchEvent(new Event('input', { bubbles: true })); txbMove(.1, 0); txbLoop();
    ok(TXB.draws === d0 + 2, 'behind other windows: no frames: ' + (TXB.draws - d0));
    document.body.classList.remove('idle'); await wait(20); ok(TXB.draws === d0 + 3, 'back in front: drawn again: ' + (TXB.draws - d0));
    sent.length = 0; await txbMake();
    const o = sent.find(x => x.cmd === 'tryx_build');
    ok(o && o.id === '14' && o.ext === 'jpg' && o.data.startsWith('/9j/'), 'sent: ' + JSON.stringify(o && { ...o, data: o.data.slice(0, 8) }));
    ok(!$('#txb').classList.contains('hidden') && TXB?.src && TXB.layers.length === 1 && $('#txb-msg').textContent === t('txb.saved') && !$('#txb-go').disabled,
      'after the save the builder says saved on this PC and keeps the work: ' + $('#txb-msg').textContent);
    txbClose(); await fanEmit();
    ok(card().querySelector('.dev-media-name').textContent === 'tryx-14.jpg', 'the file: ' + card().querySelector('.dev-media-name').textContent);
    STATE.ext.devs.pop(); delete STATE.cfg['dev.14']; await fanEmit();
  });

  test('TRYX: the builder makes an mp4 of an animation that the browser itself plays, at the screen size', async () => {
    await fresh();
    STATE.ext.devs.push({ id: 14, kind: 'tryx', title: 'TRYX PANORAMA', name: 'TRYX PANORAMA', host: '18D1:2D04', sub: 0, leds: 1, per_led: 0, online: 1, enabled: 1, type: 'screen', info: 'TRYX PANORAMA · COM5 · USB' });
    STATE.cfg['dev.14'] = { kind: 'tryx' }; await fanEmit();
    $('#dev-st-14').closest('.card').querySelector('.dev-build').click();
    const frames = [];
    for (const [i, col] of ['#c33', '#3c3', '#33c'].entries()) {
      const c = document.createElement('canvas'); c.width = 200; c.height = 100; const x = c.getContext('2d'); x.fillStyle = col; x.fillRect(0, 0, 200, 100);
      frames.push({ bmp: await createImageBitmap(c), at: i * .2, dur: .2 });
    }
    TXB.src = { frames, w: 200, h: 100, dur: .6 };
    sent.length = 0; await txbMake();
    const o = sent.find(x => x.cmd === 'tryx_build');
    ok(o && o.ext === 'mp4' && $('#txb-msg').textContent === t('txb.saved'), 'sent: ' + JSON.stringify(o && { ...o, data: o.data.slice(0, 8) }) + ' / ' + $('#txb-msg').textContent);
    const v = document.createElement('video'); v.muted = true; v.src = 'data:video/mp4;base64,' + o.data;
    const err = await new Promise(r => { v.onloadeddata = () => r(''); v.onerror = () => r('error ' + v.error?.message); });
    ok(!err && v.videoWidth === 1440 && v.videoHeight === 720 && Math.abs(v.duration - .6) < .15, `the mp4: ${err} ${v.videoWidth}x${v.videoHeight} ${v.duration} s`);
    txbClose(); STATE.ext.devs.pop(); delete STATE.cfg['dev.14']; await fanEmit();
  });

  // a Corsair LCD (drv_corsairlcd.c): its pack "HKLCD1\0\0" + [u32 length, u16 ms, JPEG]..., read back as the driver does
  const clDev = info => ({ id: 15, kind: 'corsairlcd', title: 'Corsair LCD', name: 'Corsair iCUE LINK LCD', host: '1B1C:0C4E', sub: 0, leds: 1, per_led: 0, online: /busy$/.test(info) ? 0 : 1, enabled: 1, type: 'screen', info });
  const clFrames = b64 => {
    const b = Uint8Array.from(atob(b64), c => c.charCodeAt(0)), v = new DataView(b.buffer), out = [];
    if (new TextDecoder().decode(b.slice(0, 8)) !== 'HKLCD1\0\0') return null;
    for (let o = 8; o + 6 <= b.length;) { const n = v.getUint32(o, true); out.push({ ms: v.getUint16(o + 4, true), jpg: b.slice(o + 6, o + 6 + n) }); o += 6 + n; }
    return out;
  };
  const jpgSegs = j => {   // the markers up to SOS: [{m: 'c0', b: the segment's bytes}]
    const r = [{ m: 'd8' }]; let p = 2;
    while (p < j.length && j[p] === 0xff) { const m = j[p + 1], n = j[p + 2] << 8 | j[p + 3]; r.push({ m: m.toString(16), b: j.slice(p + 4, p + 2 + n) }); if (m === 0xda) break; p += 2 + n; }
    return r;
  };
  const clPixel = async (jpg, x, y) => { const c = document.createElement('canvas'); c.width = c.height = 480; const g = c.getContext('2d'); g.drawImage(await createImageBitmap(new Blob([jpg], { type: 'image/jpeg' })), 0, 0); return [...g.getImageData(x, y, 1, 1).data.slice(0, 3)]; };

  test('the check level (README facts): on the card and in Add before anything is added; one model checked is not the line; a kind with no fact has none', async () => {
    await fresh();
    STATE.ext.devs.push({ ...clDev(''), id: 16, kind: 'wooting', title: 'Wooting', name: 'Wooting 80HE', host: '31E3:1402', type: 'keyboard', leds: 80, per_led: 1 },
      { ...clDev(''), id: 17, kind: 'aura', title: 'ASUS Aura (board)', name: 'ROG logo', host: '0B05:19AF', type: 'board', leds: 3, per_led: 1 });
    await fanEmit();
    const chk = id => $('#dev-st-' + id)?.parentElement.querySelector('.dev-chk');   // the card's head: its name, state and kind
    ok(chk(16)?.textContent === t('chk.hw', 'Wooting 60HE v2') && /60HE v2/.test(t('chk.hw', 'Wooting 60HE v2')), 'Wooting: ' + chk(16)?.textContent);
    ok($('#dev-st-17') && !chk(17), 'a kind the README has no fact for got a level: ' + !!$('#dev-st-17') + ' [' + chk(17)?.outerHTML + ']');
    const ks = $('#man-kind');
    for (const [k, want] of [['wled', t('chk.sim')], ['nanoleaf', t('chk.hw', 'Nanoleaf Blocks')]]) {
      ks.value = k; manualHints(); ok(ks.value === k && $('#man-note').textContent.endsWith(want), k + ' in Add: ' + $('#man-note').textContent);
    }
    for (const L of ['ru', 'fr']) for (const key of ['chk.hw', 'chk.sim', 'chk.none']) ok(STR[L][key] && STR[L][key] !== STR.en[key], L + ' ' + key + ' missing');
    STATE.ext.devs = STATE.ext.devs.filter(d => d.id !== 16 && d.id !== 17); await fanEmit();   // the tests after it add their own 16
  });

  test('frames and sending (Help, Diagnostics): the core\'s measurements, a send rate from two readings, nothing sent', async () => {
    await fresh();
    const P0 = STATE.perf, d = STATE.ext.devs[0], li = () => [...$$('#perf-list li')].map(l => l.textContent);
    STATE.perf = { fps10: 0, chg10: 0, avg_us: 0, max_us: 0, want: 30 }; await fanEmit();
    ok(li()[0] === t('perf.wait'), 'before the first window: ' + li()[0]);
    STATE.perf = { fps10: 298, chg10: 51, avg_us: 1250, max_us: 14200, want: 30 };
    Object.assign(d, { ns: 1000, nf: 0 }); PERF.at = -Infinity; PERF.ns = {}; await fanEmit();
    ok(li()[0] === t('perf.loop', '29.8', 30, '5.1', '1.3', '14'), 'the loop line: ' + li()[0]);
    ok(li().includes(t('perf.dev.wait', d.name)), 'one reading is no rate yet: ' + li().join(' | '));
    Object.assign(d, { ns: 1060, nf: 2 }); PERF.at = performance.now() - 2000; await fanEmit();
    const row = li().find(s => s.startsWith(d.name + ':')), r = +(row.match(/([\d.]+)/) || [])[1];
    ok(r > 27 && r <= 30 && row === t('perf.dev', d.name, r.toFixed(1), 2), '60 frames over ~2 s: ' + row);
    const off = STATE.ext.devs.find(x => x.enabled && !x.online && !x.own && !x.held && !x.covered);
    ok(!off || li().includes(t('perf.dev.off', off.name || off.title, off.fails || 0)), 'a device not connected: ' + li().join(' | '));
    ok(STATE.ext.devs.filter(x => x.own || x.held || x.covered || !x.enabled).every(x => !li().some(s => s.startsWith((x.name || x.title) + ':'))), 'a device haku does not send to has a line');
    ok(!sent.some(o => o.cmd !== 'get'), 'showing the measurements sent: ' + JSON.stringify(sent.filter(o => o.cmd !== 'get')));
    for (const L of ['ru', 'fr']) for (const key of ['perf.title', 'perf.text', 'perf.loop', 'perf.wait', 'perf.dev', 'perf.dev.wait', 'perf.dev.off']) ok(STR[L][key] && STR[L][key] !== STR.en[key], L + ' ' + key + ' missing');
    STATE.perf = P0; delete d.ns; delete d.nf; await fanEmit();
  });

  test('check for problems (Help): hidden until asked, a row per kind with its reason and action, nothing sent or changed', async () => {
    await fresh();
    CHK.on = false; $('#dchk-list').classList.add('hidden'); await fanEmit();
    ok($('#dchk-list').classList.contains('hidden'), 'the list shows before Check is pressed');
    $('#dchk-run').click();
    const txt = () => $('#dchk-list').textContent.replace(/\s+/g, ' ');
    ok(!$('#dchk-list').classList.contains('hidden') && txt().includes('Bedside' + t('dev.offline') + t('dchk.off.net')), 'the mock\'s lamp not reachable: ' + txt());
    const D0 = STATE.ext.devs.map(d => ({ ...d })), B0 = STATE.bulbs.map(b => ({ ...b })), P0 = STATE.prof, W0 = STATE.fans.wd;
    try {
    STATE.ext.devs.forEach(d => d.online = 1); STATE.bulbs.forEach(b => b.online = 1); STATE.prof = { err: 0, id: 0, n: 0 }; STATE.fans.wd = [0, 0]; await fanEmit();
    ok(txt().trim() === t('dchk.none'), 'all reachable: ' + txt());
    const dev = id => STATE.ext.devs.find(d => d.id === id), kb = dev(2), gpu = dev(6), ram = dev(7), mouse = dev(10);
    STATE.ext.devs.push({ ...clDev('Corsair iCUE LINK LCD · screen · USB'), mx: 1 }); STATE.cfg['dev.15'] = { kind: 'corsairlcd', media: 'C:\\pics\\cat.hlcd' };
    STATE.ext.kinds.push({ kind: 'corsairlcd', title: 'Corsair LCD', per_led: 0 });   // drv_corsairlcd.c's title: its first word pairs it with OpenRGB's Corsair devices
    kb.online = 0; gpu.cap = 6; Object.assign(mouse, { cap: 4, online: 0 });
    STATE.prof = { err: 1, id: 2, n: 1 }; STATE.fans.wd = [3, 0];
    sent.length = 0; await fanEmit();
    const rows = [...$$('#dchk-list .found-row')].map(r => ({ k: r.querySelector('.kind').textContent, s: r.querySelector('.what').textContent, a: r.querySelector('[data-a]').dataset.a }));
    const has = (c, why, todo, a) => rows.some(r => r.k === t('dchk.c.' + c) && r.s.includes(why) && r.s.includes(todo) && r.a === a);
    const all = rows.map(r => r.k + ' ' + r.s + ' [' + r.a + ']').join(' | ');
    ok(has('media', t('dchk.media', 'cat.hlcd'), t('dchk.media.do'), 'open'), 'a screen\'s file gone: ' + all);
    ok(has('off', kb.name, t('dchk.off.orgb'), 'open'), 'an OpenRGB device not reachable: ' + all);
    ok(has('two', t('cap.6'), t('dchk.cap6.do'), 'open'), 'one that is the same device as another: ' + all);
    ok(has('two', t('dchk.pair', 'Corsair iCUE LINK LCD'), t('dchk.pair.do'), 'open') && rows.some(r => r.s.startsWith(ram.name)), 'the path question never answered: ' + all);
    ok(has('cap', mouse.name, t('dchk.cap4.do'), 'open') && !rows.some(r => r.k === t('dchk.c.off') && r.s.startsWith(mouse.name)), 'not supported, not told as unreachable too: ' + all);
    ok(has('left', t('prof.label'), t('dchk.prof.do'), 'prof'), 'a profile not saved: ' + all);
    ok(has('left', t('fans.wd.fail', 'QUADRO', 1), t('dchk.wd.do'), 'log'), 'fans not given back: ' + all);
    STATE.cfg['dev.7'] = { same: '' }; await fanEmit();
    ok(!$('#dchk-list').textContent.includes(t('dchk.pair.do')), 'None answered, still asked');
    ok(!sent.some(o => o.cmd !== 'get'), 'the check sent: ' + JSON.stringify(sent.filter(o => o.cmd !== 'get')));
    $('#dchk-list [data-a="log"]').click(); ok(sent.filter(o => o.cmd !== 'get').length === 1 && sent.at(-1).cmd === 'open' && sent.at(-1).what === 'log', 'the log button: ' + JSON.stringify(sent.at(-1)));
    for (const L of ['ru', 'fr']) for (const key of Object.keys(STR.en).filter(k => k.startsWith('dchk.'))) ok(STR[L][key] && STR[L][key] !== STR.en[key], L + ' ' + key + ' missing');
    } finally {   // a failed check above must not leave its devices to the tests after it
    STATE.ext.devs.splice(0, Infinity, ...D0); STATE.bulbs.splice(0, Infinity, ...B0); STATE.ext.kinds = STATE.ext.kinds.filter(k => k.kind !== 'corsairlcd');
    delete STATE.cfg['dev.15']; delete STATE.cfg['dev.7']; STATE.prof = P0; STATE.fans.wd = W0; CHK.on = false; $('#dchk-list').classList.add('hidden'); await fanEmit();
    }
  });

  test('Corsair LCD: a screen card with the builder and the follow switch, no theme; the program holding it is named', async () => {
    await fresh();
    STATE.ext.devs.push(clDev('Corsair iCUE LINK LCD · screen · USB')); STATE.cfg['dev.15'] = { kind: 'corsairlcd' }; await fanEmit();
    const card = () => $('#dev-st-15').closest('.card');
    ok(card().querySelector('.dev-build') && card().querySelector('.dev-media') && card().querySelector('.dev-follow')?.checked && !card().querySelector('.dev-theme'), 'the card: ' + card().innerHTML.slice(0, 200));
    ok(!card().querySelector('.live, .zone, .dev-type, .dev-cal, .dev-ident') && t('cl.note') !== 'cl.note' && card().textContent.includes(t('cl.note')), 'the screen card has light settings or no note');
    ok(card().querySelector('.dev-chk')?.textContent === t('chk.none'), 'the check level on the card: ' + card().querySelector('.dev-chk')?.textContent);
    ok(gdWhyNot(STATE.ext.devs.at(-1)) === t('gd.t.screen'), 'the test colour offered for a screen');
    card().querySelector('.dev-follow').click(); ok(sets('dev.15', 'screen_follow').some(o => +o.v === 0), 'the switch did not save: ' + JSON.stringify(sent));
    STATE.ext.devs[STATE.ext.devs.length - 1] = clDev('iCUE.exe busy'); await fanEmit();
    ok($('#dev-st-15').textContent === t('cl.busy', 'iCUE'), 'busy: ' + $('#dev-st-15').textContent);
    ok(cfDev({ id: 'icue', run: 1 }) === 'wait' && cfDev({ id: 'icue', run: 0 }) !== 'wait', 'iCUE running not shown as making the screen wait');
    STATE.ext.devs.pop(); delete STATE.cfg['dev.15']; await fanEmit();
  });

  test('Corsair LCD: the builder makes a round 480x480 pack, black first, and hands it to the core; a TRYX after it has its shapes back', async () => {
    await fresh();
    STATE.ext.devs.push(clDev('Corsair iCUE LINK LCD · screen · USB')); STATE.cfg['dev.15'] = { kind: 'corsairlcd' }; await fanEmit();
    $('#dev-st-15').closest('.card').querySelector('.dev-build').click();
    ok(!$('#txb').classList.contains('hidden') && TXB.lcd && TXB.shape === 'round' && $('#txb-shape').closest('label').classList.contains('hidden'), 'the builder did not open round');
    const note = () => $('#txb [data-i18n^="txb.note"]').textContent;
    ok(t('txb.note.lcd') !== 'txb.note.lcd' && note() === t('txb.note.lcd'), 'the hint: ' + note());
    const c = document.createElement('canvas'); c.width = c.height = 400; c.getContext('2d').fillStyle = '#c33'; c.getContext('2d').fillRect(0, 0, 400, 400);
    await txbLoad(new File([await new Promise(r => c.toBlob(r, 'image/png'))], 'red.png', { type: 'image/png' }));
    sent.length = 0; await txbMake();
    const o = sent.find(x => x.cmd === 'tryx_build'), f = o && clFrames(o.data);
    ok(o && o.id === '15' && o.ext === 'hlcd' && f && f.length === 2 && f[0].ms === 1000, 'sent: ' + JSON.stringify(o && { ...o, data: o.data.slice(0, 12) }) + ' ' + JSON.stringify(f?.map(x => x.ms)));
    const black = await clPixel(f[0].jpg, 240, 240), mid = await clPixel(f[1].jpg, 240, 240), edge = await clPixel(f[1].jpg, 4, 4);
    ok(black.every(v => v < 16) && mid[0] > 160 && mid[1] < 90 && edge.every(v => v < 16), `frames: black ${black}, the middle ${mid}, the corner ${edge}`);
    ok(f.every(x => jpgSegs(x.jpg).map(s => s.m).join() === 'd8,db,c0,c4,da'), 'the pack\'s JPEGs are not the baseline kind: ' + f.map(x => jpgSegs(x.jpg).map(s => s.m)).join(' | '));
    ok($('#txb-msg').textContent === t('txb.saved.lcd'), 'the message: ' + $('#txb-msg').textContent);
    txbClose(); await fanEmit();
    ok($('#dev-st-15').closest('.card').querySelector('.dev-media-name').textContent === 'corsairlcd-15.hlcd' && !STATE.cfg['dev.15'].theme, 'the file: ' + JSON.stringify(STATE.cfg['dev.15']));
    STATE.ext.devs.pop(); delete STATE.cfg['dev.15'];
    STATE.ext.devs.push({ id: 14, kind: 'tryx', title: 'TRYX PANORAMA', name: 'TRYX PANORAMA', host: '18D1:2D04', sub: 0, leds: 1, per_led: 0, online: 1, enabled: 1, type: 'screen', info: 'TRYX PANORAMA · COM5 · USB' });
    STATE.cfg['dev.14'] = { kind: 'tryx' }; await fanEmit();
    $('#dev-st-14').closest('.card').querySelector('.dev-build').click();
    ok(!TXB.lcd && TXB.shape === '2:1' && $('#txb-shape').value === '2:1' && !$('#txb-shape').closest('label').classList.contains('hidden') && note() === t('txb.note'), 'the TRYX builder after the LCD: ' + note());
    txbClose(); STATE.ext.devs.pop(); delete STATE.cfg['dev.14']; await fanEmit();
  });

  test('Corsair LCD: the JPEG is Go-like baseline (no APPn, 4:2:0, Annex K Huffman tables) and decodes to the picture', async () => {
    const w = 480, h = 480, img = new ImageData(w, h);
    for (let i = 0; i < w * h; i++) img.data.set([(i % w) >> 1, (i / w | 0) >> 1, 200, 255], i * 4);
    const j = txbJpeg(img, 85), segs = jpgSegs(j), m = segs.map(s => s.m).join();
    ok(m === 'd8,db,c0,c4,da' && j[j.length - 2] === 0xff && j[j.length - 1] === 0xd9, 'markers: ' + m);
    const sof = segs.find(s => s.m === 'c0').b, dht = segs.find(s => s.m === 'c4').b;
    ok(sof.length === 15 && sof[0] === 8 && (sof[1] << 8 | sof[2]) === h && (sof[3] << 8 | sof[4]) === w && [sof[7], sof[10], sof[13]].join() === '34,17,17', 'SOF0: ' + [...sof]);
    const tabs = []; for (let p = 0; p < dht.length;) { const cnt = [...dht.slice(p + 1, p + 17)], n = cnt.reduce((a, b) => a + b); tabs.push({ tc: dht[p], cnt, vals: [...dht.slice(p + 17, p + 17 + n)] }); p += 17 + n; }
    const code = (tb, v) => { let c = 0, k = 0; for (let l = 1; l <= 16; l++, c <<= 1) for (let i = 0; i < tb.cnt[l - 1]; i++, c++) if (tb.vals[k++] === v) return c.toString(2).padStart(l, '0'); };
    const ac = new Set([0, 0xf0]); for (let r = 0; r < 16; r++) for (let s = 1; s <= 10; s++) ac.add(r << 4 | s);
    ok(tabs.map(x => x.tc + ':' + x.vals.length).join() === '0:12,16:162,1:12,17:162' && [tabs[1], tabs[3]].every(x => x.vals.every(v => ac.has(v)) && new Set(x.vals).size === ac.size),
      'DHT: ' + tabs.map(x => x.tc + ':' + x.vals.length));
    // the code words T.81 Annex K gives: EOB and ZRL for luma and chroma, the DC size 0
    ok([code(tabs[1], 0), code(tabs[1], 0xf0), code(tabs[3], 0), code(tabs[3], 0xf0), code(tabs[0], 0), code(tabs[2], 0)].join() === '1010,11111111001,00,1111111010,00,00', 'standard codes');
    const px = await Promise.all([[0, 0], [100, 300], [479, 479]].map(([x, y]) => clPixel(j, x, y)));
    ok(px.every((p, i) => { const [x, y] = [[0, 0], [100, 300], [479, 479]][i]; return Math.abs(p[0] - (x >> 1)) < 6 && Math.abs(p[1] - (y >> 1)) < 6 && Math.abs(p[2] - 200) < 6; }), 'decoded: ' + JSON.stringify(px));
  });

  test('Corsair LCD: an animation goes frame by frame at 25 a second, each with its time, black first', async () => {
    await fresh();
    STATE.ext.devs.push(clDev('Corsair iCUE LINK LCD · screen · USB')); STATE.cfg['dev.15'] = { kind: 'corsairlcd' }; await fanEmit();
    $('#dev-st-15').closest('.card').querySelector('.dev-build').click();
    const frames = [];
    for (const [i, col] of ['#c33', '#3c3', '#33c'].entries()) {
      const c = document.createElement('canvas'); c.width = c.height = 100; const x = c.getContext('2d'); x.fillStyle = col; x.fillRect(0, 0, 100, 100);
      frames.push({ bmp: await createImageBitmap(c), at: i * .2, dur: .2 });
    }
    TXB.src = { frames, w: 100, h: 100, dur: .6 };
    sent.length = 0; await txbMake();
    const o = sent.find(x => x.cmd === 'tryx_build'), f = o && clFrames(o.data);
    ok(f && f.length === 16 && f[0].ms === 1000 && f.slice(1).every(x => x.ms === 40), 'frames: ' + JSON.stringify(f?.map(x => x.ms)));
    const first = await clPixel(f[1].jpg, 240, 240), last = await clPixel(f[15].jpg, 240, 240);
    ok(first[0] > 160 && last[2] > 160, `the animation: first ${first}, last ${last}`);
    txbClose(); STATE.ext.devs.pop(); delete STATE.cfg['dev.15']; await fanEmit();
  });

  // the builder open on a TRYX, and a stand-in ImageDecoder: a file's text names its plan { n, w, h, fail, hold }, frame i
  // waiting for hold[i]; it counts the decoders and frames it made and closed, and keeps every bitmap made meanwhile
  const txbOn = async () => {
    STATE.ext.devs.push({ id: 14, kind: 'tryx', title: 'TRYX PANORAMA', name: 'TRYX PANORAMA', host: '18D1:2D04', sub: 0, leds: 1, per_led: 0, online: 1, enabled: 1, type: 'screen', info: 'TRYX PANORAMA · COM5 · USB' });
    STATE.cfg['dev.14'] = { kind: 'tryx', media: 'old.jpg' }; await fanEmit();
    $('#dev-st-14').closest('.card').querySelector('.dev-build').click();
  };
  const txbOff = async () => { if (TXB) { TXB.busy = 0; txbClose(); } STATE.ext.devs.pop(); delete STATE.cfg['dev.14']; await fanEmit(); };
  const fakeDecoder = plans => {
    const real = { D: window.ImageDecoder, cib: window.createImageBitmap }, log = { dec: 0, decClosed: 0, imgs: 0, imgClosed: 0, bmps: [] };
    window.ImageDecoder = class {
      static isTypeSupported() { return Promise.resolve(true); }
      constructor({ data }) { this.p = plans[new TextDecoder().decode(data)]; log.dec++; this.tracks = { ready: Promise.resolve(), selectedTrack: { frameCount: this.p.n } }; }
      async decode({ frameIndex: i }) {
        await this.p.hold?.[i];
        if (i === this.p.fail) throw new Error('a broken frame');
        const c = document.createElement('canvas'); c.width = this.p.w; c.height = this.p.h; log.imgs++;
        return { image: Object.assign(c, { duration: 1e5, displayWidth: c.width, displayHeight: c.height, close() { log.imgClosed++; } }) };
      }
      close() { log.decClosed++; }
    };
    window.createImageBitmap = async (...a) => { const b = await real.cib.call(window, ...a); log.bmps.push(b); return b; };
    log.restore = () => { window.ImageDecoder = real.D; window.createImageBitmap = real.cib; };
    log.tidy = () => log.dec === log.decClosed && log.imgs === log.imgClosed;
    return log;
  };
  const gif = k => new File([k], k + '.gif', { type: 'image/gif' });
  const gate = () => { let go; const p = new Promise(r => { go = r; }); p.go = go; return p; };
  const shut = b => b.width === 0;   // a closed ImageBitmap has no size

  test('TRYX: a failed save on the PC keeps the old file, the settings and the work, and the same work saves on a retry', async () => {
    await fresh(); await txbOn();
    try {
      $('#txb-text').click();
      const c = document.createElement('canvas'); c.width = 300; c.height = 150;
      await txbLoad(new File([await new Promise(r => c.toBlob(r, 'image/png'))], 'a.png', { type: 'image/png' }));
      const src = TXB.src;
      for (const x of ['open', 'write', 'replace']) {
        window.mockTryxFail = x; sent.length = 0;
        const p = txbMake(); await wait(20); emit({ type: 'tryx_built', req: 'not-this-one', ok: 1, err: '' }); await p;
        ok(sent.some(o => o.cmd === 'tryx_build' && o.req), 'no request id: ' + JSON.stringify(sent.map(o => o.cmd)));
        ok($('#txb-msg').textContent === t('txb.nosave', t('txb.e.' + x)) && $('#txb-msg').classList.contains('warn'), x + ': ' + $('#txb-msg').textContent);
        ok(!$('#txb').classList.contains('hidden') && TXB.src === src && !shut(src.bmp) && TXB.layers.length === 1 && !$('#txb-go').disabled, x + ': the work was lost');
        ok(STATE.cfg['dev.14'].media === 'old.jpg' && STATE.cfg['dev.14'].theme === undefined, x + ': the settings changed: ' + JSON.stringify(STATE.cfg['dev.14']));
      }
      window.mockTryxFail = null; await txbMake();
      ok($('#txb-msg').textContent === t('txb.saved') && !$('#txb-msg').classList.contains('warn') && /tryx-14\.jpg$/.test(STATE.cfg['dev.14'].media) && TXB.src === src, 'the retry: ' + $('#txb-msg').textContent + ' ' + STATE.cfg['dev.14'].media);
    } finally { window.mockTryxFail = null; await txbOff(); }
  });

  test('TRYX: of two picks the newer one wins whichever ends first, the other frees what it made, and nothing saves meanwhile', async () => {
    await fresh(); await txbOn();
    const A = { n: 3, w: 200, h: 100, hold: { 1: gate() } }, B = { n: 2, w: 120, h: 60 }, log = fakeDecoder({ A, B });
    try {
      const a = txbLoad(gif('A')); await wait(30);
      ok($('#txb-go').disabled && $('#txb-msg').textContent === t('txb.loading'), 'no loading state');
      sent.length = 0; await txbMake(); ok(!sent.some(o => o.cmd === 'tryx_build'), 'it saved while loading');
      await txbLoad(gif('B')); const bBmps = TXB.src.frames.map(f => f.bmp);
      ok(TXB.src.w === 120 && TXB.src.frames.length === 2 && !$('#txb-go').disabled && $('#txb-msg').textContent === t('txb.anim', .2), 'B: ' + $('#txb-msg').textContent);
      A.hold[1].go(); await a;
      ok(TXB.src.w === 120 && bBmps.every(b => !shut(b)) && $('#txb-msg').textContent === t('txb.anim', .2), 'A, ending last, took over: ' + $('#txb-msg').textContent);
      ok(log.bmps.length >= 3 && log.bmps.filter(b => !bBmps.includes(b)).every(shut) && log.tidy(), 'A left something open: ' + JSON.stringify(log));
      ok(!$('#txb-go').disabled, 'saving stayed off');
    } finally { log.restore(); await txbOff(); }
  });

  test('TRYX: a pick that breaks after a newer one loaded leaves the newer one as it is', async () => {
    await fresh(); await txbOn();
    const A = { n: 4, w: 200, h: 100, fail: 2, hold: { 1: gate() } }, B = { n: 2, w: 120, h: 60 }, log = fakeDecoder({ A, B });
    try {
      const a = txbLoad(gif('A')); await wait(30);
      await txbLoad(gif('B')); const src = TXB.src;
      A.hold[1].go(); await a;
      ok(TXB.src === src && src.frames.every(f => !shut(f.bmp)) && $('#txb-msg').textContent === t('txb.anim', .2) && !$('#txb-go').disabled, 'B was touched: ' + $('#txb-msg').textContent);
      ok(log.bmps.filter(b => !src.frames.some(f => f.bmp === b)).every(shut) && log.tidy(), 'A left something open: ' + JSON.stringify(log));
    } finally { log.restore(); await txbOff(); }
  });

  test('TRYX: closing the builder after the first frame stops the decoding, frees it all, and the builder opens again ready', async () => {
    await fresh(); await txbOn();
    const A = { n: 5, w: 200, h: 100, hold: { 1: gate() } }, log = fakeDecoder({ A });
    try {
      const a = txbLoad(gif('A')); await wait(30);
      ok(log.bmps.length === 1, 'the first frame: ' + log.bmps.length);
      txbClose(); ok(!TXB && $('#txb').classList.contains('hidden'), 'it did not close while loading');
      A.hold[1].go(); await a;
      ok(log.imgs <= 2 && log.bmps.every(shut) && log.tidy() && !TXB, 'after closing: ' + JSON.stringify(log));
      $('#dev-st-14').closest('.card').querySelector('.dev-build').click();
      ok(TXB && !TXB.src && !$('#txb-go').disabled && $('#txb-msg').textContent === '', 'reopened: ' + $('#txb-msg').textContent);
    } finally { log.restore(); await txbOff(); }
  });

  test('TRYX: a frame breaking halfway frees the frames made, closes the decoder and keeps the picture there before', async () => {
    await fresh(); await txbOn();
    const S = { n: 1, w: 300, h: 150 }, E = { n: 5, w: 200, h: 100, fail: 2 }, log = fakeDecoder({ S, E });
    try {
      await txbLoad(gif('S')); const src = TXB.src;
      ok(src.bmp && src.w === 300 && !src.dur, 'the still: ' + JSON.stringify(src));
      await txbLoad(gif('E'));
      ok(TXB.src === src && !shut(src.bmp) && $('#txb-msg').textContent === t('txb.bad') && $('#txb-msg').classList.contains('warn') && !$('#txb-go').disabled, 'after the broken one: ' + $('#txb-msg').textContent);
      const made = log.bmps.filter(b => b !== src.bmp);
      ok(made.length === 2 && made.every(shut) && log.tidy(), 'left open: ' + JSON.stringify(log));
    } finally { log.restore(); await txbOff(); }
  });

  test('TRYX: big animation frames come down to the screen size and past the memory cap the animation is cut short, saying so', async () => {
    await fresh(); await txbOn();
    const L = { n: 40, w: 2880, h: 2880 }, log = fakeDecoder({ L });
    try {
      await txbLoad(gif('L')); const s = TXB.src;
      ok(s.w === 1440 && s.h === 1440, 'not scaled down: ' + s.w + 'x' + s.h);
      ok(s.frames.length === 32 && s.frames.length * 1440 * 1440 * 4 <= 256 << 20, 'frames kept: ' + s.frames.length);
      ok($('#txb-msg').textContent === t('txb.cut', 3.2) && $('#txb-msg').classList.contains('warn') && !$('#txb-go').disabled, 'the message: ' + $('#txb-msg').textContent);
      ok(log.bmps.length === 33 && shut(log.bmps[32]) && log.tidy(), 'the frame over the cap: ' + JSON.stringify({ n: log.bmps.length, dec: log.dec, decClosed: log.decClosed }));
      txbClose(); ok(log.bmps.every(shut), 'closing left frames open');
    } finally { log.restore(); await txbOff(); }
  });

  // a stand-in VideoEncoder and VideoFrame (plan: the encode call that throws, a failing flush), counting what was made
  // and closed; frames come from a timer, as a hidden pane runs none. The work a broken or stopped video must leave as it was
  const fakeEncoder = (plan = {}) => {
    const real = { E: window.VideoEncoder, F: window.VideoFrame, raf: window.requestAnimationFrame, caf: window.cancelAnimationFrame, draw: window.txbDraw };
    const log = { enc: 0, encClosed: 0, frames: 0, framesClosed: 0, encoded: 0, draws: 0, previews: 0 };
    window.VideoEncoder = class {
      static isConfigSupported() { return Promise.resolve({ supported: true }); }
      constructor({ output }) { this.out = output; this.state = 'unconfigured'; this.encodeQueueSize = 0; log.enc++; }
      configure() { this.state = 'configured'; }
      encode(f, o) {
        if (++log.encoded === plan.encode) throw new Error('encode broke');
        this.out({ byteLength: 4, timestamp: f.timestamp, type: o.keyFrame ? 'key' : 'delta', copyTo: b => b.set([0, 0, 0, 1]) }, { decoderConfig: { description: new Uint8Array([1, 66, 0, 40]) } });
      }
      flush() { return plan.flush ? Promise.reject(new Error('flush broke')) : Promise.resolve(); }
      close() { this.state = 'closed'; log.encClosed++; }
    };
    window.VideoFrame = class { constructor(c, { timestamp }) { this.timestamp = timestamp; log.frames++; } close() { log.framesClosed++; } };
    window.requestAnimationFrame = cb => setTimeout(() => cb(performance.now()), 16); window.cancelAnimationFrame = clearTimeout;
    window.txbDraw = (c, tt, sel) => { if (sel) log.previews++; else if (++log.draws === plan.draw) throw new Error('draw broke'); return real.draw(c, tt, sel); };
    log.restore = () => { window.VideoEncoder = real.E; window.VideoFrame = real.F; window.requestAnimationFrame = real.raf; window.cancelAnimationFrame = real.caf; window.txbDraw = real.draw; };
    log.tidy = () => log.enc === log.encClosed && log.frames === log.framesClosed;
    return log;
  };
  const within = (p, ms = 4000) => Promise.race([p, wait(ms).then(() => { throw new Error('it never ended: busy=' + TXB?.busy); })]);
  const txbAnim = async dur => { await txbOn(); $('#txb-text').click(); txbZoom(2);
    const frames = [];
    for (let i = 0; i < 3; i++) { const c = document.createElement('canvas'); c.width = 400; c.height = 200; frames.push({ bmp: await createImageBitmap(c), at: i * dur / 3, dur: dur / 3 }); }
    TXB.src = { frames, w: 400, h: 200, dur }; TXB.cx = .4; txbClamp(); return TXB.src; };
  const txbKept = async (src, log, why) => {
    const B = TXB, p = log.previews;
    ok(B && !B.busy && B.src === src && src.frames.every(f => !shut(f.bmp)) && B.layers.length === 1 && B.z === 2 && Math.abs(B.cx - .4) < 1e-9 && B.shape === '2:1', why + ': the work changed');
    ok(['#txb-go', '#txb-src', '#txb-cancel', '#txb-x', '#txb-shape', '#txb-zoom'].every(s => !$(s).disabled) && !$('#txb').classList.contains('hidden'), why + ': the builder is not usable');
    // the preview draws each new frame of the animation (one lasts dur / frames): the loop must run and draw the next one
    ok(B.raf, why + ': the preview loop stopped'); await wait(src.dur * 1000 / src.frames.length + 80); ok(log.previews > p, why + ': the preview stopped');
  };

  test('TRYX: a video frame failing to encode, the first or the second, ends the making, frees the encoder and every frame, keeps the work, and a retry saves', async () => {
    await fresh(); const src = await txbAnim(.3); let log = null;
    try {
      for (const n of [1, 2]) {
        log = fakeEncoder({ encode: n }); sent.length = 0;
        await within(txbMake());
        ok($('#txb-msg').textContent === t('txb.fail', 'encode broke') && $('#txb-msg').classList.contains('warn'), n + ': ' + $('#txb-msg').textContent);
        ok(log.tidy() && log.encoded === n && !sent.some(o => o.cmd === 'tryx_build'), n + ': left open or sent: ' + JSON.stringify(log));
        await txbKept(src, log, 'frame ' + n); log.restore(); log = null;
      }
      log = fakeEncoder(); sent.length = 0; await within(txbMake());
      const o = sent.find(x => x.cmd === 'tryx_build');
      ok(o && o.ext === 'mp4' && $('#txb-msg').textContent === t('txb.saved') && !$('#txb-msg').classList.contains('warn') && log.tidy() && log.frames > 2, 'the retry: ' + $('#txb-msg').textContent + ' ' + JSON.stringify(log));
    } finally { log?.restore(); await txbOff(); }
  });

  test('TRYX: drawing a frame or the flush failing ends the making with the reason and frees it all', async () => {
    await fresh(); const src = await txbAnim(.3); let log = null;
    try {
      for (const [plan, e] of [[{ draw: 1 }, 'draw broke'], [{ draw: 2 }, 'draw broke'], [{ flush: 1 }, 'flush broke']]) {
        log = fakeEncoder(plan); sent.length = 0;
        await within(txbMake());
        ok($('#txb-msg').textContent === t('txb.fail', e) && log.tidy() && !sent.some(o => o.cmd === 'tryx_build'), JSON.stringify(plan) + ': ' + $('#txb-msg').textContent + ' ' + JSON.stringify(log));
        await txbKept(src, log, JSON.stringify(plan)); log.restore(); log = null;
      }
    } finally { log?.restore(); await txbOff(); }
  });

  test('TRYX: Cancel or Escape while the video is made stops it, frees it, saves nothing, keeps the work; one making at a time; a retry saves', async () => {
    await fresh(); const src = await txbAnim(1.5); let log = null;
    try {
      for (const how of ['cancel', 'escape', 'cross']) {
        log = fakeEncoder(); sent.length = 0;
        const p = txbMake(); await wait(100);
        ok(TXB.busy && log.frames > 0 && !$('#txb-cancel').disabled && !$('#txb-x').disabled && $('#txb-go').disabled && $('#txb-src').disabled, how + ': while making: ' + JSON.stringify(log));
        await txbMake(); ok(log.enc === 1, how + ': a second making started');
        $('#txb').dispatchEvent(new PointerEvent('pointerdown', { bubbles: true })); ok(!TXB.stop, how + ': a click beside the builder stopped it');
        if (how === 'cancel') $('#txb-cancel').click(); else if (how === 'cross') $('#txb-x').click(); else key(document.body, 'Escape');
        await within(p);
        ok($('#txb-msg').textContent === t('txb.stopped') && !$('#txb-msg').classList.contains('warn'), how + ': ' + $('#txb-msg').textContent);
        ok(log.tidy() && !sent.some(o => o.cmd === 'tryx_build') && log.draws < 60, how + ': left open, sent or ran on: ' + JSON.stringify(log));
        await txbKept(src, log, how); log.restore(); log = null;
      }
      log = fakeEncoder(); sent.length = 0; await within(txbMake(), 6000);
      ok(sent.some(o => o.cmd === 'tryx_build') && $('#txb-msg').textContent === t('txb.saved') && log.tidy(), 'the retry: ' + $('#txb-msg').textContent);
    } finally { log?.restore(); await txbOff(); }
  });

  test('TRYX: once the video is handed to the core, Cancel is off and never says it stopped what the core may have saved', async () => {
    await fresh(); const src = await txbAnim(.3); const log = fakeEncoder();
    try {
      sent.length = 0; const p = txbMake();
      while (!sent.some(o => o.cmd === 'tryx_build')) await wait(4);
      ok(TXB.busy && $('#txb-cancel').disabled && $('#txb-x').disabled && $('#txb-msg').textContent === t('txb.saving'), 'while saving: ' + $('#txb-msg').textContent);
      txbClose(); key(document.body, 'Escape');
      await within(p);
      ok($('#txb-msg').textContent === t('txb.saved') && !$('#txb').classList.contains('hidden') && log.tidy(), 'after: ' + $('#txb-msg').textContent);
      await txbKept(src, log, 'saved');
    } finally { log.restore(); await txbOff(); }
  });

  test('OpenRGB: a board whose connectors have 0 LEDs says it needs setup, even switched off, and opens its LED counts', async () => {
    await fresh();
    STATE.ext.devs.push({ id: 15, kind: 'openrgb', title: 'OpenRGB', name: 'X870E', host: '127.0.0.1', sub: 5, leds: 0, per_led: 1, online: 0, enabled: 0, type: 'board', info: 'OpenRGB · set the LEDs on its connectors', zones: 'Addressable RGB Header 1:0:0:120|Addressable RGB Header 2:0:0:120' });
    STATE.cfg['dev.15'] = { kind: 'openrgb', enabled: '0' }; await fanEmit();
    ok($('#dev-st-15').textContent === t('zones.need'), 'the status: ' + $('#dev-st-15').textContent);
    ok($('#dev-st-15').closest('.card').querySelector('details.dev-adv').open, 'the LED counts are not open');
    STATE.ext.devs.pop(); delete STATE.cfg['dev.15']; await fanEmit();
  });

  test('keyboard: 60% / TKL / full size picked by its key count, the keys lit in reading order from a thinned-out frame', async () => {
    ok(KB[60].keys.length === 61 && KB.tkl.keys.length === 87 && KB.full.keys.length === 104, 'the key counts: ' + [KB[60], KB.tkl, KB.full].map(l => l.keys.length));
    const pick = n => Object.keys(KB).find(k => KB[k] === kbLayout(n));
    ok([1, 24, 25, 61, 68, 69, 70, 84, 87, 95, 96, 104, 160].map(pick).join() === 'full,full,60,60,60,60,tkl,tkl,tkl,tkl,full,full,full', 'the thresholds: ' + [1, 24, 25, 61, 68, 69, 70, 84, 87, 95, 96, 104, 160].map(pick));
    ok(KB.full.keys.every(([x, y, w, h]) => x + 14 + w <= KB.full.w - 10 && y + 14 + h <= KB.full.h - 10), 'a key off the plate');
    // 104 LEDs come thinned out to 52 colours: the first key from the first, the last key from the last
    const cv_ = Object.assign(document.createElement('canvas'), { width: 868, height: 345 }), c = cv_.getContext('2d');
    const cols = [...Array(52)].map((_, i) => i === 0 ? '#ff0000' : i === 51 ? '#00ff00' : '#000000');
    drawFixture(c, 0, 0, 868, 345, 'keyboard', cols, true, 104);
    const L = KB.full, s = Math.min(868 * .95 / L.w, 345 * .8 / L.h), ox = 434 - L.w * s / 2, oy = 172.5 - L.h * s / 2;
    const px = ([x, y, w, h]) => [...c.getImageData(Math.round(ox + (14 + x + w / 2) * s), Math.round(oy + (14 + y + h / 2) * s), 1, 1).data];
    const [first, last, mid] = [px(L.keys[0]), px(L.keys[103]), px(L.keys[50])];
    ok(first[0] > 100 && first[1] < 40, 'Esc not red: ' + first);
    ok(last[1] > 100 && last[0] < 40, 'Num . not green: ' + last);
    ok(mid[0] < 80 && mid[1] < 80, 'an unlit key lit: ' + mid);
    // the tile is as wide as the layout
    await fresh();
    const it = () => heroItems(true).find(i => i.key === 'ext2').w, dv = STATE.ext.devs.find(d => d.id === 2), keep = dv.leds, full = it();
    dv.leds = 61; await fanEmit(); const small = it();
    dv.leds = keep; await fanEmit();
    ok(full > small && Math.abs(full - KB.full.w / KB.full.h * .85) < 1e-9, 'the tile widths: ' + full + ' / ' + small);
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

  test('home: what needs attention, each with its help; the fans as they run; where to start when nothing is found', async () => {
    await fresh(); showTab('home');
    const row = [...$$('#home-attn-list .found-row')].find(r => r.textContent.includes('Bedside'));
    ok(!$('#home-attn').classList.contains('hidden') && row && row.textContent.includes(t('dev.offline')), 'the offline bulb not listed: ' + $('#home-attn-list').textContent);
    row.querySelector('[data-help]').click(); await wait(30);
    ok($('#tab-help').classList.contains('active') && $('#help-offline').open, 'Help does not open its topic');
    $('#help-offline').open = false; showTab('home');
    ok(!$('#home-cool').classList.contains('hidden') && $('#home-cool').textContent.includes(STATE.fans.f[0].rpm + ' ' + t('home.rpm')), 'the fans not shown: ' + $('#home-cool').textContent);
    $('#home-cool').click();
    ok($('#tab-fans').classList.contains('active'), 'the fans card does not open Fans');
    showTab('home');
    ok($('#home-first').classList.contains('hidden'), 'where to start shown with devices found');
    const none = JSON.parse(JSON.stringify(STATE));
    Object.assign(none, { bulbs: [], nano: { ctls: [] }, sticks: 0, msi: null, fans: { on: 0 }, ll: null }); none.ext.devs = [];
    emit(none); await wait(80);
    ok(!$('#home-first').classList.contains('hidden') && $('#home-attn').classList.contains('hidden') && $('#home-cool').classList.contains('hidden'), 'nothing found: where to start not shown alone: ' + [$('#home-first').className, $('#home-attn').className, $('#home-attn-list').textContent.replace(/\s+/g, ' '), $('#home-cool').className, gdOn()].join(' / '));
    emit(STATE); await wait(80);
    ok(['en', 'ru', 'fr'].every(l => ['home.first', 'home.attn', 'help.title', 'help.start.text', 'help.terms', 'help.terms.text', 'help.offline.text', 'help.orgb.text', 'help.conflict.text', 'help.fans.text', 'help.diag.text', 'help.diag.open'].every(k => STR[l][k])), 'a language misses the help texts');
  });

  test('home: changes refused by a full settings file are said once, with how many; Got it hides them until the next', async () => {
    await fresh(); showTab('home');
    const row = () => [...$$('#home-attn-list .found-row')].find(r => r.textContent.includes(t('home.cfgfull'))), n0 = presets().length;
    try {
      ok(!row(), 'a full settings file said before anything was refused');
      window.mockCfgFull = 1;
      send({ cmd: 'preset_save', id: '0', name: 'cap test', effect: 'fire' }); await wait(120);
      ok(presets().length === n0 && !presets().some(p => p.name === 'cap test'), 'a refused preset still came in');
      let r = row();
      ok(r && r.textContent.includes(t('home.cfgfull.why', 1)) && r.querySelector('[data-a="cfgok"]'), 'no row for the refused change: ' + $('#home-attn-list').textContent);
      r.querySelector('[data-a="cfgok"]').click(); await wait(60);
      ok(!row(), 'Got it did not hide the row');
      send({ cmd: 'preset_copy', id: String((presets()[0] || {}).id || 1), name: 'x' }); await wait(120);
      r = row(); ok(r && r.textContent.includes(t('home.cfgfull.why', 1)), 'a new refusal after Got it is not counted from there: ' + (r && r.textContent));
      ok(['en', 'ru', 'fr'].every(l => ['home.cfgfull', 'home.cfgfull.why', 'home.cfgfull.ok', 'cmp.st.err.cap', 'cmp.draft.cap', 'prof.err.room', 'bk.full', 'bk.bad_prof'].every(k => STR[l][k])), 'a language misses a full-settings text');
    } finally { window.mockCfgFull = 0; HOME.cfgOk = STATE.cfg_full; updateHome(); }
  });

  test('home: a failed save, a failed profile and fans not given back are listed with their own action; one cause is one row; fixed ones go', async () => {
    await fresh(); showTab('home');
    const rows = () => [...$$('#home-attn-list .found-row')], find = s => rows().find(r => r.textContent.includes(s));
    const keep = JSON.parse(JSON.stringify(STATE));
    try {
      STATE.unsaved = 1; emit(STATE); await wait(80);
      let r = find(t('home.unsaved'));
      ok(r && r.textContent.includes(t('home.unsaved.why')) && r.querySelector('[data-a="save"]').textContent === t('home.save.again') && !r.querySelector('[data-help]'), 'the unsaved row: ' + (r && r.innerHTML));
      window.mockSaveFail = 1; sent.length = 0;
      r.querySelector('[data-a="save"]').click(); await wait(120);
      ok(sent.some(o => o.cmd === 'save_retry') && find(t('home.unsaved')), 'a save that fails again: the row must stay');
      ok(find(t('home.unsaved')).querySelector('[data-a="save"]').disabled, 'Save again can be pressed again at once');
      window.mockSaveFail = 0; await wait(3000);
      find(t('home.unsaved')).querySelector('[data-a="save"]').click(); await wait(120);
      ok(!find(t('home.unsaved')), 'the row stays after the save went through');

      STATE.prof = { err: 1, id: 0, n: 1 }; emit(STATE); await wait(80);
      r = find(t('prof.label'));
      ok(r && r.querySelector('[data-a="prof"]'), 'no row for a profile that failed: ' + $('#home-attn-list').textContent);
      STATE.prof = { err: 5, id: 0, n: 2 }; emit(STATE); await wait(80);
      ok(!rows().some(x => x.querySelector('[data-a="prof"]')), 'a full profile list is not a problem to fix here');
      STATE.prof = { err: 0, id: 0, n: 2 };

      STATE.fans.wd = [3, 0]; STATE.fans.rfail = 2; emit(STATE); await wait(80);
      r = find(t('fans.wd.fail', 'QUADRO', 2));
      ok(r && r.querySelector('[data-a="open"]') && r.querySelector('[data-help="fans"]'), 'the fans not given back: ' + $('#home-attn-list').textContent);
      r.querySelector('[data-a="open"]').click(); await wait(30);
      ok($('#tab-fans').classList.contains('active'), 'Open does not go to Fans');
      showTab('home'); STATE.fans.wd = [0, 0]; STATE.fans.rfail = 0;

      const off = STATE.ext.devs.filter(d => !d.online && d.enabled && !d.own), base = STATE.ext.devs.length;
      ok(off.length, 'the mock has no device waiting');
      const twin = (n, id) => Object.assign(JSON.parse(JSON.stringify(off[0])), { id, name: n });
      STATE.ext.devs.push(twin('Twin A', 901), twin('Twin B', 902), twin('Twin C', 903));
      emit(STATE); await wait(80);
      const same = rows().filter(x => x.textContent.includes('Twin'));
      ok(same.length === 1 && same[0].textContent.includes(t('home.more', 2)), 'one cause, several rows: ' + same.map(x => x.textContent).join(' | '));
      same[0].querySelector('[data-a="open"]').click(); await wait(30);
      ok($('#tab-devices').classList.contains('active'), 'Open on a merged row does not go to Devices');
      showTab('home');
      STATE.ext.devs.length = base; emit(STATE); await wait(80);
      ok(!rows().some(x => x.textContent.includes('Twin')), 'removed devices still listed');
      ok(['en', 'ru', 'fr'].every(l => ['home.unsaved', 'home.unsaved.why', 'home.save.again', 'home.why', 'home.more'].every(k => STR[l][k])), 'a language misses the new Home texts');
    } finally { window.mockSaveFail = 0; Object.assign(STATE, keep); emit(STATE); await wait(80); }
  });

  test('home cooling: old readings not shown as now, a lost QUADRO is one row while the board reads on; problems first, worst first, each opens its fan and goes once fixed', async () => {
    await fresh(); await fanReset(); showTab('home');
    const F = STATE.fans, q = F.f.slice(), rpm = n => n + ' ' + t('home.rpm'), cool = () => $('#home-cool').textContent;
    const rows = () => HOME.attn.filter(x => x.fan), up = async () => { await fanEmit(); showTab('home'); updateHome(); };
    F.chip = 'NCT6799D'; F.f.push({ b: 1, rpm: 900, pct: 40, own: 0, want: -1, out: -1, ok: 1, em: '', err: '' }); await up();
    ok(cool().includes(rpm(q[0].rpm)) && cool().includes(rpm(900)) && !rows().length, 'fresh: ' + cool());
    F.age = 9000; await up();
    ok(!cool().includes(rpm(q[0].rpm)) && !cool().includes(rpm(q[1].rpm)) && cool().includes(t('fans.tile.old')) && cool().includes(rpm(900)), 'old readings shown as now, or the board lost with them: ' + cool());
    ok(rows().length === 1 && rows()[0].n === 'QUADRO' && rows()[0].why === t('fans.st.old'), 'old readings: ' + JSON.stringify(rows()));
    F.link = 0; await up();
    ok(rows().length === 1 && rows()[0].why === t('fans.st.stale') && HOME.attn[0] === rows()[0] && cool().includes(t('fans.tile.nolink')) && cool().includes(rpm(900)), 'a lost link: ' + JSON.stringify(rows()) + cool());
    $('#home-attn-list .found-row [data-a="open"]').click(); await wait(120);
    ok(SHEET.kind === 'fan' && tab === 'home', 'the row does not open its fan');
    closeSheet();
    Object.assign(F, { link: 1, age: 400 }); await up();
    ok(cool().includes(rpm(q[0].rpm)) && !rows().length, 'back: ' + cool());
    // control asked and taken, or not confirmed: the line under the rpm tells which
    setCfg('fans', 'fan2', 'curve'); Object.assign(F.f[1], { own: 1, want: 50, ok: 1 }); await up();
    const line = () => $('#home-cool [data-hf="1"] small').textContent;
    ok(line() === fanMode(1) + ' · ' + t('fans.st.ok'), 'applied: ' + line());
    F.f[1].ok = 0; await up();
    ok(line() === fanMode(1) + ' · ' + t('fans.st.unconf') && !rows().length, 'not confirmed: ' + line());
    F.f[1].ok = -1; F.f[0].em = 'nosensor'; await up();
    ok(rows().map(x => x.k).join() === '2,3' && rows()[0].n === fanName(0) && rows()[1].n === fanName(1), 'the worst first: ' + JSON.stringify(rows()));
    F.f[0].em = ''; F.f[1].ok = 1; await up();
    ok(!rows().length, 'still listed once fixed: ' + JSON.stringify(rows()));
    F.f.splice(0, F.f.length, ...q); delete F.chip; setCfg('fans', 'fan2', '');
    await fanReset(); showTab('home');
    $('#home-cool').nextElementSibling.querySelector('[data-help=""]').click(); await wait(30);
    ok($('#tab-help').classList.contains('active'), 'Help and diagnostics on Home opens nothing');
    showTab('home');
  });

  test('a device card keeps its last events: newest first, the same one counted not repeated, none shown without any', async () => {
    await fresh(); showTab('devices');
    const ev = id => $('#dev-ev-' + id), lines = id => ev(id).querySelector('pre').textContent.split('\n');
    ok(ev(3) && !ev(3).classList.contains('hidden') && lines(3).length === 3, 'the events: ' + ev(3)?.textContent);
    ok(lines(3)[0].endsWith(t('dev.ev.3') + ' ×4') && lines(3)[2].endsWith(t('dev.ev.1')), 'not newest first, or the repeat not counted: ' + lines(3));
    ok(ev(1).classList.contains('hidden'), 'an empty history shown');
    const d = devById(1), now = Date.now() / 1e3 | 0;
    d.ev = [[now, 5, 3, 1], [now, 6, 2, 1]]; updateDevices();
    ok(lines(1)[0].endsWith(t('dev.ev.6.2')) && lines(1)[1].endsWith(t('dev.ev.5', t('drive.3'))), 'mode or let go: ' + lines(1));
    delete d.ev; updateDevices();
    ok(['en', 'ru', 'fr'].every(l => ['dev.ev', 'dev.ev.1', 'dev.ev.2', 'dev.ev.3', 'dev.ev.4', 'dev.ev.5', 'dev.ev.6.0', 'dev.ev.6.1', 'dev.ev.6.2'].every(k => STR[l][k])), 'a language misses the event texts');
  });

  test('help: the diagnostics file saved on a click, sends nothing else, then offered to open and check', async () => {
    await fresh(); showTab('settings'); sent.length = 0;   // leaving Home tells the core to slow its frames (frameNeed)
    ok($('#help-diag-open').classList.contains('hidden'), 'Open to check shown before a file is saved');
    $('#help-diag-save').click();
    ok(sent.length === 1 && sent[0].cmd === 'diag', 'Save sent: ' + JSON.stringify(sent));
    await wait(400);
    ok(!$('#help-diag-open').classList.contains('hidden') && $('#help-diag-st').textContent.includes(STATE.diag.file), 'the saved file not shown: ' + $('#help-diag-st').textContent);
    sent.length = 0; $('#help-diag-open').click();
    ok(sent.length === 1 && sent[0].cmd === 'open' && sent[0].what === 'diag', 'Open to check sent: ' + JSON.stringify(sent));
    delete STATE.diag; emit(STATE); await wait(80); showTab('home');
  });

  test('a report answered on the site: shown once with what it said, Answer opens a new report about it; a done one as before', async () => {
    await fresh();
    STATE.diag = { fixed: { id: 'aaaaaaaaaa,bbbbbbbbbb', rid: 'bbbbbbbbbb', text: 'line1\nline2', report: 'the lights blink' } };
    emit(STATE); await wait(80);
    ok(!$('#fixed').classList.contains('hidden'), 'the answer not shown');
    ok($('#fixed-title').textContent === t('fixed.r.title') && $('#fixed-hint').textContent === t('fixed.r.done'), 'not the answer texts: ' + $('#fixed-title').textContent + ' / ' + $('#fixed-hint').textContent);
    ok($('#fixed-text').textContent === 'line1\nline2' && !$('#fixed-was').classList.contains('hidden') && $('#fixed-report').textContent === 'the lights blink', 'the answer or the report not shown');
    STATE.diag = {}; $('#fixed-answer').click(); await wait(80);
    ok(sent.some(m => m.cmd === 'report_seen' && m.id === 'aaaaaaaaaa,bbbbbbbbbb'), 'not marked as seen: ' + JSON.stringify(sent));
    ok($('#fixed').classList.contains('hidden') && !$('#report-dlg').classList.contains('hidden'), 'the report window not opened');
    ok($('#report-text').value.startsWith('Re #bbbbbbbbbb: '), 'the new report does not name the answered one: ' + $('#report-text').value);
    $('#report-text').value = ''; key(document.body, 'Escape'); await wait(60);
    STATE.diag = { fixed: { id: 'cccccccccc', text: 'fixed in 0.4.52' } }; emit(STATE); await wait(80);
    ok($('#fixed-title').textContent === t('fixed.title') && $('#fixed-was').classList.contains('hidden') && $('#fixed-answer').classList.contains('hidden'), 'a done report shown as an answer');
    STATE.diag = {}; $('#fixed-ok').click(); await wait(60);
    ok(sent.some(m => m.cmd === 'report_seen' && m.id === 'cccccccccc'), 'done not marked as seen');
    delete STATE.diag; emit(STATE); await wait(80);
  });

  test("help: the tray's Help opens the Help card, sends nothing", async () => {
    await fresh(); showTab('home'); sent.length = 0;
    emit({ type: 'go', to: 'help' }); await wait(80);
    ok(tab === 'help' && $('#tab-help').classList.contains('active') && $('#help').offsetParent, 'not on the Help card: ' + tab);
    // the one message: no picture to draw on Help, so the core is told to slow its frames (frameNeed); nothing else
    ok(sent.length === 1 && sent[0].cmd === 'frames' && sent[0].live === 0, 'sent: ' + JSON.stringify(sent));
    showTab('home');
  });

  test("the tray's Settings opens the Settings page, not Help", async () => {
    await fresh(); showTab('home');
    emit({ type: 'go', to: 'settings' }); await wait(80);
    ok(tab === 'settings' && $('#tab-settings').classList.contains('active'), 'not on Settings: ' + tab);
    ok($('#nav button[data-tab="settings"]').getAttribute('aria-current') === 'page', 'the nav does not say so');
    showTab('home');
  });

  test('each page comes back where it was left this session; a jump to a card wins; nothing is stored', async () => {
    await fresh(); const m = $('main'), max = () => m.scrollHeight - m.clientHeight;
    showTab('help'); ok(max() > 200, 'Help too short to scroll here: ' + max());
    m.scrollTop = 150; showTab('settings'); ok(m.scrollTop === 0, 'a page never seen does not start at the top: ' + m.scrollTop);
    m.scrollTop = 40; showTab('help'); ok(m.scrollTop === 150, 'Help not back where it was: ' + m.scrollTop);
    showTab('settings'); ok(m.scrollTop === Math.min(40, max()), 'Settings not back where it was: ' + m.scrollTop);
    showTab('help'); showTab('help'); ok(m.scrollTop === 150, 'the same page again moved: ' + m.scrollTop);
    showTab('settings'); tabScroll.help = 1e6; showTab('help'); ok(m.scrollTop === max(), 'not clamped to the page: ' + m.scrollTop);
    // the Programs in the way jump scrolls to its card after the page is shown
    m.scrollTop = 0; showTab('home'); const c = $('#cf-card'), hid = c.classList.contains('hidden'); c.classList.remove('hidden');
    go('cf'); await wait(900); const r = c.getBoundingClientRect(), mr = m.getBoundingClientRect();
    ok(r.bottom > mr.top && r.top < mr.bottom, 'the jump did not reach its card');
    c.classList.toggle('hidden', hid);
    ok(!Object.keys(localStorage).some(k => /scroll/i.test(k)), 'a scroll place was stored');
    showTab('home');
  });

  test('a steady-colour-only controller: no effect to pick, the colour it holds told, one chosen in place', async () => {
    await fresh();
    const d = S.ext.devs.find(x => x.id === 10), sec = 'zone.dev10', was = { mode: cv(sec, 'mode', ''), palette: cv(sec, 'palette', '') };
    const el = document.createElement('div'); el.className = 'zone'; el.dataset.zone = sec; document.body.appendChild(el);
    try {
      setCfg(sec, 'mode', 'effect'); setCfg(sec, 'palette', '');
      d.cap = 3; renderZone(el);
      ok(!el.querySelector('.zfx') && el.querySelector('.zsteady'), 'the effect choice shown: ' + el.innerHTML.slice(0, 200));
      const ms = [...el.querySelectorAll('.seg button')].map(b => b.dataset.m).join();
      ok(ms === 'static,white' && !el.querySelector('.seg button.on'), 'modes: ' + ms);
      ok(el.querySelector('.zfb').textContent.includes(fxName(S.effect)), 'the colour it holds: ' + el.querySelector('.zfb').textContent);
      el.querySelector('.seg button[data-m="static"]').click();
      ok(cv(sec, 'mode') === 'static' && parsePal(cv(sec, 'palette'))[0] === effPal(S.effect)[0], 'Colour did not keep the one it held: ' + cv(sec, 'palette'));
      ok(el.querySelector('.swatches') && el.querySelector('.seg button.on').dataset.m === 'static', 'no colour to pick');
      delete d.cap; renderZone(el);
      ok(el.querySelector('.zfx') && el.querySelectorAll('.seg button').length === MODES.length, 'a full controller lost its choices');
    } finally { delete d.cap; el.remove(); setCfg(sec, 'palette', was.palette); setCfg(sec, 'mode', was.mode); }
  });

  // ---- animations: one setting, nothing left running, scenes that end cleanly (the preview's page counts as hidden,
  // so these say it is in front while they run)
  const shown = v => Object.defineProperty(document, 'hidden', { value: !v, configurable: true });
  const motionWas = () => ({ theme: cv('general', 'theme', 'dark'), unlocked: cv('general', 'unlocked', ''), mo: cv('general', 'ui_motion', 'sys') });
  const motionBack = w => {
    delete document.hidden; delete RM.matches; sceneStop(false); document.body.classList.remove('idle');
    $$('.ky-fly, .vchat').forEach(x => x.remove());
    setCfg('general', 'unlocked', w.unlocked); setCfg('general', 'ui_motion', w.mo); applyMotion(); setTheme(w.theme);
  };
  const motion = v => { setCfg('general', 'ui_motion', v); applyMotion(); };

  test('animations: Off and On apply at once, As in Windows follows the system, a choice made stays', async () => {
    await fresh();
    const w = motionWas(), calm = () => document.body.classList.contains('calm'), seg = v => $(`#ui-motion button[data-v="${v}"]`);
    const sys = m => { Object.defineProperty(RM, 'matches', { value: m, configurable: true }); RM.dispatchEvent(new Event('change')); };
    try {
      showTab('settings');
      seg('0').click(); ok(calm() && sets('general', 'ui_motion').pop().v === '0' && seg('0').classList.contains('on'), 'Off: ' + document.body.className);
      seg('1').click(); ok(!calm(), 'On left it still');
      sys(true); ok(!calm(), 'an On chosen gave way to the system');
      seg('sys').click(); ok(calm() && seg('sys').classList.contains('on'), 'As in Windows did not follow "reduce"');
      sys(false); ok(!calm(), 'As in Windows did not follow the system turning motion back on');
      delete S.cfg.general.ui_motion; updateSettings();
      ok($('#ui-motion button.on').dataset.v === 'sys', 'never chosen is not As in Windows: ' + $('#ui-motion button.on').dataset.v);
    } finally { motionBack(w); showTab('home'); }
  });

  test('effect previews: one loop, only on Effects in front; the preset window opened twice runs one, none once closed', async () => {
    await fresh();
    const raf = window.requestAnimationFrame, caf = window.cancelAnimationFrame, Q = new Map();
    let id = 1e6, now = performance.now();
    caf(ANIM.raf); caf(PDLG.raf); ANIM.raf = PDLG.raf = 0;
    window.requestAnimationFrame = cb => (Q.set(++id, cb), id);
    window.cancelAnimationFrame = i => Q.delete(i);
    const n = f => [...Q.values()].filter(c => c === f).length;
    const tick = () => { const q = [...Q.values()]; Q.clear(); now += 34; q.forEach(c => c(now)); };
    try {
      showTab('effects'); previewWake(); previewWake();
      ok(n(animate) === 1, 'loops on Effects: ' + n(animate));
      tick(); tick(); ok(n(animate) === 1, 'loops after two frames: ' + n(animate));
      document.body.classList.add('idle'); await wait(0); tick();
      ok(n(animate) === 0 && !ANIM.raf, 'still running in the background');
      document.body.classList.remove('idle'); await wait(0);
      ok(n(animate) === 1, 'loops back in front: ' + n(animate));
      showTab('home'); tick(); ok(n(animate) === 0, 'running off Effects');
      presetDialog(0, 'x'); presetDialog(0, 'x');
      ok(n(pdlgPreview) === 1, 'preset window loops: ' + n(pdlgPreview));
      tick(); ok(n(pdlgPreview) === 1, 'preset window loops after a frame: ' + n(pdlgPreview));
      closePresetDialog(); tick(); ok(n(pdlgPreview) === 0 && !PDLG.raf, 'the closed preset window still draws');
    } finally {
      window.requestAnimationFrame = raf; window.cancelAnimationFrame = caf; Q.clear();
      closePresetDialog(); ANIM.raf = PDLG.raf = 0; previewWake(); showTab('home');
    }
  });

  test('a code scene: a theme picked during it stays, it ends on its own timers, the background or Off ends it with its theme', async () => {
    await fresh();
    const w = motionWas(), th = () => document.documentElement.dataset.theme;
    try {
      shown(true); setCfg('general', 'unlocked', 'verity,kyoka,akapzz'); motion('1'); setTheme('dark');
      kyokaArrives(); ok($('.kyoka-ov') && SCENE, 'no kyoka scene');
      $('#theme button[data-v="grey"]').click();
      ok(!$('.kyoka-ov') && !SCENE && !document.body.classList.contains('ky-pressing'), 'the scene stayed after a theme was picked');
      await wait(2100);
      ok(th() === 'grey' && cv('general', 'theme') === 'grey', 'the stopped scene brought its theme back: ' + th());
      kyokaArrives(); await wait(4200);   // no animationend or transitionend needed: its own timers end it
      ok(!$('.kyoka-ov') && !SCENE && th() === 'kyoka' && !document.body.classList.contains('ky-pressing'), 'did not end on its own: ' + th());
      $$('.ky-fly').forEach(x => x.remove()); setTheme('dark');
      akapzzArrives(); ok($('.akz-ov') && SCENE, 'no akapzz scene');
      shown(false); document.dispatchEvent(new Event('visibilitychange'));
      ok(!$('.akz-ov') && !SCENE && th() === 'akapzz', 'the background did not end it with its theme: ' + th());
      shown(true); setTheme('dark');
      kyokaArrives(); motion('0');
      ok(!$('.kyoka-ov') && !SCENE && th() === 'kyoka', 'Off did not end it with its theme: ' + th());
      setTheme('dark'); akapzzArrives();
      ok(!$('.akz-ov') && !SCENE && th() === 'akapzz', 'with Off a scene still played: ' + th());
    } finally { motionBack(w); }
  });

  test("verity's scene is only looks: the tab, title, a typed field and its focus stay", async () => {
    await fresh();
    const w = motionWas(), inp = $('#code-in'), body = document.body;
    try {
      shown(true); setCfg('general', 'unlocked', 'verity'); motion('1'); setTheme('dark');
      showTab('settings'); setCat('look'); codeShow(true); inp.value = 'abc'; inp.focus();
      const title = $('#title').textContent, dt = document.title;
      const same = () => tab === 'settings' && $('#title').textContent === title && document.title === dt && inp.value === 'abc' && document.activeElement === inp;
      verityArrives(); await wait(400);
      ok($('.vglitch') && same(), 'moved during the scene: ' + tab + ' / ' + $('#title').textContent + ' / ' + inp.value + ' / ' + (document.activeElement || {}).id);
      ok(!document.documentElement.dataset.theme, 'the theme changed before the scene ended');
      sceneStop(true);
      ok(!$('.vglitch') && !body.classList.contains('glitching') && !['--gx', '--gy', '--sk', '--hue', '--v-rgb2', '--look'].some(p => body.style.getPropertyValue(p)), 'the scene left something: ' + body.getAttribute('style'));
      ok(document.documentElement.dataset.theme === 'verity' && same(), 'after the scene: ' + document.documentElement.dataset.theme + ' / ' + tab);
    } finally { inp.value = ''; codeShow(false); motionBack(w); showTab('home'); }
  });

  test('butterflies: paused in the background, none sent then, gone with Off; lights and fans not touched', async () => {
    await fresh();
    const w = motionWas(), body = document.body, paused = f => f.every(x => x.getAnimations().every(a => a.playState === 'paused'));
    try {
      shown(true); motion('1'); await wait(0); sent.length = 0;
      hellButterflies(2); const f = $$('.ky-fly'); ok(f.length === 2, 'flying: ' + f.length);
      body.classList.add('idle'); await wait(0);
      ok(paused(f), 'still flying in the background');
      hellButterflies(1); ok($$('.ky-fly').length === 2, 'a new one in the background');
      body.classList.remove('idle'); await wait(0);
      ok(!f.some(x => x.getAnimations().some(a => a.playState === 'paused')), 'not flying again in front');
      motion('0'); await wait(0);
      ok(!$$('.ky-fly').length, 'left flying with Off');
      ok(!sent.some(o => o.cmd !== 'set' || o.s !== 'general'), 'the decorations sent: ' + JSON.stringify(sent));
    } finally { motionBack(w); }
  });

  test('the intro: a key before it has loaded opens the page at once, the background ends it, stalled frames end it in bounded time', async () => {
    const open = async () => {
      const fr = document.createElement('iframe');
      fr.style.cssText = 'position:fixed;left:0;top:0;width:800px;height:600px;opacity:0;pointer-events:none';
      fr.src = '?mock#intro'; document.body.appendChild(fr);
      let i = 0; while (i++ < 300 && !(fr.contentDocument && fr.contentDocument.URL.includes('mock') && fr.contentDocument.readyState !== 'loading')) await wait(5);
      fr.contentWindow.setCfg('general', 'ui_motion', '1'); fr.contentWindow.applyMotion();   // whatever this Windows says
      return fr;
    };
    const gone = fr => !fr.contentDocument.documentElement.classList.contains('intro') && !fr.contentDocument.getElementById('intro');
    let fr = await open();
    try {
      const ov = fr.contentDocument.getElementById('intro');
      ok(ov && !ov.classList.contains('run') && fr.contentDocument.documentElement.classList.contains('intro'), 'the intro had already started or was gone');
      fr.contentWindow.dispatchEvent(new fr.contentWindow.KeyboardEvent('keydown', { key: 'Escape' }));
      ok(gone(fr), 'a key before it loaded did not open the page');
      fr.remove(); fr = await open();
      let i = 0; while (i++ < 60 && !fr.contentDocument.querySelector('#intro.run')) await wait(50);
      ok(fr.contentDocument.querySelector('#intro.run'), 'the intro did not start');
      Object.defineProperty(fr.contentDocument, 'hidden', { value: true, configurable: true }); fr.contentDocument.dispatchEvent(new Event('visibilitychange'));
      ok(gone(fr) && !fr.contentDocument.documentElement.classList.contains('intro-out'), 'the background did not end it, or ended it with the coming in');
      fr.remove(); fr = await open();
      i = 0; while (i++ < 140 && !gone(fr)) await wait(50);   // the preview's page is hidden: its frames hardly come, the worst slow case
      ok(gone(fr), 'still on after 7 s');
    } finally { fr.remove(); }
  });

  // the first-run guide: each test sets its own devices and puts the sample ones back
  const gdKeep = () => JSON.stringify({ devs: STATE.ext.devs, found: STATE.ext.found, bulbs: STATE.bulbs, ctls: STATE.nano.ctls, prof: STATE.cfg.general.profile, sticks: STATE.sticks, msi: STATE.msi, msi_own: STATE.msi_own, ram_src: STATE.ram_src });
  const gdBack = async k => { const o = JSON.parse(k); Object.assign(STATE.ext, { devs: o.devs, found: o.found, scanning: 0 }); STATE.bulbs = o.bulbs; STATE.nano.ctls = o.ctls; Object.assign(STATE, { sticks: o.sticks, msi: o.msi, msi_own: o.msi_own, ram_src: o.ram_src });
    STATE.test = ''; STATE.cfg.general.profile = o.prof; window.mockScanStuck = window.mockSaveFail = 0; GUIDE.tips = null; GUIDE.saving = null; GUIDE.adding.clear(); GUIDE.test = '';
    setCfg('general', 'guide', ''); emit(STATE); await wait(120); };
  // direct: haku's own memory and MSI drivers, off unless a test is about them
  const gdSet = async (devs, step, direct) => { Object.assign(STATE.ext, { devs, found: [], scanning: 0 }); STATE.bulbs = []; STATE.nano.ctls = [];
    Object.assign(STATE, { sticks: 0, msi: 0, msi_own: 0, ram_src: [] }, direct); emit(STATE); await wait(80);
    showTab('home'); setCfg('general', 'guide', step); updateGuide(); await wait(60); };
  const gd = g => $(`#guide [data-g="${g}"]`), gdSt = () => $('#guide .gd-st').textContent, gdH = () => $('#gd-h').textContent;
  const noHw = () => ok(!sent.some(o => /^fans|^cf_|^orgb_stop|^pc_own|^profile$/.test(o.cmd) || o.cmd === 'set' && o.s === 'fans'), 'fans, programs or the own lighting touched: ' + JSON.stringify(sent));

  test('guide: offered after the welcome of a new install, never after an update; skip, resume, reopen', async () => {
    const k = gdKeep();
    try {
      await fresh(); delete S.cfg.general.guide; delete STATE.cfg.general.guide; showTab('home'); updateGuide();
      ok($('#guide').classList.contains('hidden'), 'an install from before the guide (no key) gets it');
      wizard(true); $('#wz-next').click(); $('#wz-next').click(); $('#wz-next').click(); await wait(60);
      ok(cv('general', 'guide') === 'dev' && !$('#guide').classList.contains('hidden'), 'the welcome did not hand over: ' + cv('general', 'guide'));
      ok($('#home-first').classList.contains('hidden'), 'the empty Home card shows beside the guide');
      ok(gdH() === t('gd.dev.h') && $('#guide .gd-prog').textContent === t('gd.prog', 1, gdSteps().length), 'step 1: ' + gdH());
      gd('skip').click(); await wait(40);
      ok(cv('general', 'guide') === 'skip' && $('#guide').classList.contains('hidden'), 'Skip did not hide it');
      ok(document.activeElement === $('.home-help button'), 'Skip left the focus nowhere');
      setCfg('general', 'guide', 'test'); updateGuide();   // as read back after a restart
      ok(gdH() === t('gd.test.h'), 'not resumed where it was left: ' + gdH());
      setCfg('general', 'guide', 'done'); updateGuide();
      ok($('#guide').classList.contains('hidden'), 'shown again once done');
      sent.length = 0; showTab('settings'); $('#guide-open').click(); await wait(60);
      ok(cv('general', 'guide') === 'dev' && $('#tab-home').classList.contains('active') && !$('#guide').classList.contains('hidden'), 'Getting started did not open it on Home');
      ok(!sent.some(o => /^dev_|^profile/.test(o.cmd) || o.cmd === 'set' && !(o.s === 'general' && /^(guide|tab)$/.test(o.k))), 'reopening changed something: ' + JSON.stringify(sent));
      ok(document.activeElement === $('#gd-h'), 'the focus is not on the guide');
    } finally { await gdBack(k); }
  });

  test('guide device step: scanning, nothing, an error, rescan; Add once; a device added elsewhere; the real support level', async () => {
    const k = gdKeep();
    try {
      await fresh(); await gdSet([], 'dev');
      ok(gd('scan') && gd('scan').textContent === t('dev.scan') && gd('manual'), 'no Find devices or manual add');
      gd('scan').click(); await wait(20);
      ok(sent.filter(o => o.cmd === 'scan').length === 1 && gdSt() === t('dev.scanning.note') && gd('scan').disabled, 'scanning not shown: ' + gdSt());
      await wait(400);
      ok(gdSt() === t('gd.d.none') && $('#guide .gd-st.bad') && gd('scan').textContent === t('gd.d.rescan') && gd('help-start'), 'nothing found not told: ' + gdSt());
      window.mockScanStuck = 1; gd('scan').click(); await wait(30); GUIDE.scanAt -= 31000; updateGuide();
      ok(gdSt() === t('gd.d.err') && !gd('scan').disabled, 'a search that does not end is not an error: ' + gdSt());
      window.mockScanStuck = 0; STATE.ext.scanning = 0;
      STATE.ext.found = [{ kind: 'wled', title: 'WLED', host: '192.168.1.99', sub: -1, name: 'Shelf', leds: 30, info: '', added: 0 }];
      gd('scan').click(); await wait(400);
      ok(gdSt() === t('dev.found', 1) && gd('add'), 'the find not listed: ' + gdSt());
      sent.length = 0; gd('add').click(); gd('add')?.click(); await wait(150);
      ok(sent.filter(o => o.cmd === 'dev_add').length === 1, 'added ' + sent.filter(o => o.cmd === 'dev_add').length + ' times');
      ok(!gd('add') && $('#guide').textContent.includes('Shelf') && $('#guide').textContent.includes(t('gd.lv.fx')), 'the added device not in the list with its level');
      ok(gd('next') && gd('next').classList.contains('primary'), 'no Next once a device is in');
      STATE.ext.devs.push({ id: 41, kind: 'openrgb', title: 'OpenRGB', name: 'Board', host: '127.0.0.1', sub: 4, leds: 8, per_led: 1, online: 1, enabled: 1, cap: 3, info: '' },
        { id: 42, kind: 'openrgb', title: 'OpenRGB', name: 'Odd', host: '127.0.0.1', sub: 5, leds: 8, per_led: 1, online: 1, enabled: 1, cap: 2, info: '' },
        { id: 43, kind: 'openrgb', title: 'OpenRGB', name: 'Dead', host: '127.0.0.1', sub: 6, leds: 8, per_led: 1, online: 1, enabled: 1, cap: 4, info: '' });
      emit(STATE); await wait(80);   // as if added on the Devices page
      const txt = $('#guide').textContent;
      ok(txt.includes(t('gd.lv.static')) && txt.includes(t('gd.lv.exp')) && txt.includes(devStatus(devById(43))), 'static-only, experimental, unsupported not told apart');
      noHw();
    } finally { await gdBack(k); }
  });

  test('guide path step: only for a possible pair, one way chosen, a hint only from what OpenRGB can do', async () => {
    const k = gdKeep();
    try {
      await fresh();
      const dev = (id, kind, name, cap) => ({ id, kind, title: kind, name, host: '127.0.0.1', sub: id, leds: 8, per_led: 1, online: 1, enabled: 1, cap, info: '' });
      await gdSet([dev(61, 'razer', 'BlackWidow'), dev(62, 'openrgb', 'Razer BlackWidow V3', 3), dev(63, 'openrgb', 'ENE DRAM', 1)], 'dev');
      ok(gdSteps().join() === 'dev,path,test,save', 'no path step for a pair: ' + gdSteps());
      gd('next').click(); await wait(40);
      ok(gdH() === t('gd.path.h') && $$('#guide [data-gp]').length === 1, 'not one pair offered');
      ok($('#guide .gd-pair .hint')?.textContent === t('gd.p.rec', 'BlackWidow', t('drive.3')), 'no hint for a static-only OpenRGB device');
      sent.length = 0; const s = $('#guide [data-gp="62"]'); s.value = 'direct:61'; s.dispatchEvent(new Event('change', { bubbles: true })); await wait(80);
      ok(sets('dev.62', 'path').map(o => o.v).join() === 'direct' && sets('dev.62', 'same').map(o => o.v).join() === '61', 'the pair not set once: ' + JSON.stringify(sent));
      ok(!sets('dev.61', 'path').length && !sets('dev.62', 'own').length && !sets('dev.61', 'own').length, 'more than the pair changed');
      ok(devById(61).held + devById(62).held === 1, 'both paths drive it');
      STATE.ext.devs[1].cap = 1; STATE.ext.devs[1].same = 0; setCfg('dev.62', 'same', ''); emit(STATE); await wait(80);
      ok(!$('#guide .gd-pair .hint'), 'a hint from the name alone');
      await gdSet([dev(63, 'openrgb', 'ENE DRAM', 1)], 'dev');
      ok(gdSteps().join() === 'dev,test,save', 'a path step with no pair');
      noHw();
    } finally { await gdBack(k); }
  });

  test('device history and owner: a refusal by another program is its own event; haku owns it only while it reaches it, else the program or unknown', async () => {
    await fresh();
    const base = JSON.parse(JSON.stringify(STATE.ext.devs.find(d => d.online && d.enabled && !d.own && !d.held && !d.covered && !d.pro)));
    const who = x => { const m = devWhy(Object.assign({}, base, x)).match(/<dt>([^<]*)<\/dt><dd>([^<]*)<\/dd>/g).find(r => r.startsWith('<dt>' + t('why.who') + '<'));
      return m && m.replace(/^.*<dd>|<\/dd>$/g, ''); };
    ok(who({}) === 'haku', 'reached: ' + who({}));
    ok(who({ online: 0, fails: 2, info: 'no answer' }) === t('why.who.unk'), 'not reached, no reason: ' + who({ online: 0, fails: 2, info: 'no answer' }));
    ok(who({ online: 0, fails: 1, kind: 'corsairlcd', info: 'iCUE.exe busy' }) === t('why.who.app', 'iCUE'), 'the program the driver names: ' + who({ online: 0, fails: 1, kind: 'corsairlcd', info: 'iCUE.exe busy' }));
    const tx = who({ online: 0, fails: 1, kind: 'tryx', info: 'COM5 busy' });
    ok(tx === (cfRun('tryx') ? t('why.who.app', cfApp('tryx').name) : t('why.who.busy')), 'a port refused, the program not named: ' + tx);
    const ev = devEvText(Object.assign({}, base, { ev: [[1700000000, 1, 0, 1], [1700000060, 7, 0, 3], [1700000120, 2, 0, 1]] })).split('\n');
    ok(ev.length === 3 && ev[0].endsWith(t('dev.ev.2')) && ev[1].endsWith(t('dev.ev.7') + ' ×3') && ev[2].endsWith(t('dev.ev.1')), 'the history: ' + ev.join(' | '));
    ok(['en', 'ru', 'fr'].every(l => ['dev.ev.7', 'why.who.app', 'why.who.busy', 'why.who.unk'].every(k => STR[l][k])), 'a language misses the owner or history texts');
  });

  test('guide test step: one steady colour on that device only, ended by Works, Problem, Stop, Back, leaving, the window, the core; never saved', async () => {
    const k = gdKeep();
    try {
      await fresh();
      const dev = (id, kind, name, x) => Object.assign({ id, kind, title: kind, name, host: '10.0.0.' + id, sub: -1, leds: 8, per_led: 1, online: 1, enabled: 1, info: '' }, x);
      await gdSet([dev(1, 'wled', 'Strip'), dev(2, 'openrgb', 'Board', { cap: 3 }), dev(3, 'openrgb', 'Dead', { cap: 4 }), dev(4, 'tryx', 'Cooler'), dev(11, 'elgato', 'Key Light')], 'test');
      ok([...$('#gd-dev').options].map(o => o.value).join() === '1,2', 'the test offered for ' + [...$('#gd-dev').options].map(o => o.value));
      const txt = $('#guide').textContent;
      ok(txt.includes(t('gd.t.screen')) && txt.includes(t('gd.t.white')) && $$('#guide [data-g="card"]').length === 3, 'the screen, white-only and unsupported ones not sent to their card');
      sent.length = 0; gd('check').click(); await wait(80);
      const tests = () => sent.filter(o => o.cmd === 'dev_test').map(o => o.zone);
      ok(tests().join() === 'zone.dev1' && gdSt() === t('gd.t.on', 'Strip'), 'Check: ' + tests() + ' / ' + gdSt());
      ok(gd('works') && gd('problem') && gd('stop') && !gd('check'), 'no Works / Problem / Stop');
      gd('works').click(); await wait(60);
      ok(tests().join() === 'zone.dev1,' && gdSt() === t('gd.t.ok') && gd('next').classList.contains('primary'), 'Works did not end it: ' + tests());
      ok(sent.every(o => o.cmd === 'dev_test') && devById(1).cap === undefined, 'Works (the user\'s word) sent or marked more than the end of the test: ' + JSON.stringify(sent));
      const d2 = $('#gd-dev'); d2.value = '2'; d2.dispatchEvent(new Event('change', { bubbles: true })); await wait(20);
      sent.length = 0; gd('check').click(); await wait(60); gd('problem').click(); await wait(20);
      ok(tests().join() === 'zone.dev2,' && $$('#guide .gd-probs button').length === 4, 'Problem: ' + tests());
      gd('pr-blink').click(); await wait(20);
      ok($('#guide').textContent.includes(t('gd.pr.blink.t')) && gd('help-conflict') && gd('check').textContent === t('gd.t.again'), 'Blinks has no help or retry');
      gd('pr-colour'); gd('check').click(); await wait(60);
      sent.length = 0; gd('stop').click(); await wait(20);
      ok(tests().join() === '' && gdSt() === t('gd.t.back'), 'Stop: ' + tests() + ' / ' + gdSt());
      sent.length = 0; gd('check').click(); await wait(60); gd('back').click(); await wait(20);
      ok(tests().join() === 'zone.dev2,' && cv('general', 'guide') === 'dev', 'Back left it on: ' + tests());
      setCfg('general', 'guide', 'test'); updateGuide();
      sent.length = 0; gd('check').click(); await wait(60); showTab('devices'); await wait(20);
      ok(tests().join() === 'zone.dev2,', 'leaving Home left it on: ' + tests());
      showTab('home'); sent.length = 0; gd('check').click(); await wait(60); dispatchEvent(new Event('pagehide')); await wait(20);
      ok(tests().join() === 'zone.dev2,', 'closing the window left it on: ' + tests());
      updateGuide(); sent.length = 0; gd('check').click(); await wait(1100);   // the status ticks
      STATE.test = ''; emit(STATE); await wait(60);   // the core's minute, or a layout change
      ok(gdSt() === t('gd.t.ended') && gd('check'), 'the core ending it not shown: ' + gdSt());
      gd('check').click(); await wait(60); devById(2).online = 0; STATE.ext.devs[1].online = 0; gd('stop').click(); await wait(20);
      ok(gdSt() === t('gd.t.lost'), 'a lost device promised back: ' + gdSt());
      // an error: the device drops while its test colour is on, nobody clicks anything
      STATE.ext.devs[1].online = 1; emit(STATE); await wait(60); const d3 = $('#gd-dev'); d3.value = '2'; d3.dispatchEvent(new Event('change', { bubbles: true })); await wait(20);
      sent.length = 0; gd('check').click(); await wait(1100);
      ok(tests().join() === 'zone.dev2' && gdSt() === t('gd.t.on', 'Board'), 'the test did not start again: ' + tests() + ' / ' + gdSt());
      STATE.ext.devs[1].online = 0; emit(STATE); await wait(60);
      ok(tests().join() === 'zone.dev2,' && !GUIDE.test && gdSt() === t('gd.t.lost'), 'a device lost during its test left the colour on: ' + tests() + ' / ' + gdSt());
      ok(!sent.some(o => o.cmd === 'set' && /^zone\.|^profile\./.test(o.s) || /^profile/.test(o.cmd)), 'the test colour went into the settings: ' + JSON.stringify(sent));
      noHw();
    } finally { await gdBack(k); }
  });

  test('guide with only haku\'s own memory and MSI: found, testable, continue; an unclear or given-back one is explained; none goes on without lights', async () => {
    const k = gdKeep();
    try {
      await fresh(); await gdSet([], 'dev', { sticks: 2, msi: 1, ram_src: [0, 0] });
      ok(gdAny() && gd('next') && gd('next').classList.contains('primary'), 'direct devices not counted as found');
      const txt = $('#guide').textContent;
      ok(txt.includes(t('pc.memory')) && txt.includes(stripName()) && txt.includes(t('ram.src.haku')), 'memory and MSI not listed: ' + txt.slice(0, 200));
      gd('next').click(); await wait(40);
      ok(gdH() === t('gd.test.h') && $('#gd-dev') && [...$('#gd-dev').options].map(o => o.value).join() === 'ram,gpu', 'the test not offered for them: ' + ($('#gd-dev') ? [...$('#gd-dev').options].map(o => o.value) : 'no list'));
      const tests = () => sent.filter(o => o.cmd === 'dev_test').map(o => o.zone);
      sent.length = 0; gd('check').click(); await wait(80);
      ok(tests().join() === 'zone.ram' && gdSt() === t('gd.t.on', `ENE · 2 ${t('chip.pcs')}`), 'Check on memory: ' + tests() + ' / ' + gdSt());
      gd('stop').click(); await wait(20);
      ok(tests().join() === 'zone.ram,' && gdSt() === t('gd.t.back'), 'Stop on memory: ' + tests() + ' / ' + gdSt());
      const sel = $('#gd-dev'); sel.value = 'gpu'; sel.dispatchEvent(new Event('change', { bubbles: true })); await wait(20);
      sent.length = 0; gd('check').click(); await wait(80); gd('works').click(); await wait(40);
      ok(tests().join() === 'zone.gpu,' && gdSt() === t('gd.t.ok'), 'Check on MSI: ' + tests() + ' / ' + gdSt());
      // OpenRGB might own both sticks, and another program has the MSI: not tested, each with its reason and card
      // (one stick haku's own is still tested: the colour goes to that one only, main.c build_scene)
      await gdSet([], 'test', { sticks: 2, msi: 1, msi_own: 1, ram_src: [2, 2] });
      ok(!$('#gd-dev') && !gd('check') && $('#guide').textContent.includes(t('gd.t.none')), 'an unclear device offered for the test');
      const g = $('#guide').textContent;
      ok(g.includes(ramSrcText()) && g.includes(t('ram.src.other')) && $$('#guide [data-g="card"]').length === 2, 'the reasons or cards missing');
      // nothing at all: the search ends empty, and the guide still goes on
      await gdSet([], 'dev'); gd('scan').click(); await wait(400);
      ok(gdSt() === t('gd.d.none') && gd('next') && gd('next').textContent === t('gd.d.go'), 'no way on without lights: ' + gdSt());
      gd('next').click(); await wait(40);
      ok(gdH() === t('gd.test.h') && $('#guide').textContent.includes(t('gd.t.none')) && gd('next'), 'continue without lights did not reach the next step');
      window.mockScanStuck = 1; await gdSet([], 'dev'); gd('scan').click(); await wait(30); GUIDE.scanAt -= 31000; updateGuide();
      ok(gdSt() === t('gd.d.err') && gd('next') && gd('next').textContent === t('gd.d.go'), 'a search error leaves no way on');
      ok(!sent.some(o => o.cmd === 'set' && /^zone\.|^layout$/.test(o.s)), 'the guide changed the lighting settings: ' + JSON.stringify(sent));
      noHw();
    } finally { await gdBack(k); }
  });

  test('guide save step: a new profile only, sent once, the name kept on an error, done only when the core has it; tips', async () => {
    const k = gdKeep(), before = profiles().map(p => p.id + p.name);
    let made = 0;
    try {
      await fresh(); await gdSet(STATE.ext.devs, 'save');
      ok($('#gd-name').value === gdName() && !profiles().some(p => p.name === $('#gd-name').value), 'the suggested name is taken: ' + $('#gd-name').value);
      window.mockSaveFail = 1; $('#gd-name').value = 'My setup'; $('#gd-name').focus();
      key($('#gd-name'), 'Enter'); gd('save').click(); gd('save').click(); await wait(20);
      const saves = sent.filter(o => o.cmd === 'profile_save');
      ok(saves.length === 1 && saves[0].id === '0' && saves[0].name === 'My setup', 'saves sent: ' + JSON.stringify(saves));
      ok(gdSt() === t('gd.s.saving') && gd('save').disabled && !$('#guide').textContent.includes(t('gd.done.h')), 'shown done before the core has it');
      await wait(100);   // the core's refusal (prof.err 1): said at once, not after the wait
      ok(gdSt() === t('prof.err.write') && $('#guide .gd-st.bad') && $('#gd-name').value === 'My setup' && gd('save').textContent === t('gd.s.retry'), 'the refusal: ' + gdSt());
      ok(profiles().length === before.length, 'a profile shows that the core did not save');
      // a core that says nothing: after the wait the same retry
      window.mockSaveFail = 'silent'; gd('save').click(); await wait(100);
      ok(gdSt() === t('gd.s.saving'), 'no answer yet, not saving: ' + gdSt());
      GUIDE.saving.at -= 7000; updateGuide();
      ok(gdSt() === t('gd.s.fail', PROFILE_MAX) && $('#gd-name').value === 'My setup' && gd('save').textContent === t('gd.s.retry'), 'the error: ' + gdSt());
      window.mockSaveFail = 0; gd('save').click(); await wait(200); made = 1;
      ok(gdH() === t('gd.done.h') && $('#guide').textContent.includes(t('gd.done.t', 'My setup')) && cv('general', 'guide') === 'done', 'not done: ' + gdH());
      ok(before.every(x => profiles().some(p => p.id + p.name === x)) && profiles().length === before.length + 1, 'the profiles there were changed');
      ok(document.activeElement === $('#gd-h'), 'the focus is not on the confirmation');
      gd('tip-prof').click(); await wait(20);
      ok(!gd('tip-prof') && document.activeElement === gd('tip-tray'), 'the first tip not gone or the focus lost');
      gd('tip-tray').click(); await wait(20);
      ok($('#guide').classList.contains('hidden') && document.activeElement === $('#prof-btn'), 'the tips did not end it');
      // all profiles taken: nothing is written, only a way to them
      const add = []; for (let n = 1; profiles().length < PROFILE_MAX; n++) if (!S.cfg['profile.' + n]?.name) { STATE.cfg['profile.' + n] = { name: 'P' + n }; add.push(n); emit(STATE); await wait(30); }
      setCfg('general', 'guide', 'save'); updateGuide();
      ok(!$('#gd-name') && gd('prof') && gd('finish') && !gd('save'), 'a save offered with every profile taken');
      add.forEach(n => delete STATE.cfg['profile.' + n]);
    } finally {
      if (made) Object.keys(STATE.cfg).forEach(s => { if (STATE.cfg[s].name === 'My setup' && /^profile\./.test(s)) delete STATE.cfg[s]; });
      await gdBack(k);
    }
  });

  test('guide: every string in every language, the remote window never shows it', async () => {
    const used = new Set((await (await fetch('app.js')).text()).match(/'gd\.[a-z.]*[a-z]'/g).map(s => s.slice(1, -1)));
    for (const s of ['dev', 'path', 'test', 'save', 'done']) { used.add('gd.' + s + '.h'); if (s !== 'done') used.add('gd.' + s + '.t'); }
    ['dark', 'blink', 'other', 'colour'].forEach(p => { used.add('gd.pr.' + p); used.add('gd.pr.' + p + '.t'); });
    const miss = ['en', 'ru', 'fr'].flatMap(l => [...used].filter(x => !STR[l][x]).map(x => l + ':' + x));
    ok(!miss.length, 'missing: ' + miss.join(' '));
    try {
      setCfg('general', 'guide', 'dev'); document.documentElement.classList.add('remote'); updateGuide();
      ok($('#guide').classList.contains('hidden'), 'shown in the remote window');
    } finally { document.documentElement.classList.remove('remote'); setCfg('general', 'guide', ''); updateGuide(); }
  });

  test('devices: one catalogue, each device once in a steady order; search by name or model; no results told from no devices; the filter kept', async () => {
    await fresh(); showTab('devices'); $('#cat-reset').click();
    const all = CATV.length, tiles = () => $$('#cat-grid .cat-tile').length, q = $('#cat-q'), keys = () => CATV.map(x => x.key).join();
    ok(all > 5 && tiles() === all && $('#cat-none').classList.contains('hidden') && $('#cat-nores').classList.contains('hidden'), 'not every device listed: ' + all);
    ok(new Set(CATV.map(x => x.key)).size === all, 'a device listed twice');
    const order = keys(); emit(STATE); await wait(80);
    ok(keys() === order, 'the order moved on an update');
    ok(['Times Gate', 'TimesFrame'].every(n => CATV.some(x => x.name === n && x.brand === 'Divoom')), 'two of one brand merged');
    const bed = CATV.find(x => x.name === 'Bedside');
    ok(bed && bed.state === 'offline' && $(`.cat-tile[data-key="${bed.key}"]`).dataset.st === 'offline', 'a bulb that does not answer left the catalogue');
    ok(CATV.find(x => x.k === 'ram').where === 'pc' && CATV.filter(x => x.k === 'bulb').every(x => x.where === 'room'), 'PC or room wrong');
    q.focus(); q.value = '300183039'; q.dispatchEvent(new Event('input'));
    ok(CATV.length === 1 && CATV[0].name === 'Times Gate' && tiles() === 1 && $('#cat-count').textContent === t('cat.count', 1, all), 'search by model: ' + CATV.map(x => x.name));
    q.value = 'timesframe'; q.dispatchEvent(new Event('input'));
    ok(CATV.length === 1 && CATV[0].name === 'TimesFrame', 'search by name: ' + CATV.map(x => x.name));
    emit(STATE); await wait(80);
    ok(q.value === 'timesframe' && document.activeElement === q && CATV.length === 1, 'an update wiped the search or took the focus');
    showTab('home'); showTab('devices');
    ok(q.value === 'timesframe' && CATV.length === 1 && JSON.parse(localStorage.getItem('cat')).q === 'timesframe', 'the search lost on the way back');
    q.value = 'zzzz'; q.dispatchEvent(new Event('input'));
    ok(!tiles() && !$('#cat-nores').classList.contains('hidden') && $('#cat-none').classList.contains('hidden') && $('#cat-nores-t').textContent === t('cat.nores.t', all), 'no results not told apart from no devices');
    $('#cat-reset').click();
    ok(!q.value && CATV.length === all && document.activeElement === q && $('#cat-nores').classList.contains('hidden'), 'reset did not clear the filters');
    $('#cat-where [data-v="room"]').click();
    ok(CATV.length && CATV.every(x => x.where === 'room') && $('#pc-tools').classList.contains('hidden') && $('#cat-where [data-v="room"]').getAttribute('aria-pressed') === 'true', 'the Room filter');
    $('#cat-where [data-v="pc"]').click();
    ok(CATV.length && CATV.every(x => x.where === 'pc') && !$('#pc-tools').classList.contains('hidden'), 'the PC filter');
    $('#cat-brand').value = 'Divoom'; $('#cat-brand').dispatchEvent(new Event('change'));
    ok(!CATV.length && !$('#cat-nores').classList.contains('hidden'), 'PC and Divoom: ' + CATV.map(x => x.name));
    $('#cat-reset').click();
  });

  test('devices: nothing found at all is its own empty state, with Add and Help', async () => {
    await fresh(); showTab('devices'); $('#cat-reset').click();
    const none = JSON.parse(JSON.stringify(STATE));
    Object.assign(none, { bulbs: [], nano: { ctls: [] }, sticks: 0, msi: null }); none.ext.devs = [];
    emit(none); await wait(80);
    ok(!$('#cat-none').classList.contains('hidden') && $('#cat-nores').classList.contains('hidden') && !$$('#cat-grid .cat-tile').length && !$('#cat-count').textContent && $('#cat-none [data-help="start"]'), 'no devices: ' + $('#cat-count').textContent);
    emit(STATE); await wait(80);
    ok($('#cat-none').classList.contains('hidden') && $$('#cat-grid .cat-tile').length, 'the devices not back');
  });

  test('devices: a tile with its own look goes back to the common lighting, the inheritance only: its own colours stay', async () => {
    await fresh(); showTab('devices'); $('#cat-reset').click();
    const z = 'zone.gpu', keep = { effect: cv(z, 'effect', ''), mode: cv(z, 'mode', ''), palette: cv(z, 'palette', '') }, tile = () => $('.cat-tile[data-key="gpu"]');
    try {
      ok(keep.effect && tile().querySelector('.cat-own.on') && tile().querySelector('.cat-revert'), 'its own effect not shown');
      tile().querySelector('.cat-revert').click();
      ok(cv(z, 'effect', '') === '' && cv(z, 'mode', '') === 'effect' && cv(z, 'palette', '') === keep.palette, 'more than the inheritance changed: ' + cv(z, 'palette', ''));
      ok(!tile().querySelector('.cat-own.on') && !tile().querySelector('.cat-revert') && tile().querySelector('.cat-own').textContent === t('cat.global'), 'the tile still says its own');
    } finally { setCfg(z, 'effect', keep.effect); setCfg(z, 'mode', keep.mode); }
  });

  test('devices: a tile opens its own card in the sheet; on close the card goes home and the focus back to the tile', async () => {
    await fresh(); showTab('devices'); $('#cat-reset').click();
    const b = $('.cat-tile[data-key="ram"] .cat-open'); b.focus(); b.click(); await wait(80);
    ok(!$('#sheet').classList.contains('hidden') && SHEET.kind === 'ram' && $('#sheet-body #ram-card'), 'the tile did not open its card');
    emit(STATE); await wait(80);
    closeSheet(); await wait(20);
    ok(document.activeElement === $('.cat-tile[data-key="ram"] .cat-open'), 'focus not back on the tile: ' + (document.activeElement || {}).className);
    ok($('#ram-card').closest('#dev-store'), 'the card did not go back to its place');
  });

  test('the live colours: asked for only where they are drawn (Home, Devices, a device sheet), a frame at once on return', async () => {
    const live = () => sent.filter(o => o.cmd === 'frames').map(o => o.live).join('');
    try {
      showTab('home'); await fresh();
      for (const p of ['effects', 'settings', 'help', 'fans']) { showTab(p); ok(FRAMES.live === false, p + ': still asks for 20 a second'); }
      ok(live() === '0', 'one message for four pages: ' + live());
      F = { ram: [[], []], gpu: [], board: null, bulbs: [], nano: {}, ext: {} };
      const n = FRAMES.sent; showTab('home');
      ok(FRAMES.live && FRAMES.sent === n + 1 && F.ram[0].length === 8, `Home: no frame at once (${FRAMES.sent - n}, ${F.ram[0].length} RAM LEDs)`);
      showTab('devices'); ok(live() === '01', 'Devices is live too: ' + live());
      // a device sheet over a page that draws nothing has its own live view
      showTab('settings'); openSheet({ k: 'ram' }, 0, 0);
      ok(SHEET.kind === 'ram' && FRAMES.live, 'a RAM sheet over Settings gets no live colours');
      openSheet({ k: 'fx' }, 0, 0); ok(SHEET.kind === 'fx' && !FRAMES.live, 'the effect sheet draws none, yet asks');
      closeSheet(); ok(!FRAMES.live && live() === '01010', 'closed: ' + live());
    } finally { closeSheet(); showTab('home'); }
  });

  test('the nav: six pages, a new brand adds none; its one mark sits on the open page; tiles move only with motion on', async () => {
    await fresh();
    const pages = () => $$('#nav button[data-tab]').map(b => b.dataset.tab).join(), w = motionWas();
    const moving = () => document.getAnimations().some(a => a.effect && a.effect.target && a.effect.target.closest && a.effect.target.closest('#cat-grid'));
    ok(pages() === PAGES.join(), 'nav: ' + pages());
    STATE.ext.devs.push({ id: 77, kind: 'lifx', title: 'LIFX', name: 'Desk LIFX', host: '192.168.1.77', sub: -1, leds: 1, per_led: 0, online: 1, enabled: 1 });
    try {
      emit(STATE); await wait(80);
      ok(pages() === PAGES.join(), 'a brand got a button: ' + pages());
      for (const p of ['effects', 'devices', 'help', 'home']) {
        showTab(p); const b = $(`#nav [data-tab="${p}"]`), n = $('#nav').style;
        ok(n.getPropertyValue('--ny') === b.offsetTop + 'px' && n.getPropertyValue('--nh') === b.offsetHeight + 'px', 'the mark not on ' + p);
      }
      shown(true); motion('0'); showTab('devices'); $('#cat-reset').click();
      $('#cat-where [data-v="room"]').click(); $('#cat-reset').click();
      ok(!moving(), 'tiles move with motion off');
      motion('1'); $('#cat-where [data-v="room"]').click(); $('#cat-reset').click();
      ok(moving(), 'no tile moved with motion on');
    } finally { motionBack(w); STATE.ext.devs.pop(); emit(STATE); await wait(80); }
  });

  test('settings: one category at a time, picked by its tab and kept', async () => {
    await fresh(); showTab('settings');
    for (const v of ['app', 'look', 'control', 'integr', 'data', 'about']) {
      $(`#set-cats button[data-v="${v}"]`).click();
      const on = $$('#tab-settings [data-cat]').filter(c => !c.classList.contains('cat-off'));
      ok(on.length && on.every(c => c.dataset.cat === v) && $(`#set-cats [data-v="${v}"]`).getAttribute('aria-selected') === 'true' && localStorage.getItem('setcat') === v, 'category ' + v);
    }
    setCat('nosuch'); ok($('#set-cats [data-v="app"]').classList.contains('on'), 'an unknown category');
  });

  test('home: Alt+arrows swap a tile with its neighbour, the focus stays on it; an end does not wrap', async () => {
    await fresh(); showTab('home'); await wait(60);
    const was = cv('ui', 'hero_order', ''), keys = () => heroItems('any').map(it => it.key);
    const btn = k => [...$('#hero-keys').children].find(x => x.dataset.k === k && !x.hasAttribute('data-pw'));
    const alt = (el, k) => { const e = new KeyboardEvent('keydown', { key: k, altKey: true, bubbles: true, cancelable: true }); el.dispatchEvent(e); return e; };
    try {
      const k0 = keys(), first = HERO.hits.find(x => k0.includes(x.key)).key;
      btn(first).focus();
      ok(alt(btn(first), 'ArrowLeft').defaultPrevented && keys().join() === k0.join(), 'the first tile moved left');
      alt(btn(first), 'ArrowRight'); await wait(60);
      const k1 = keys();
      ok(k1.join() !== k0.join() && k1.indexOf(first) > k0.indexOf(first) && cv('ui', 'hero_order', '') === k1.join(), 'not swapped: ' + k1);
      ok(document.activeElement === btn(first), 'the focus left the tile: ' + (document.activeElement || {}).dataset?.k);
      alt(btn(first), 'ArrowLeft'); await wait(60);
      ok(keys().join() === k0.join(), 'back: ' + keys());
      ok(!key(btn(first), 'ArrowRight').defaultPrevented && keys().join() === k0.join(), 'an arrow alone moved it');
    } finally { setCfg('ui', 'hero_order', was); showTab('home'); }
  });

  test('a profile picked shows as asked until the core has it on, then as on', async () => {
    await fresh();
    const was = +cv('general', 'profile', 0), to = was === 2 ? 3 : 2; sent.length = 0;
    try {
      profGo(to);
      ok(sent.some(m => m.cmd === 'profile' && m.id === String(to)) && $('#prof-btn').classList.contains('busy') && $('#prof-btn').getAttribute('aria-busy') === 'true', 'not shown as asked');
      await wait(250);
      ok(!$('#prof-btn').classList.contains('busy') && $('#prof-btn').getAttribute('aria-busy') === 'false' && $('#prof-cur').textContent === profCur().name && profCur().id === to, 'not shown as on: ' + $('#prof-cur').textContent);
    } finally { profGo(was); await wait(250); }
  });

  // ---- the effects library: catalogue, search, settings, ready-made looks, the input's state
  const fxOpen = () => { showTab('effects'); FX_TAB = 'fx'; FXQ = ''; buildEffects(); };
  test('effects: each one once in a named group, no empty group, every card with its name, line, preview and needs', async () => {
    await fresh(); fxOpen();
    const cards = $$('#fx-grid .fx[data-id]'), ids = cards.map(b => b.dataset.id);
    ok(new Set(ids).size === ids.length && S.effects.every(e => ids.includes(e.id)), 'cards: ' + ids.join(','));
    for (const id of ['pendulum', 'interference', 'steps', 'twinkle', 'meteor']) ok(ids.includes(id), id + ' missing');
    for (const h of $$('#fx-grid .fx-head')) {
      const n = cards.filter(b => h.dataset.g === 'fav' ? favs().includes(b.dataset.id) : !favs().includes(b.dataset.id) && (FX_GROUP[b.dataset.id] || 'waves') === h.dataset.g).length;
      ok(n > 0 && +h.querySelector('i').textContent === n && !/fx\.g\./.test(h.textContent), 'group ' + h.dataset.g + ': ' + h.textContent + ', ' + n + ' cards');
    }
    for (const b of cards) {
      const id = b.dataset.id, txt = b.querySelector('b').textContent + ' ' + b.querySelector('small').textContent;
      ok(!/(^|\s)(fx|short)\./.test(txt) && b.querySelector('canvas') && !/^desc\./.test(t('desc.' + id)), 'card ' + id + ': ' + txt);
      ok(!!b.querySelector('.tag:not(.demo)') === !!FX_NEEDS[id] && !!b.querySelector('.tag.demo') === !!FX_DEMO[id], 'badges of ' + id);
    }
    ok(fxName('twinkle') === t('fx.twinkle') && fxName('meteor') === t('fx.meteor'), 'names');
  });

  test('effect search: by name or description, empty groups hidden, a line when nothing matches', async () => {
    await fresh(); fxOpen();
    try {
      const q = $('#fx-q'), shown = () => $$('#fx-grid .fx[data-id]:not(.hidden)').map(b => b.dataset.id);
      q.value = fxName('meteor'); q.dispatchEvent(new Event('input', { bubbles: true }));
      ok(shown().includes('meteor') && shown().length < S.effects.length, 'shown: ' + shown());
      ok($$('#fx-grid .fx-head:not(.hidden)').every(h => +h.querySelector('i').textContent > 0), 'an empty group shows');
      ok($('#fx-grid .fx-none').classList.contains('hidden'), 'the nothing-found line shows with results');
      q.value = 'zzqqxx'; q.dispatchEvent(new Event('input', { bubbles: true }));
      ok(!shown().length && !$$('#fx-grid .fx-head:not(.hidden)').length && !$('#fx-grid .fx-none').classList.contains('hidden'), 'nothing found is not said');
    } finally { FXQ = ''; buildEffects(); }
  });

  test('effect settings: the usual ones, rare ones under More, units, choices that show others, a reset', async () => {
    await fresh(); showTab('effects');
    const was = S.effect, box = $('#fx-params');
    try {
      S.effect = 'pendulum'; renderEffectSide();
      const top = [...box.children].filter(e => e.matches('.field, .set-row')).map(e => e.querySelector('[data-k]').dataset.k);
      const adv = [...box.querySelectorAll('details.fx-adv [data-k]')].map(e => e.dataset.k);
      ok(top.join() === 'motion,width,tail,mirror' && [...new Set(adv)].join() === 'axis,soft,bg', 'shown: ' + top + ' | ' + adv);
      ok(/%$/.test(box.querySelector('b[data-k="width"]').textContent) && !/fxp\./.test(box.textContent), 'units or labels: ' + box.textContent);
      ok(box.querySelector('.fx-reset').disabled, 'reset on with nothing set');
      const r = box.querySelector('input[data-k="width"]'); r.value = 40; r.dispatchEvent(new Event('input')); r.dispatchEvent(new Event('change')); flushSets();
      ok(cv('pendulum', 'width', '') === '40' && $('#fx-params b[data-k="width"]').textContent === '40%', 'width not set: ' + cv('pendulum', 'width', ''));
      S.effect = 'meteor'; renderEffectSide(); S.effect = 'pendulum'; renderEffectSide();
      ok(cv('pendulum', 'width', '') === '40' && +$('#fx-params input[data-k="width"]').value === 40, 'lost on an effect switch');
      sent.length = 0; $('#fx-params .fx-reset').click();
      ok(sets('pendulum', 'width').some(o => o.v === '') && cv('pendulum', 'width', '') === '' && $('#fx-params .fx-reset').disabled, 'reset: ' + JSON.stringify(sent));
      // audio: the level's settings, then the beat's
      S.effect = 'audio'; renderEffectSide();
      const ks = () => [...box.querySelectorAll('[data-k]')].map(e => e.dataset.k);
      ok(ks().includes('dir') && ks().includes('peak') && !ks().includes('gap'), 'level: ' + ks());
      box.querySelector('.seg[data-k="mode"] [data-v="1"]').click();
      ok(!ks().includes('peak') && ks().includes('gap') && ks().includes('decay'), 'beat: ' + ks());
      ok(/ms$/.test(box.querySelector('b[data-k="gap"]').textContent), 'gap without ms: ' + box.querySelector('b[data-k="gap"]').textContent);
      box.querySelector('.fx-reset').click();
      // breathe: a cycle in seconds hides the speed
      S.effect = 'breathe'; renderEffectSide();
      ok(!$('#fx-speed-field').classList.contains('hidden') && box.querySelector('b[data-k="cycle"]').textContent === t('fxp.byspeed'), 'cycle 0');
      setCfg('breathe', 'cycle', '8'); renderEffectSide();
      ok($('#fx-speed-field').classList.contains('hidden') && box.querySelector('b[data-k="cycle"]').textContent === '8 ' + t('u.s'), 'cycle 8 s');
      setCfg('breathe', 'cycle', '');
    } finally { S.effect = was; renderEffectSide(); }
  });

  test('ready-made looks: one click puts it on and lights it, an edit makes it the effect again, + saves a new copy', async () => {
    await fresh(); showTab('effects');
    const was = S.effect, br = STATE.brightness, keep = JSON.stringify(S.cfg), before = presets().map(p => p.id);
    try {
      FX_TAB = 'presets'; buildEffects();
      ok($$('#fx-grid .fx[data-builtin]').length === 8 && !/bp\./.test($('#fx-grid').textContent), 'cards: ' + $$('#fx-grid .fx[data-builtin]').length);
      $('#fx-grid .fx[data-builtin="neon"]').click(); await wait(60);
      ok(S.effect === 'pendulum' && cv('pendulum', 'width', '') === '24' && cv('pendulum', 'mirror', '') === '1' && sent.some(o => o.cmd === 'effect' && o.id === 'pendulum'), 'not applied');
      ok(builtinActive() && builtinActive().id === 'neon' && $('#fx-grid .fx[data-builtin="neon"]').classList.contains('on') && $('#fx-name').textContent === t('bp.neon'), 'not lit');
      setCfg('pendulum', 'tail', '70'); renderEffectSide();
      ok(!builtinActive() && !$('#fx-grid .fx[data-builtin="neon"]').classList.contains('on') && $('#fx-name').textContent === fxName('pendulum'), 'still lit after an edit');
      sent.length = 0;
      $('#fx-grid .fx[data-builtin="embers"] .pmenu').click(); await wait(80);
      const ps = sent.filter(o => o.cmd === 'preset_save');
      ok(ps.length === 1 && ps[0].id === '0' && ps[0].effect === 'twinkle', 'save: ' + JSON.stringify(ps));
      const added = presets().filter(p => !before.includes(p.id));
      ok(added.length === 1 && S.cfg['preset.' + added[0].id]['fx.density'] === '30' && before.every(id => S.cfg['preset.' + id].name), 'copy: ' + JSON.stringify(added));
    } finally { STATE.cfg = JSON.parse(keep); STATE.brightness = br; S.effect = was; bridge.postMessage(JSON.stringify({ cmd: 'get' })); await wait(120); FX_TAB = 'fx'; buildEffects(); }
  });

  test('an effect that follows a reading says when there is none: no sound device, silence, the sensor lost', async () => {
    await fresh(); showTab('effects');
    const was = S.effect, st = () => $('#fx-state').classList.contains('hidden') ? '' : $('#fx-state').textContent;
    try {
      S.effect = 'audio'; S.fx_audio = -1; renderEffectSide(); ok(st() === t('fxs.nocap'), 'no capture: ' + st());
      S.fx_audio = 0; updateFxState(); ok(st() === t('fxs.silent'), 'silence: ' + st());
      S.fx_audio = 1; updateFxState(); ok(!st(), 'sound: ' + st());
      S.effect = 'temperature'; S.fx_temp = 0; renderEffectSide(); ok(st() === t('fxs.notemp'), 'lost: ' + st());
      S.fx_temp = 1; updateFxState(); ok(!st(), 'reading: ' + st());
      S.effect = 'flow'; S.fx_temp = 0; renderEffectSide(); ok(!st(), 'shown for another effect');
    } finally { S.fx_audio = S.fx_temp = -2; S.effect = was; renderEffectSide(); }
  });

  // the constructor: [custom] l1..l8 and k1..k3 as effects.c comp_load reads them
  const cmpLines = () => [1, 2, 3, 4, 5, 6, 7, 8].map(i => cv('custom', 'l' + i, '')).filter(Boolean);
  // what is lit: [custom], or the reactor when it is empty (effects.c comp_load)
  const litLines = () => cmpLines().length ? cmpLines() : fxPreview.comp.DEFAULT.map(s => fxPreview.comp.line(fxPreview.comp.parse(s)));
  const cmpKeep = async fn => {
    const was = S.effect, keep = JSON.stringify(S.cfg);
    try { await fn(); } finally {
      if (!$('#cdlg').classList.contains('hidden')) cmpClose(); if (!$('#share-dlg').classList.contains('hidden')) closeShare();
      flushSets(); STATE.cfg = JSON.parse(keep); STATE.effect = S.effect = was; bridge.postMessage(JSON.stringify({ cmd: 'get' })); await wait(120); renderEffectSide();
    }
  };
  // the editor's draft as [lines, knobs]
  const dl = () => JSON.parse(CMP.cur)[0], dk = () => JSON.parse(CMP.cur)[1];
  const tutPick = v => { const s = $('#cdlg-tut'); s.value = v; s.dispatchEvent(new Event('change', { bubbles: true })); };
  const cmpSets = () => sent.filter(o => o.cmd === 'set' && o.s === 'custom' || o.cmd === 'comp_apply');
  test('constructor: add, hide, move and delete layers; its own undo and redo; a draft until Apply, then [custom] at once', async () => {
    await fresh(); showTab('effects');
    await cmpKeep(async () => {
      for (let i = 1; i <= 8; i++) setCfg('custom', 'l' + i, ''); for (let i = 1; i <= 3; i++) setCfg('custom', 'k' + i, '');
      STATE.effect = S.effect = 'custom'; renderEffectSide(); flushSets();
      ok($$('#fx-params input[data-kn]').length === 2 && $('#fx-params button[data-kn]'), 'side panel: the reactor knobs');
      $('#cmp-open').click(); await wait(80); sent.length = 0;
      const li = () => $$('#cdlg-list li');
      ok(li().length === 3 && $('#cdlg-undo').disabled && $('#cdlg-redo').disabled && $('#cdlg-apply').disabled && $('#cdlg-revert').disabled, 'opens on the reactor, nothing to undo or apply');
      $('#cdlg-add').click(); await wait(20);
      ok(li().length === 4 && li()[3].classList.contains('on') && dl().length === 4 && dl()[3].startsWith('s:grad'), 'add: ' + dl().join(' | '));
      ok(dl()[0] === fxPreview.comp.DEFAULT[0] && dk()[2].startsWith('3 n '), 'the reactor whole with its knobs');
      ok(!cmpLines().length && !$('#cdlg-apply').disabled && $('#cdlg-st').classList.contains('draft') && $('#cdlg-st').textContent === t('cmp.st.draft'), 'a draft: the lighting untouched, Apply offered');
      li()[3].querySelector('[data-a="up"]').click(); await wait(20);
      ok(dl()[2].startsWith('s:grad') && dl()[3].startsWith('s:spark') && li()[2].classList.contains('on'), 'up: ' + dl().join(' | '));
      ok(dk()[2].startsWith('4 n '), 'a knob follows its layer when the order changes: ' + dk()[2]);
      li()[0].querySelector('[data-a="eye"]').click(); await wait(20);
      ok(/hide:1/.test(dl()[0]) && li()[0].classList.contains('off'), 'hide: ' + dl()[0]);
      // a setting dragged is one step however many inputs it sends
      li()[2].querySelector('[data-a="pick"]').click(); await wait(20);
      const r = $('#cdlg-edit input[data-p="v"]'); ok(r, 'no speed slider on the gradient');
      for (const v of [30, 45, 60]) { r.value = v; r.dispatchEvent(new Event('input', { bubbles: true })); }
      await wait(300); flushSets();
      ok(/ v:60/.test(dl()[2]), 'drag: ' + dl()[2]);
      li()[3].querySelector('[data-a="del"]').click(); await wait(20);
      ok(dl().length === 3 && !dk()[2], 'delete takes its knob too: ' + dk()[2]);
      const steps = [dl().join('|')], top = UNDO[UNDO.length - 1];
      key(document.body, 'z', true); await wait(20); steps.push(dl().join('|'));
      ok(dl().length === 4 && dk()[2].startsWith('4 n ') && / v:60/.test(dl()[2]), 'undo the delete');
      key(document.body, 'z', true); await wait(20);
      ok(!/ v:60/.test(dl()[2]), 'undo the drag in one step: ' + dl()[2]);
      key(document.body, 'y', true); key(document.body, 'y', true); await wait(20);
      ok(dl().join('|') === steps[0] && $('#cdlg-redo').disabled, 'redo back to the last');
      ok(top && UNDO.includes(top), 'Ctrl+Z in the constructor is its own: the window undo did not run');
      // the shape and the colour: own colour shows a swatch; time and sound open their parts with a visible amount
      const sel = (f, v) => { const s = $(`#cdlg-edit select[data-f="${f}"]`); s.value = v; s.dispatchEvent(new Event('change', { bubbles: true })); };
      sel('shape', 3); await wait(20); ok($('#cdlg-edit input[data-p="dur"]') && $('#cdlg-edit input[type=checkbox][data-p="fade"]'), 'ring settings');
      sel('col', -2); await wait(20); ok($('#cdlg-sw'), 'own colour swatch');
      sel('ap', fxPreview.comp.LPD.findIndex(d => d[0] === 'sz')); await wait(20); ok(/ap:sz/.test(dl()[2]) && /aa:\d/.test(dl()[2]) && $('#cdlg-edit details[data-d="anim"]').open, 'animate: ' + dl()[2]);
      sel('mp', 0); await wait(20); ok(/mp:o/.test(dl()[2]) && /ma:\d/.test(dl()[2]) && $('#cdlg-edit select[data-f="ms"]'), 'sound: ' + dl()[2]);
      ok(!cmpLines().length && !cmpSets().length, 'nothing written while editing: ' + JSON.stringify(cmpSets()));
      // Apply: one comp_apply with all 11 lines for the profile in use; then [custom] is the draft
      $('#cdlg-apply').click(); await wait(10);
      ok($('#cdlg-apply').disabled && $('#cdlg-st').textContent === t('cmp.st.sent'), 'applying: ' + $('#cdlg-st').textContent);
      await wait(120);
      const ca = sent.filter(o => o.cmd === 'comp_apply');
      ok(ca.length === 1 && ca[0].p === cv('general', 'profile', '') && ca[0].kv.split('\n').length === 11 && !sent.some(o => o.cmd === 'set' && o.s === 'custom'), 'one comp_apply: ' + JSON.stringify(cmpSets()));
      ok(cmpLines().join('|') === dl().join('|') && cv('custom', 'k3', '') === '' && $('#cdlg-st').classList.contains('ok') && $('#cdlg-apply').disabled, 'applied: ' + cmpLines().join(' | '));
      // Esc with nothing new closes at once
      key(document.body, 'Escape'); await wait(20);
      ok($('#cdlg').classList.contains('hidden'), 'Esc closes');
    });
  });

  test('constructor: closing with changes asks; Save the draft keeps it in Drafts; Undo the changes is one undo step; errors of Apply', async () => {
    await fresh(); showTab('effects');
    await cmpKeep(async () => {
      try {
        STATE.effect = S.effect = 'custom'; renderEffectSide(); $('#cmp-open').click(); await wait(60);
        const live = cmpLines().join('|'), drafts = () => Object.keys(S.cfg).filter(s => /^cdraft\.\d+$/.test(s) && S.cfg[s].at);
        const n0 = drafts().length;
        tutPick('lights'); await wait(20);
        ok(dl().length === 2 && cmpLines().join('|') === live, 'the tutorial is in the draft only');
        // Esc asks; Esc again goes back to editing; the close button asks again
        key(document.body, 'Escape'); await wait(20);
        ok(!$('#cdlg').classList.contains('hidden') && !$('#cdlg-ask').classList.contains('hidden') && $('#cdlg-row').classList.contains('hidden') && document.activeElement === $('#cdlg-ask-keep'), 'Esc asks first');
        key(document.body, 'Escape'); await wait(20);
        ok($('#cdlg-ask').classList.contains('hidden') && !$('#cdlg-row').classList.contains('hidden') && dl().length === 2, 'Esc again: back to editing');
        $('#cdlg-close').click(); await wait(20); $('#cdlg-ask-back').click(); await wait(20);
        ok(!$('#cdlg').classList.contains('hidden') && $('#cdlg-ask').classList.contains('hidden'), 'Keep editing');
        $('#cdlg-close').click(); await wait(20); $('#cdlg-ask-drop').click(); await wait(40);
        ok($('#cdlg').classList.contains('hidden') && drafts().length === n0 && cmpLines().join('|') === live, 'Don\'t save: closed, no draft, the lighting as it was');
        // Save the draft: kept with the profile it was for, offered in the menu, loads back as a step
        $('#cmp-open').click(); await wait(60); tutPick('lights'); await wait(20);
        $('#cdlg-close').click(); await wait(20); $('#cdlg-ask-keep').click(); await wait(60);
        const D = drafts();
        ok($('#cdlg').classList.contains('hidden') && D.length === n0 + 1 && cmpLines().join('|') === live, 'Save the draft: ' + JSON.stringify(D));
        const sec = D.find(s => /s:dots/.test(S.cfg[s].l2 || ''));
        ok(sec && S.cfg[sec].prof === cv('general', 'profile', '') && S.cfg[sec].name.startsWith(t('cmp.draft.name', '').trim()), 'the draft section: ' + JSON.stringify(S.cfg[sec]));
        $('#cmp-open').click(); await wait(60);
        ok($$('#cdlg-tut optgroup option').some(o => o.value === 'd' + sec.split('.')[1]), 'the draft is in the menu');
        tutPick('d' + sec.split('.')[1]); await wait(20);
        ok(dl().length === 2 && /s:dots/.test(dl()[1]) && !$('#cdlg-undo').disabled && cmpLines().join('|') === live, 'the draft loads into the editor, not the lighting');
        $('#cdlg-revert').click(); await wait(20);
        ok(JSON.stringify(dl()) === JSON.stringify(litLines()) && $('#cdlg-revert').disabled && $('#cdlg-apply').disabled, 'Undo the changes: back to the lighting');
        key(document.body, 'z', true); await wait(20);
        ok(dl().length === 2 && /s:dots/.test(dl()[1]), 'Undo the changes is itself one step');
        // Apply refused: nothing taken, the draft still to apply
        window.mockCompFail = 'bad'; $('#cdlg-apply').click(); await wait(120);
        ok($('#cdlg-st').classList.contains('bad') && $('#cdlg-st').textContent === t('cmp.st.err.bad') && !$('#cdlg-apply').disabled && cmpLines().join('|') === live, 'refused: ' + $('#cdlg-st').textContent);
        // in use but not on disk: said so, not shown as plainly applied
        window.mockCompFail = 'save'; $('#cdlg-apply').click(); await wait(120);
        ok($('#cdlg-st').textContent === t('cmp.st.err.save') && $('#cdlg-st').classList.contains('bad') && cmpLines().length === 2 && $('#cdlg-apply').disabled, 'not saved: ' + $('#cdlg-st').textContent);
        // no answer: said after a while, the draft kept
        tutPick('reactor'); await wait(20);
        window.mockCompFail = 'silent'; $('#cdlg-apply').click(); await wait(4200);
        ok($('#cdlg-st').textContent === t('cmp.st.err.noreply') && !$('#cdlg-apply').disabled && cmpLines().length === 2, 'no answer: ' + $('#cdlg-st').textContent);
        // a reply that comes after the window was closed and opened again is not taken for the new one
        const req = CMP.req || String(cmpReq); cmpClose(); $('#cmp-open').click(); await wait(60);
        emit({ type: 'comp_done', req, err: '' }); await wait(20);
        ok($('#cdlg-st').textContent === '' && $('#cdlg-apply').disabled, 'a late reply did nothing: ' + $('#cdlg-st').textContent);
      } finally { window.mockCompFail = undefined; }
    });
  });

  test('constructor: a profile switch under the editor keeps the draft in Drafts, never applies it; the slider still on its way is dropped', async () => {
    await fresh(); showTab('effects');
    const was = cv('general', 'profile', ''), other = profiles().find(p => String(p.id) !== was);
    await cmpKeep(async () => {
      try {
        STATE.effect = S.effect = 'custom'; renderEffectSide(); $('#cmp-open').click(); await wait(60);
        const live = cmpLines().join('|'), k1 = cv('custom', 'k1', '');
        tutPick('lights'); await wait(20);
        window.mockCompFail = 'silent'; $('#cdlg-apply').click(); await wait(20);
        const req = CMP.req;
        setCfgSoon('custom', 'k1', '9 x 0 1 late');   // a panel knob still waiting to go when the profile changes
        sent.length = 0; window.mockCompFail = undefined;
        bridge.postMessage(JSON.stringify({ cmd: 'profile', id: String(other.id) })); await wait(250);
        ok(cv('general', 'profile', '') === String(other.id) && CMP.prof === String(other.id), 'the editor follows the profile: ' + CMP.prof);
        ok(!$('#cdlg-moved').classList.contains('hidden') && $('#cdlg-moved-text').textContent === t('cmp.moved'), 'said: ' + $('#cdlg-moved-text').textContent);
        const sec = Object.keys(S.cfg).find(s => /^cdraft\.\d+$/.test(s) && S.cfg[s].prof === was && /s:dots/.test(S.cfg[s].l2 || ''));
        ok(sec, 'the draft kept for the old profile: ' + JSON.stringify(Object.keys(S.cfg).filter(s => s.startsWith('cdraft'))));
        ok(!sent.some(o => o.cmd === 'comp_apply' || o.cmd === 'set' && o.s === 'custom' && o.p !== was), 'nothing sent for the new profile\'s [custom]: ' + JSON.stringify(sent.filter(o => o.cmd !== 'get')));
        await wait(100); flushSets();
        ok(cv('custom', 'k1', '') === k1 && cmpLines().join('|') === live, 'the slider and the draft did not reach the new profile');
        ok($('#cdlg-undo').disabled && $('#cdlg-redo').disabled && CMP.req === '', 'no undo, no pending Apply across profiles');
        emit({ type: 'comp_done', req, err: '' }); await wait(20);
        ok(!$('#cdlg-st').classList.contains('ok'), 'the old Apply\'s answer is not taken');
        // the old profile's setting sent late is dropped by the core (ui_web.cpp other_profile) and the page catches up
        bridge.postMessage(JSON.stringify({ cmd: 'set', s: 'custom', k: 'k1', v: 'stale', p: was })); await wait(80);
        ok(cv('custom', 'k1', '') === k1, 'a set for the old profile was taken');
        // the draft stays in the editor until asked: as a preset (nothing applied), or the current profile's composition
        ok(dl().length === 2 && /s:dots/.test(dl()[1]), 'the draft still shown');
        $('#cdlg-moved-preset').click(); await wait(60);
        ok(!$('#pdlg').classList.contains('hidden') && $('#cdlg').classList.contains('hidden'), 'the preset window');
        sent.length = 0; $('#pdlg-form').requestSubmit(); await wait(120);
        const ps = sent.find(o => o.cmd === 'preset_save');
        ok(ps && ps.apply === '0' && /l2=s:dots/.test(ps.fxkv) && cmpLines().join('|') === live, 'saved as a preset, not applied: ' + JSON.stringify(ps));
        ok(!$('#cdlg').classList.contains('hidden') && $('#cdlg-st').textContent.includes(ps.name), 'back in the editor, said: ' + $('#cdlg-st').textContent);
        $('#cdlg-moved-open').click(); await wait(20);
        ok($('#cdlg-moved').classList.contains('hidden') && dl().join('|') === litLines().join('|') && $('#cdlg-apply').disabled, 'the current profile\'s composition');
      } finally { window.mockCompFail = undefined; }
    });
  });

  const draftSecs = () => Object.keys(S.cfg).filter(s => /^cdraft\.\d+$/.test(s) && S.cfg[s].at);
  const dsaves = slot => sent.filter(o => o.cmd === 'draft_save' && o.slot === slot);
  test('constructor drafts: Save the draft closes only once the core wrote it; a failed write keeps the work with Try again and Export; drafts full; no answer; a late answer', async () => {
    await fresh(); showTab('effects');
    await cmpKeep(async () => {
      try {
        STATE.effect = S.effect = 'custom'; renderEffectSide(); $('#cmp-open').click(); await wait(60);
        const live = cmpLines().join('|'), n0 = draftSecs().length;
        tutPick('lights'); await wait(20);
        // the disk refuses: the window stays with the work, the button says Try again, Export is offered
        window.mockDraftFail = 'save'; sent.length = 0;
        $('#cdlg-close').click(); await wait(20); $('#cdlg-ask-keep').click(); await wait(5);
        ok($('#cdlg-ask-keep').disabled && $('#cdlg-ask-text').textContent === t('cmp.draft.saving'), 'saving said: ' + $('#cdlg-ask-text').textContent);
        await wait(80);
        const d0 = dsaves('');
        ok(d0.length === 1 && d0[0].kv.split('\n').length === 14 && !sent.some(o => o.cmd === 'set' && /^cdraft/.test(o.s)), 'one draft_save, no loose sets: ' + JSON.stringify(sent.map(o => o.cmd)));
        ok(!$('#cdlg').classList.contains('hidden') && !$('#cdlg-ask').classList.contains('hidden') && $('#cdlg-ask-text').textContent === t('cmp.draft.fail'), 'not written, the window stays: ' + $('#cdlg-ask-text').textContent);
        ok(!$('#cdlg-ask-keep').disabled && $('#cdlg-ask-keep').textContent === t('cmp.draft.retry') && !$('#cdlg-ask-export').classList.contains('hidden'), 'Try again and Export offered');
        ok(draftSecs().length === n0 && dl().length === 2 && /s:dots/.test(dl()[1]) && cmpLines().join('|') === live, 'no draft, the work still in the editor, nothing lit');
        // Export: the share window with the work in it; back to the editor when it closes
        $('#cdlg-ask-export').click(); await wait(40);
        ok(!$('#share-dlg').classList.contains('hidden') && $('#cdlg').classList.contains('hidden'), 'the share window');
        closeShare(); await wait(40);
        ok(!$('#cdlg').classList.contains('hidden') && dl().length === 2 && $('#cdlg-ask').classList.contains('hidden'), 'back in the editor with the work');
        // Try again with the disk back: written, then closed
        window.mockDraftFail = undefined;
        $('#cdlg-close').click(); await wait(20);
        ok($('#cdlg-ask-keep').textContent === t('cmp.ask.keep') && $('#cdlg-ask-export').classList.contains('hidden'), 'the question asked afresh');
        $('#cdlg-ask-keep').click(); await wait(80);
        ok($('#cdlg').classList.contains('hidden') && draftSecs().length === n0 + 1, 'written, then closed: ' + JSON.stringify(draftSecs()));
        // it is in the settings the core keeps: a page loaded again still offers it
        const sec = draftSecs().find(s => /s:dots/.test(S.cfg[s].l2 || ''));
        bridge.postMessage(JSON.stringify({ cmd: 'hello' })); await wait(80);
        $('#cmp-open').click(); await wait(60);
        ok(sec && $$('#cdlg-tut optgroup option').some(o => o.value === 'd' + sec.split('.')[1]), 'offered after a reload: ' + sec);
        // drafts full: never one overwritten, the window stays
        for (let n = 1; n <= 16; n++) if (!(STATE.cfg['cdraft.' + n] || {}).at) STATE.cfg['cdraft.' + n] = { name: 'mine ' + n, at: String(1000 + n), prof: '' };
        const before = JSON.stringify(Object.keys(STATE.cfg).filter(s => /^cdraft/.test(s)).map(s => STATE.cfg[s]));
        bridge.postMessage(JSON.stringify({ cmd: 'get' })); await wait(80);
        tutPick('lights'); await wait(20);
        $('#cdlg-close').click(); await wait(20); $('#cdlg-ask-keep').click(); await wait(80);
        ok(!$('#cdlg').classList.contains('hidden') && $('#cdlg-ask-text').textContent === t('cmp.draft.full') && $('#cdlg-ask-export').classList.contains('hidden'), 'full: ' + $('#cdlg-ask-text').textContent);
        ok(JSON.stringify(Object.keys(STATE.cfg).filter(s => /^cdraft/.test(s)).map(s => STATE.cfg[s])) === before, 'none overwritten');
        // no answer: said after 4 s, the work kept; the answer that comes after is not taken for a new try
        $('#cdlg-ask-back').click(); await wait(20);
        for (let n = 1; n <= 16; n++) if (/^mine /.test((STATE.cfg['cdraft.' + n] || {}).name || '')) delete STATE.cfg['cdraft.' + n];
        bridge.postMessage(JSON.stringify({ cmd: 'get' })); await wait(80);
        window.mockDraftFail = 'silent'; sent.length = 0;
        $('#cdlg-close').click(); await wait(20); $('#cdlg-ask-keep').click(); await wait(4200);
        ok(!$('#cdlg').classList.contains('hidden') && $('#cdlg-ask-text').textContent === t('cmp.draft.fail') && !$('#cdlg-ask-keep').disabled, 'no answer: ' + $('#cdlg-ask-text').textContent);
        const late = dsaves('')[0].req;
        emit({ type: 'draft_done', req: late, err: '', n: 3 }); await wait(20);
        ok(!$('#cdlg').classList.contains('hidden') && $('#cdlg-ask-text').textContent === t('cmp.draft.fail'), 'the late answer changed nothing');
        // a refused document is a failure too
        $('#cdlg-ask-keep').click(); await wait(20);
        emit({ type: 'draft_done', req: dsaves('')[1].req, err: 'bad', n: 0 }); await wait(20);
        ok(!$('#cdlg').classList.contains('hidden') && $('#cdlg-ask-text').textContent === t('cmp.draft.fail'), 'refused: ' + $('#cdlg-ask-text').textContent);
      } finally { window.mockDraftFail = undefined; }
    });
  });

  test('constructor drafts: a profile switch during Save the draft keeps it once; a failed write there offers Try again', async () => {
    await fresh(); showTab('effects');
    const was = cv('general', 'profile', ''), other = profiles().find(p => String(p.id) !== was);
    await cmpKeep(async () => {
      try {
        STATE.effect = S.effect = 'custom'; renderEffectSide(); $('#cmp-open').click(); await wait(60);
        tutPick('lights'); await wait(20);
        window.mockDraftFail = 'silent'; sent.length = 0;
        $('#cdlg-close').click(); await wait(20); $('#cdlg-ask-keep').click(); await wait(20);
        bridge.postMessage(JSON.stringify({ cmd: 'profile', id: String(other.id) })); await wait(250);
        ok(dsaves('').length === 1, 'one save for both: ' + dsaves('').length);
        ok($('#cdlg-ask').classList.contains('hidden') || $('#cdlg-ask-keep').disabled, 'the question waits for the save');
        ok(!$('#cdlg-moved').classList.contains('hidden') && $('#cdlg-moved-text').textContent === t('cmp.moved.saving'), 'saving said: ' + $('#cdlg-moved-text').textContent);
        // the answer comes: kept once, both told
        const o = dsaves('')[0], L = o.kv.split('\n').map(l => [l.slice(0, l.indexOf('=')), l.slice(l.indexOf('=') + 1)]);
        STATE.cfg['cdraft.9'] = Object.fromEntries(L); emit(STATE); emit({ type: 'draft_done', req: o.req, err: '', n: 9 }); await wait(40);
        ok($('#cdlg-moved-text').textContent === t('cmp.moved') && $('#cdlg-ask').classList.contains('hidden') && !$('#cdlg').classList.contains('hidden'), 'kept, the editor stays on the new profile: ' + $('#cdlg-moved-text').textContent);
        ok(S.cfg['cdraft.9'].prof === was && /s:dots/.test(S.cfg['cdraft.9'].l2) && !sent.some(x => x.cmd === 'comp_apply'), 'marked with the old profile, never applied');
        // back to the first profile with the work still unapplied: the disk refuses, Try again writes it
        window.mockDraftFail = 'save';
        bridge.postMessage(JSON.stringify({ cmd: 'profile', id: was })); await wait(250);
        ok($('#cdlg-moved-text').textContent === t('cmp.moved.fail') && !$('#cdlg-moved-retry').classList.contains('hidden') && /s:dots/.test(dl()[1] || ''), 'not written, said, the work kept: ' + $('#cdlg-moved-text').textContent);
        window.mockDraftFail = undefined; $('#cdlg-moved-retry').click(); await wait(80);
        ok($('#cdlg-moved-text').textContent === t('cmp.moved') && $('#cdlg-moved-retry').classList.contains('hidden'), 'Try again: kept');
      } finally { window.mockDraftFail = undefined; }
    });
  });

  test('constructor recovery: the unapplied work is copied 3 s after a change; offered after an unexpected end, restored as a step, never applied; Delete; a write error said', async () => {
    await fresh(); showTab('effects');
    await cmpKeep(async () => {
      try {
        STATE.effect = S.effect = 'custom'; renderEffectSide(); $('#cmp-open').click(); await wait(60);
        const live = cmpLines().join('|');
        ok($('#cdlg-rec').classList.contains('hidden'), 'nothing to offer');
        sent.length = 0; tutPick('lights'); await wait(1000);
        ok(!dsaves('r').length, 'not at once');
        await wait(2300);
        const r = dsaves('r');
        ok(r.length === 1 && /l2=s:dots/.test(r[0].kv) && S.cfg.crecover && /s:dots/.test(S.cfg.crecover.l2), 'the copy written: ' + JSON.stringify(r));
        ok(!sent.some(o => o.cmd === 'comp_apply' || o.cmd === 'set' && o.s === 'custom') && cmpLines().join('|') === live, 'nothing applied');
        await wait(3200); ok(dsaves('r').length === 1, 'not written again while unchanged');
        // the window goes without a close (the app ended): the next editor offers it
        CMP.tok++; clearTimeout(CMP.rt); $('#cdlg').classList.add('hidden');
        $('#cmp-open').click(); await wait(60);
        ok(!$('#cdlg-rec').classList.contains('hidden') && $('#cdlg-rec-text').textContent.length > 10 && dl().join('|') === litLines().join('|'), 'offered, not loaded by itself: ' + $('#cdlg-rec-text').textContent);
        // while it is not answered, its copy stays as it was
        sent.length = 0; tutPick('reactor'); await wait(3300);
        ok(!dsaves('r').length && /s:dots/.test(S.cfg.crecover.l2), 'the copy kept while asked');
        $('#cdlg-rec-open').click(); await wait(20);
        ok($('#cdlg-rec').classList.contains('hidden') && dl().length === 2 && /s:dots/.test(dl()[1]) && !$('#cdlg-undo').disabled, 'restored as a step: ' + dl().join(' | '));
        ok(!sent.some(o => o.cmd === 'comp_apply' || o.cmd === 'set' && o.s === 'custom') && cmpLines().join('|') === live, 'restored, not applied');
        // a close the user chose clears it
        sent.length = 0; $('#cdlg-close').click(); await wait(20); $('#cdlg-ask-drop').click(); await wait(80);
        ok(dsaves('r').some(o => o.kv === '') && !S.cfg.crecover, 'cleared on a close: ' + JSON.stringify(S.cfg.crecover));
        // Delete
        STATE.cfg.crecover = { name: 'x', at: String(Date.now()), prof: '', l1: 's:dots c:p' }; bridge.postMessage(JSON.stringify({ cmd: 'get' })); await wait(80);
        $('#cmp-open').click(); await wait(60);
        ok(!$('#cdlg-rec').classList.contains('hidden'), 'offered again');
        $('#cdlg-rec-drop').click(); await wait(80);
        ok($('#cdlg-rec').classList.contains('hidden') && !S.cfg.crecover && dl().join('|') === litLines().join('|'), 'deleted');
        // the copy cannot be written: said in the status, the work stays
        window.mockDraftFail = 'save'; tutPick('lights'); await wait(3300);
        ok($('#cdlg-st').textContent.includes(t('cmp.rec.fail')) && dl().length === 2, 'said: ' + $('#cdlg-st').textContent);
      } finally { window.mockDraftFail = undefined; }
    });
  });

  test('constructor: the tutorials are plain layers; a panel knob moves its layer\'s setting; Save as preset sends the layers', async () => {
    await fresh(); showTab('effects');
    await cmpKeep(async () => {
      STATE.effect = S.effect = 'custom'; renderEffectSide(); $('#cmp-open').click(); await wait(60);
      tutPick('lights'); await wait(20);
      ok(dl().length === 2 && dl()[0].startsWith('s:grad') && /s:dots.*b:mask/.test(dl()[1]), 'lights: ' + dl().join(' | '));
      ok(dk()[1] === '2 n 4 30 ' + t('cmp.k.lights'), 'knob: ' + dk()[1]);
      tutPick('reactor'); await wait(20);
      $('#cdlg-apply').click(); await wait(120);
      ok(JSON.stringify(litLines()) === JSON.stringify(fxPreview.comp.DEFAULT.map(s => fxPreview.comp.line(fxPreview.comp.parse(s)))), 'reactor: ' + litLines().join(' | '));
      $('#cdlg-close').click(); await wait(20);
      ok($('#cdlg').classList.contains('hidden'), 'applied: Close closes without asking');
      // Sparks 1..30: the slider at 100 is 30 sparks
      const r = $('#fx-params input[data-kn="2"]'); r.value = 100; r.dispatchEvent(new Event('input', { bubbles: true })); await wait(300); flushSets();
      ok(/ n:30/.test(cv('custom', 'l3', '')) && $('#fx-params b[data-kn="2"]').textContent === '30', 'knob: ' + cv('custom', 'l3', ''));
      sent.length = 0; $('#cmp-open').click(); await wait(40); $('#cdlg-save').click(); await wait(60);
      ok($('#cdlg').classList.contains('hidden') && !$('#pdlg').classList.contains('hidden'), 'the preset window');
      $('#pdlg-form').requestSubmit(); await wait(80);
      const ps = sent.filter(o => o.cmd === 'preset_save');
      ok(ps.length === 1 && ps[0].effect === 'custom' && ps[0].id === '0' && ps[0].fx === '' && ps[0].apply === '1' && ps[0].fxkv.split('\n').length === 11, 'save (nothing unapplied: the preset in use): ' + JSON.stringify(ps));
      const P = presets().filter(p => p.effect === 'custom').pop();
      ok(P && / n:30/.test(cv('preset.' + P.id, 'fx.l3', '')), 'the preset keeps the layers');
      ok(!$('#cdlg').classList.contains('hidden') && $('#cdlg-st').textContent.includes(P.name), 'back in the constructor: ' + $('#cdlg-st').textContent);
      // a draft saved as a preset: the preset has the draft, the lighting does not
      const live = cmpLines().join('|'); tutPick('lights'); await wait(20);
      sent.length = 0; $('#cdlg-save').click(); await wait(60); $('#pdlg-form').requestSubmit(); await wait(120);
      ok(sent.filter(o => o.cmd === 'preset_save').map(o => o.apply).join() === '0', 'a draft saved as a preset was applied');
      const Q =presets().filter(p => p.effect === 'custom').pop();
      ok(Q && Q.id !== P.id && /s:dots/.test(cv('preset.' + Q.id, 'fx.l2', '')) && !cv('preset.' + Q.id, 'fx.l3', '') && cmpLines().join('|') === live && dl().length === 2, 'the draft as a preset, the lighting kept');
      cmpClose();
      // the preset showing, saved again: the core is told to take the layers as they are now
      sent.length = 0; presetDialog(P.id, P.name); await wait(40); $('#pdlg-form').requestSubmit(); await wait(60);
      ok(sent.some(o => o.cmd === 'preset_save' && o.id === String(P.id) && o.fx === 'cur'), 'resave: ' + JSON.stringify(sent.filter(o => o.cmd === 'preset_save')));
    });
  });

  test('constructor codes: the layers travel in haku:, a bad layer refuses the code, the community is not offered', async () => {
    await fresh(); showTab('effects');
    await cmpKeep(async () => {
      for (let i = 1; i <= 8; i++) setCfg('custom', 'l' + i, ''); for (let i = 1; i <= 3; i++) setCfg('custom', 'k' + i, '');
      STATE.effect = S.effect = 'custom'; renderEffectSide(); $('#cmp-open').click(); await wait(60);
      tutPick('lights'); await wait(20);
      $('#cdlg-export').click(); await wait(60);
      const code = $('#share-code').value;
      ok(!$('#share-dlg').classList.contains('hidden') && $('#pub-row').classList.contains('hidden') && $('#share-note').textContent.includes(t('cmp.comm')), 'share window, no Publish');
      const p = presetFromCode(code);
      ok(p && p.effect === 'custom' && p.comp.l.length === 2 && /s:dots/.test(p.comp.l[1]) && p.comp.k[1].startsWith('2 n 4 30'), 'round trip: ' + JSON.stringify(p));
      const raw = o => 'haku:' + b64u(JSON.stringify(o)), base = { v: 1, n: 'x', e: 'custom', p: ['#FF0000'] };
      ok(!presetFromCode(raw({ ...base, c: { l: ['s:grad', 'x:5'], k: [] } })), 'a layer with no shape is refused');
      ok(!presetFromCode(raw({ ...base, c: { l: ['s:grad; s:fill'], k: [] } })), 'a ; in a line is refused');
      ok(!presetFromCode(raw({ ...base, c: { l: Array(9).fill('s:fill'), k: [] } })), 'nine layers are refused');
      ok(!presetFromCode(raw({ ...base, c: { l: ['s:fill'], k: ['2 o 0 100'] } })), 'a knob on a missing layer is refused');
      ok(!presetFromCode(raw(base)), 'a constructor code without layers is refused');
      // strict: what is not known or out of range refuses the whole code, a newer version is said so
      const why = o => codeRead(typeof o === 'string' ? o : raw(o)).err;
      ok(why('haku:' + 'A'.repeat(20000)) === 'bad', 'an oversized code is not decoded');
      ok(why({ ...base, v: 2, c: { l: ['s:fill'], k: [] } }) === 'new', 'a newer version is said as one');
      ok(why({ ...base, c: { l: ['s:ring x:900'], k: [] } }) === 'bad', 'a number out of range');
      ok(why({ ...base, c: { l: ['s:ring zz:1'], k: [] } }) === 'bad', 'an unknown layer word');
      ok(why({ ...base, c: { l: ['s:ring x:1e3'], k: [] } }) === 'bad' && why({ ...base, c: { l: ['s:ring x:NaN'], k: [] } }) === 'bad', 'not a plain number');
      ok(why({ ...base, c: { l: ['s:ring s:fill'], k: [] } }) === 'bad', 'two shapes in a layer');
      ok(why({ ...base, s: '5', c: { l: ['s:fill'], k: [] } }) === 'bad' && why({ ...base, s: 99, c: { l: ['s:fill'], k: [] } }) === 'bad', 'a speed as text or out of range');
      ok(why({ v: 1, n: 'x', e: 'rainbow', c: { l: ['s:fill'], k: [] } }) === 'bad', 'layers on an effect that has none');
      ok(why({ v: 1, n: 'x', e: 'rainbow', p: ['red'] }) === 'bad' && why([1, 2]) === 'bad' && why('haku:%%') === 'bad', 'a bad colour, an array, not base64');
      ok(!why({ v: 1, n: 'old', e: 'rainbow', p: ['#FF0000', '#00FF00'], s: 7 }) && !why(code), 'an old plain code and this one still read');
      closeShare(); await wait(60);
      ok(!$('#cdlg').classList.contains('hidden') && dl().length === 2, 'the constructor back with its draft');
      cmpClose();
      // importing: a new preset with the layers in it, not applied: the lit composition and the effect stay
      for (let i = 1; i <= 8; i++) setCfg('custom', 'l' + i, ''); flushSets();
      const fx0 = STATE.effect;
      sent.length = 0; shareDialog(null); $('#share-code').value = code; $('#share-form').requestSubmit(); await wait(80);
      const ps = sent.filter(o => o.cmd === 'preset_save');
      ok(ps.length === 1 && ps[0].effect === 'custom' && ps[0].apply === '0' && /l2=s:dots/.test(ps[0].fxkv) && !sent.some(o => o.cmd === 'set' && o.s === 'custom'), 'import: ' + JSON.stringify(sent));
      ok(!cmpLines().length && STATE.effect === fx0, 'the lighting untouched');
      ok($('#share-msg').textContent === t('import.done', p.name) && !$('#share-code').value, 'said: ' + $('#share-msg').textContent);
      const P = presets().filter(x => x.effect === 'custom').pop();
      ok(P && /s:dots/.test(cv('preset.' + P.id, 'fx.l2', '')), 'the imported preset has the layers');
      sent.length = 0; $('#share-code').value = raw({ ...base, v: 3, c: { l: ['s:fill'], k: [] } }); $('#share-form').requestSubmit(); await wait(40);
      ok(!sent.length && $('#share-msg').textContent === t('import.new'), 'a newer code: nothing saved, said why');
      closeShare(); await wait(40);
    });
  });

  test('constructor layers: names travel and are escaped, duplicate, solo only in the preview, dead bindings said, undo by gesture, the preview clock', async () => {
    await fresh(); showTab('effects');
    await cmpKeep(async () => {
      for (let i = 1; i <= 8; i++) setCfg('custom', 'l' + i, ''); for (let i = 1; i <= 3; i++) setCfg('custom', 'k' + i, '');
      STATE.effect = S.effect = 'custom'; renderEffectSide(); flushSets();
      $('#cmp-open').click(); await wait(80); sent.length = 0;
      const fxc = fxPreview.comp, li = () => $$('#cdlg-list li');
      const nm = $('#cdlg-edit input[data-f="name"]'); ok(nm && nm.placeholder === t('cmp.s.' + fxc.SHAPES[CMP.L[0].shape]), 'a name field, the shape as its placeholder');
      nm.value = '<b>Ring;#[x]=1</b>'; nm.dispatchEvent(new Event('change', { bubbles: true })); await wait(20);
      ok(!/[;#\[\]=]/.test(CMP.L[0].name) && / nm:\S+$/.test(dl()[0]) && !li()[0].querySelector('b'), 'cleaned and escaped: ' + dl()[0]);
      const back = fxc.parse(dl()[0]); ok(back.name === CMP.L[0].name && fxc.line(back) === dl()[0], 'the name reads back from the line');
      ok(fxc.parse('s:ring c:p1 x:20').name === '' && !/nm:/.test(fxc.line(fxc.parse('s:ring c:p1 x:20'))), 'an old line without a name stays without');
      const long = fxc.parse(fxc.line(CMP.L[0])); long.name = 'x'.repeat(40); ok(fxc.line(long).length <= 240 && fxc.parse(fxc.line(long)).name.length === 24, 'a name is cut to 24');
      // duplicate: a copy under it with the same name, one undo step
      const n0 = dl().length; li()[0].querySelector('[data-a="dup"]').click(); await wait(20);
      ok(dl().length === n0 + 1 && dl()[1] === dl()[0] && li()[1].classList.contains('on'), 'duplicate: ' + dl().slice(0, 2).join(' | '));
      key(document.body, 'z', true); await wait(20); ok(dl().length === n0, 'undo the duplicate');
      key(document.body, 'y', true); await wait(20);
      // the name in a draft and in a code
      $('#cdlg-export').click(); await wait(60); const code = $('#share-code').value; closeShare(); await wait(60);
      const pc = presetFromCode(code); ok(pc && fxc.parse(pc.comp.l[0]).name === CMP.L[0].name, 'the name in a haku: code');
      // solo: the preview only, no undo step, never sent
      const cur = CMP.cur, h = CMP.hist.length; li()[2].querySelector('[data-a="solo"]').click(); await wait(20);
      ok(CMP.solo === CMP.L[2] && li()[2].querySelector('[data-a="solo"]').getAttribute('aria-pressed') === 'true' && CMP.cur === cur && CMP.hist.length === h, 'solo is not an edit');
      ok($('#cdlg-cap').textContent.includes(t('cmp.solo.on', 3)) && $('#cdlg-cap').textContent.includes(t('cmp.view.model')), 'said: ' + $('#cdlg-cap').textContent);
      const shown = cmpShow(); ok(shown.length === CMP.L.length && shown.filter(L => L.p[CMP_LP.hide] < 0.5).length === 1 && shown[2].p[CMP_LP.hide] < 0.5, 'the others hidden in the preview');
      li()[2].querySelector('[data-a="solo"]').click(); await wait(20); ok(!CMP.solo, 'solo off');
      ok(!cmpSets().length, 'nothing sent by names, duplicate or solo: ' + JSON.stringify(cmpSets()));
      // a mask in black and white
      tutPick('lights'); await wait(20); li()[1].querySelector('[data-a="pick"]').click(); await wait(20);
      $('#cdlg-edit [data-a="maskview"]').click(); await wait(20);
      ok(CMP.solo === CMP.L[1] && cmpShow().length === 3 && $('#cdlg-cap').textContent.includes(t('cmp.mask.bw')), 'mask view: ' + $('#cdlg-cap').textContent);
      // only the settings the shape reads; one chosen before a shape change is said to do nothing
      const sel = (f, v) => { const x = $(`#cdlg-edit select[data-f="${f}"]`); x.value = v; x.dispatchEvent(new Event('change', { bubbles: true })); };
      sel('shape', fxc.SHAPES.indexOf('ring')); await wait(20);
      const dur = fxc.LPD.findIndex(d => d[0] === 'dur'), opts = () => [...$('#cdlg-edit select[data-f="ap"]').options].map(o => +o.value);
      ok(opts().includes(dur), 'a ring offers its duration'); sel('ap', dur); await wait(20);
      sel('shape', fxc.SHAPES.indexOf('fill')); await wait(20);
      ok(opts().includes(dur) && $('#cdlg-edit .cdlg-dead') && $('#cdlg-edit .cdlg-dead').textContent.includes(t('cmp.dead.note')), 'the dead binding stays and is said');
      ok(!opts().includes(fxc.LPD.findIndex(d => d[0] === 'w')) || CMP_P.fill.includes('w'), 'a setting the fill does not read is not offered');
      // undo by gesture: a pointer drag is one step, two key presses are two, a new change clears redo
      li()[0].querySelector('[data-a="pick"]').click(); await wait(20);
      const r = $('#cdlg-edit input[data-p="o"]'), inp = v => { r.value = v; r.dispatchEvent(new Event('input', { bubbles: true })); };
      const h0 = CMP.hist.length;
      r.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true })); inp(20); inp(40); inp(60); r.dispatchEvent(new PointerEvent('pointerup', { bubbles: true }));
      ok(CMP.hist.length === h0 + 1, 'one drag, one step: ' + (CMP.hist.length - h0));
      for (const v of [61, 62]) { r.dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true })); inp(v); r.dispatchEvent(new KeyboardEvent('keyup', { key: 'ArrowRight', bubbles: true })); }
      ok(CMP.hist.length === h0 + 3, 'two key presses, two steps: ' + (CMP.hist.length - h0));
      r.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true })); inp(70); window.dispatchEvent(new Event('blur')); inp(80);
      ok(CMP.hist.length === h0 + 5, 'leaving the window ends a gesture: ' + (CMP.hist.length - h0));
      key(document.body, 'z', true); await wait(20); ok(!$('#cdlg-redo').disabled, 'redo offered');
      const r2 = $('#cdlg-edit input[data-p="o"]');   // the undo drew the box again
      r2.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true })); r2.value = 10; r2.dispatchEvent(new Event('input', { bubbles: true })); ok($('#cdlg-redo').disabled && !CMP.fut.length, 'a new change clears redo');
      // the text field keeps its own undo
      const nm2 = $('#cdlg-edit input[data-f="name"]'), hh = CMP.hist.length; key(nm2, 'z', true); await wait(20); ok(CMP.hist.length === hh, 'Ctrl+Z in the name field is the field\'s');
      // the preview: paused and scrubbed, the same picture twice; the speed is not saved
      const pix = () => { cmpDraw(); return Array.from($('#cdlg-view').getContext('2d').getImageData(0, 0, $('#cdlg-view').width, $('#cdlg-view').height).data).join(); };
      if (CMP.play) $('#cdlg-pp').click(); ok(!CMP.play && $('#cdlg-pp').getAttribute('aria-pressed') === 'false' && $('#cdlg-pp').title === t('cmp.play'), 'paused');
      const tr = $('#cdlg-t'); tr.value = 5; tr.dispatchEvent(new Event('input', { bubbles: true })); await wait(80);
      const a = pix(); await wait(120); const b = pix(); ok(a === b && CMP.t === 5 && $('#cdlg-tv').textContent === '5.0', 'scrubbed to 5: the same frame');
      $('#cdlg-rs').click(); ok(CMP.t === 0 && $('#cdlg-tv').textContent === '0.0', 'from the start');
      tr.value = 5; tr.dispatchEvent(new Event('input', { bubbles: true })); ok(pix() === a, 'back at 5: the same frame again');
      // paused: no frames until what they show changes, then one (the loop driven by hand: a background page gets no rAF)
      let T = performance.now() + 1e4; const tick = () => { cancelAnimationFrame(CMP.raf); cmpPreview(T += 40); };
      tick(); let d0 = CMP.draws; tick(); tick(); tick(); ok(CMP.draws === d0, 'paused and unchanged: not drawn again: ' + (CMP.draws - d0));
      const r3 = $('#cdlg-edit input[data-p="o"]'); r3.value = 20; r3.dispatchEvent(new Event('input', { bubbles: true })); tick(); tick();
      ok(CMP.draws === d0 + 1, 'an edit on a pause: drawn once: ' + (CMP.draws - d0));
      // paused, the loop itself stops (no frame asked for) and an edit or a resize asks for exactly one
      tick(); ok(!CMP.raf, 'paused and unchanged: the loop still runs');
      r3.value = 25; r3.dispatchEvent(new Event('input', { bubbles: true })); const w1 = CMP.raf; cmpWake(); cmpWake();
      ok(w1 && CMP.raf === w1, 'an edit on a pause did not ask for one frame, or asked for several');
      cancelAnimationFrame(CMP.raf); CMP.raf = 0; dispatchEvent(new Event('resize')); ok(CMP.raf, 'a resize on a pause asks for no frame');
      tick(); ok(!CMP.raf, 'after that one frame the loop did not stop');
      $('#cdlg-big').click(); ok(CMP.raf && $('#cdlg-view').width === 96, 'the enlarged view asks for no frame'); tick(); $('#cdlg-big').click(); tick();
      $('#cdlg-ab').click(); ok(CMP.raf || !cmpDirty(), 'Before asks for no frame'); tick(); if (CMP.ab) $('#cdlg-ab').click(); tick();
      ok(!CMP.raf, 'paused: a loop left running');
      d0 = CMP.draws; $('#cdlg-pp').click(); tick(); tick(); tick(); ok(CMP.draws - d0 === 3, 'playing: drawn each frame: ' + (CMP.draws - d0)); $('#cdlg-pp').click();
      const rt = $('#cdlg-rate'); rt.value = '2'; rt.dispatchEvent(new Event('change', { bubbles: true }));
      ok(CMP.rate === 2 && !JSON.stringify(S.cfg).includes('rate') && !sent.some(o => JSON.stringify(o).includes('rate')), 'the preview speed is not saved');
      $('#cdlg-pp').click(); await wait(200); ok(CMP.play && CMP.t > 0, 'playing again: ' + CMP.t);
      rt.value = '1'; rt.dispatchEvent(new Event('change', { bubbles: true }));
      // the clock runs on past a minute (no jump back to 0); the slider shows the minute it is in
      if (CMP.play) $('#cdlg-pp').click();
      CMP.t = 59.95; $('#cdlg-pp').click(); for (let i = 0; i < 10; i++) tick(); $('#cdlg-pp').click();
      ok(CMP.t > 60 && CMP.t < 70 && +tr.min === 60 && +tr.max === 120 && $('#cdlg-tv').textContent === CMP.t.toFixed(1), 'past a minute, the time goes on: ' + CMP.t + ' in ' + tr.min + '..' + tr.max);
      tr.value = 70; tr.dispatchEvent(new Event('input', { bubbles: true })); ok(CMP.t === 70, 'scrubbed in the second minute: ' + CMP.t);
      $('#cdlg-rs').click(); ok(CMP.t === 0 && +tr.min === 0 && +tr.max === 60, 'from the start: the first minute again');
      // Before: the applied composition at the same moment, size and colours; nothing sent; an edit shows the draft again
      tr.value = 5; tr.dispatchEvent(new Event('input', { bubbles: true }));
      const v = $('#cdlg-view'), draftPix = pix(), s0 = sent.length;
      const want = (() => { const c = document.createElement('canvas'); c.width = v.width; c.height = v.height; const g = c.getContext('2d');
        g.putImageData(new ImageData(fxPreview.comp.frame(JSON.parse(CMP.base)[0].map(fxPreview.comp.parse), effPal('custom').map(hex2rgb), v.width, v.height, 5), v.width, v.height), 0, 0);
        return Array.from(g.getImageData(0, 0, v.width, v.height).data).join(); })();
      ok(cmpDirty() && !$('#cdlg-ab').disabled, 'a draft: Before offered');
      $('#cdlg-ab').click();
      ok(CMP.ab && $('#cdlg-ab').getAttribute('aria-pressed') === 'true' && $('#cdlg-cap').textContent.startsWith(t('cmp.ab.on')), 'Before on, and said');
      ok(pix() === want && want !== draftPix, 'Before draws the applied composition at the same moment');
      ok(sent.length === s0 && cmpDirty(), 'Before sends nothing and keeps the draft');
      const r4 = $('#cdlg-edit input[data-p="o"]'); r4.value = 30; r4.dispatchEvent(new Event('input', { bubbles: true }));
      ok(!CMP.ab && $('#cdlg-ab').getAttribute('aria-pressed') === 'false', 'an edit shows the draft again');
      // enlarged: a wider window, the picture computed at 96 x 54 at most; Esc goes back first, the editor stays
      $('#cdlg-big').click();
      ok(v.width === 96 && v.height === 54 && $('#cdlg .cdlg').classList.contains('big') && $('#cdlg-big').getAttribute('aria-pressed') === 'true', 'enlarged: ' + v.width + 'x' + v.height);
      document.body.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
      ok(v.width === 64 && v.height === 36 && !cmpIsBig() && !$('#cdlg').classList.contains('hidden') && $('#cdlg-ask').classList.contains('hidden'), 'Esc: back to the normal size, the editor still open');
    });
  });

  test('library: Built-in, My effects, Drafts; a draft opens without lighting, is renamed, kept as a preset, shared and deleted; a preset copy', async () => {
    await fresh(); showTab('effects');
    await cmpKeep(async () => {
      const fxc = fxPreview.comp, L = ['s:dots c:p1 nm:Base', 's:ring c:p2'].map(x => fxc.line(fxc.parse(x)));
      for (let n = 1; n <= 16; n++) delete STATE.cfg['cdraft.' + n];
      STATE.cfg['cdraft.3'] = { name: 'Old one', at: String(Date.now() - 1000), prof: '', l1: L[0], l2: L[1], k1: '' };
      bridge.postMessage(JSON.stringify({ cmd: 'get' })); await wait(150);
      fxTab('presets'); await wait(30);
      const heads = $$('#fx-grid .fx-head').map(h => h.firstChild.textContent);
      ok([t('bp.head'), t('bp.own'), t('bp.drafts')].every(x => heads.includes(x)), 'the three groups: ' + heads.join(' | '));
      const tile = () => $('#fx-grid [data-draft="3"]');
      ok(tile() && tile().querySelector('small').textContent.includes(t('draft.layers', 2)), 'the draft tile says its layers');
      // a click opens it in the constructor; nothing is lit or written
      const fx0 = STATE.effect, lit0 = cmpLines().join('|'); sent.length = 0;
      tile().click(); await wait(80);
      ok(!$('#cdlg').classList.contains('hidden') && dl().join('|') === L.join('|'), 'opened in the constructor: ' + dl().join(' | '));
      ok(!sent.some(o => o.cmd === 'comp_apply' || o.cmd === 'set' || /^preset_/.test(o.cmd)) && STATE.effect === fx0 && cmpLines().join('|') === lit0, 'nothing lit: ' + JSON.stringify(sent));
      cmpClose(); await wait(40); fxTab('presets'); await wait(30);
      // rename: [cdraft.3] name, cleaned, no profile
      tile().querySelector('.pmenu').click(); await wait(40);
      ok(!$('#ddlg').classList.contains('hidden') && $('#ddlg-name').value === 'Old one' && $('#ddlg-note').textContent.includes(t('draft.note')), 'the draft window');
      sent.length = 0; $('#ddlg-name').value = 'New;[x]'; $('#ddlg-save').click(); await wait(60);
      const rn = sets('cdraft.3', 'name');
      ok(rn.length === 1 && !rn[0].p && !/[;\[\]]/.test(rn[0].v) && rn[0].v.startsWith('New') && $('#ddlg').classList.contains('hidden'), 'renamed: ' + JSON.stringify(rn));
      ok(tile() && tile().querySelector('b').textContent === rn[0].v, 'the tile has the new name');
      // kept as a preset: its own layers, not applied; the draft stays
      tile().querySelector('.pmenu').click(); await wait(40); sent.length = 0;
      $('#ddlg-preset').click(); await wait(40);
      ok(!$('#pdlg').classList.contains('hidden') && $('#pdlg-name').value === rn[0].v, 'the preset window, with the draft name');
      $('#pdlg-save').click(); await wait(80);
      const ps = sent.find(o => o.cmd === 'preset_save');
      ok(ps && ps.effect === 'custom' && ps.apply === '0' && ps.fxkv.includes('l1=' + L[0]) && ps.fxkv.includes('l2=' + L[1]), 'saved, not applied: ' + JSON.stringify(ps));
      ok(!sent.some(o => o.cmd === 'comp_apply' || o.cmd === 'set' && o.s === 'custom') && cv('cdraft.3', 'at', ''), 'nothing lit, the draft stays');
      // shared: a code with the draft's layers
      fxTab('presets'); await wait(30); tile().querySelector('.pmenu').click(); await wait(40);
      $('#ddlg-share').click(); await wait(80);
      const pc = presetFromCode($('#share-code').value);
      ok(!$('#share-dlg').classList.contains('hidden') && pc && pc.effect === 'custom' && pc.comp.l.join('|') === L.join('|'), 'the code: ' + JSON.stringify(pc && pc.comp));
      closeShare(); await wait(40);
      // delete: asked twice, every key emptied, no profile, the tile gone
      fxTab('presets'); await wait(30); tile().querySelector('.pmenu').click(); await wait(40); sent.length = 0;
      $('#ddlg-del').click(); await wait(20);
      ok(!sent.length && $('#ddlg-del').textContent === t('preset.delete.sure') && !$('#ddlg').classList.contains('hidden'), 'asks first');
      $('#ddlg-del').click(); await wait(60);
      const dd = sent.filter(o => o.cmd === 'set' && o.s === 'cdraft.3');
      ok(dd.length === 14 && dd.every(o => o.v === '' && !o.p) && !tile() && $('#ddlg').classList.contains('hidden'), 'deleted: ' + dd.length);
      ok(!$$('#fx-grid [data-draft]').length && $('#fx-grid .comm-note') && $('#fx-grid').textContent.includes(t('draft.none')), 'no drafts said');
      // copy of a preset of one's own: preset_copy, every key kept under the new name, nothing applied
      const P = presets()[0], src = Object.assign({}, STATE.cfg['preset.' + P.id], { 'fx.x': '1', 'z.1': '#123456' });
      STATE.cfg['preset.' + P.id] = src; bridge.postMessage(JSON.stringify({ cmd: 'get' })); await wait(150);
      const g0 = cv('general', 'preset', ''), n0 = presets().length;
      presetDialog(P.id); await wait(40);
      ok(!$('#pdlg-copy').classList.contains('hidden'), 'Copy offered for a preset of one\'s own');
      sent.length = 0; $('#pdlg-copy').click(); await wait(150);
      const cp = sent.find(o => o.cmd === 'preset_copy'), Q = presets().find(q => q.name === t('preset.copy.name', P.name));
      ok(cp && cp.id === String(P.id) && $('#pdlg').classList.contains('hidden'), 'sent: ' + JSON.stringify(cp));
      ok(Q && presets().length === n0 + 1 && cv('preset.' + Q.id, 'fx.x', '') === '1' && cv('preset.' + Q.id, 'z.1', '') === '#123456', 'the copy keeps its keys');
      ok(cv('general', 'preset', '') === g0 && !sent.some(o => o.cmd === 'preset' || o.cmd === 'effect'), 'nothing applied');
      presetDialog(0); ok($('#pdlg-copy').classList.contains('hidden'), 'no Copy for a new preset'); closePresetDialog();
    });
  });

  test('device card: What controls its lighting, folded, from real data; sent is never called confirmed', async () => {
    await fresh(); showTab('devices'); await wait(30);
    const d = S.ext.devs.find(x => x.id === 1), det = id => $('#dev-st-' + id).closest('.card').querySelector('.dev-why');
    const body = id => $('#dev-why-' + id).textContent, open = async id => { det(id).open = true; await wait(40); };
    ok(det(1) && !det(1).open && !$('#dev-why-1').textContent, 'folded and empty until asked');
    ok(!$$('.why-list').some(x => !x.closest('#dev-grid')), 'shown on the device cards only');
    await open(1);
    ok(body(1).includes(t('why.prof')) && body(1).includes(fxName(S.effect)) && body(1).includes(t('why.own.none')) && body(1).includes(t('why.simple.none')), 'the rows: ' + body(1));
    ok(d.sent >= 0 && body(1).includes(t('why.st.sent', '<1')) && body(1).includes(t('why.noack')), 'sent, and said not to be a confirmation');
    ok(!/confirmed|подтвержден|confirmé/i.test(body(1).replace(t('why.noack'), '')), 'nothing called confirmed');
    // its own brightness: the share of the overall one
    setCfgLocal('zone.dev1', 'brightness', '50'); updateDevices();
    ok(body(1).includes(t('why.bri.of', Math.round(S.brightness / 2), S.brightness, 50)), 'own brightness: ' + body(1));
    setCfgLocal('zone.dev1', 'effect', 'rainbow'); updateDevices();
    ok(body(1).includes(fxName('rainbow')), 'own effect shown');
    setCfgLocal('zone.dev1', 'brightness', '100'); setCfgLocal('zone.dev1', 'effect', '');
    // unreachable, nothing sent yet, given back to its own lighting
    d.online = 0; d.fails = 2; updateDevices();
    ok(body(1).includes(t('why.st.off')) && body(1).includes(t('dev.offline')), 'unreachable: ' + body(1));
    Object.assign(d, { online: 1, fails: 0, sent: -1 }); updateDevices();
    ok(body(1).includes(t('why.st.wait')), 'no frame yet: ' + body(1));
    d.own = 1; updateDevices();
    ok(body(1).includes(t('why.st.not')) && body(1).includes(t('own.status')), 'its own lighting: ' + body(1));
    d.own = 0; d.sent = 120;
    // a profile being switched to: chosen, not yet on
    const P0 = profiles()[0];
    if (P0) { PROF.go = P0.id; updateDevices(); ok(body(1).includes(t('why.st.asked', P0.name)), 'switching said: ' + body(1)); PROF.go = 0; }
    // simplified: a bulb takes one colour per light, an OpenRGB controller with no Direct one steady colour
    await open(4);
    ok(body(4).includes(t('why.bulb')) && body(4).includes(t('why.st.simple')), 'a bulb: ' + body(4));
    const o = S.ext.devs.find(x => x.id === 2); o.cap = 3; await open(2); updateDevices();
    ok(body(2).includes(t('why.steady')) && body(2).includes(t('why.st.simple')), 'steady colour only: ' + body(2));
    o.cap = 1; updateDevices();
    ok(!body(2).includes(t('why.st.simple')), 'Direct: not simplified');
    // kept open when the cards are built again, closed again on request
    buildDevices(); updateDevices();
    ok(det(1).open && body(1).includes(t('why.prof')), 'stays open across a rebuild');
    det(1).open = false; await wait(40); buildDevices();
    ok(!det(1).open, 'closed stays closed');
  });

  test('home summary: the effect, how many devices haku lights of those on; its buttons go where they say; hidden with nothing found', async () => {
    await fresh(); showTab('home'); await wait(30);
    const st = () => $('#home-sum-st').textContent;
    ok(!$('#home-sum').classList.contains('hidden') && $('#home-sum-fx').textContent.startsWith(fxName(S.effect)), 'the effect: ' + $('#home-sum-fx').textContent);
    ok(!$('#home-sum').textContent.includes(t('prof.none')) && !$('#home-sum').querySelector('[id*=prof]'), 'the profile not repeated: it is the picker above');
    // count it here the same way: on and not given away, lit when haku drives it and it answers
    const ext = S.ext.devs.filter(d => d.enabled && !d.held), n = st().match(/\d+/g).map(Number);
    const total = ext.length + S.bulbs.length + nanoCtls().length + (S.sticks ? 1 : 0) + (S.msi ? 1 : 0);
    ok(n.includes(total), 'of all on: ' + st() + ' / ' + total);
    const before = ext.filter(d => !d.own && (d.online || d.covered)).length, d = ext.find(x => !x.own && x.online);
    const lit0 = +st().match(/(\d+)/g).slice(-2)[0];
    d.own = 1; updateHome();
    ok(st() !== '' && !st().includes(t('home.sum.lit', lit0, total)) && st().includes(t('home.sum.lit', lit0 - 1, total)), 'its own lighting is not haku\'s: ' + st());
    d.own = 0; updateHome(); ok(before >= 1, 'a lit device in the mock');
    $('#home-go-fx').click(); ok($('#tab-effects').classList.contains('active'), 'Lighting opens Lighting');
    showTab('home'); $('#home-go-dev').click(); ok($('#tab-devices').classList.contains('active'), 'Devices opens Devices');
    showTab('home');
    ok(['en', 'ru', 'fr'].every(l => STR[l]['home.sum.lit']), 'a language misses the summary');
    const none = JSON.parse(JSON.stringify(STATE));
    Object.assign(none, { bulbs: [], nano: { ctls: [] }, sticks: 0, msi: null, fans: { on: 0 }, ll: null }); none.ext.devs = [];
    emit(none); await wait(80);
    ok($('#home-sum').classList.contains('hidden'), 'hidden with nothing found');
    emit(STATE); await wait(80);
  });

  test('a modal window: focus goes in on open, Tab stays inside both ways, Escape or Cancel gives it back to the opener', async () => {
    await fresh(); showTab('help'); await wait(30);
    const op = $('#report-open'), w = $('#report-dlg'), tab = shift => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', shiftKey: shift, bubbles: true, cancelable: true }));
    op.focus(); op.click(); await wait(80);
    ok(w.contains(document.activeElement), 'focus inside on open: ' + document.activeElement.id);
    const f = [...w.querySelectorAll('button, input, textarea')].filter(e => !e.disabled && e.getClientRects().length);
    f[f.length - 1].focus(); tab(false);
    ok(document.activeElement === f[0], 'Tab from the last goes to the first: ' + document.activeElement.id);
    tab(true); ok(document.activeElement === f[f.length - 1], 'Shift+Tab from the first goes to the last: ' + document.activeElement.id);
    op.blur(); document.body.focus(); tab(false); ok(w.contains(document.activeElement), 'focus outside is pulled back in');
    $('#report-close').click(); await wait(30);
    ok(document.activeElement === op, 'Cancel gives focus back to the opener: ' + (document.activeElement.id || document.activeElement.tagName));
    // Escape on another one, opened from the keyboard: the preset dialog
    showTab('effects'); fxTab('presets'); await wait(30);
    const save = $('#fx-grid .fx.add') || $('.fx.add'); ok(save, 'the new preset tile'); save.focus(); save.click(); await wait(80);
    ok(!$('#pdlg').classList.contains('hidden') && $('#pdlg').contains(document.activeElement), 'the preset dialog takes focus');
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); await wait(30);
    ok($('#pdlg').classList.contains('hidden') && document.activeElement === save, 'Escape gives it back: ' + (document.activeElement.id || document.activeElement.tagName));
    tab(false); ok(true, 'no modal: Tab left alone');
  });

  const only =decodeURIComponent(location.hash.slice(1));   // ?mock&test#TRYX: only the tests whose name has it
  setCfg('general', 'ui_motion', '1'); applyMotion();   // the tests move whatever this Windows says about motion
  for (const [name, fn] of tests) {
    if (only && !name.includes(only)) continue;
    document.title = 'running: ' + name;   // what a run that never ends was on (tools/sim/ui-tests.ps1 prints it)
    try { await fn(); log.push('ok   ' + name); }
    catch (e) { R.failed++; log.push('FAIL ' + name + ': ' + e.message); }
  }
  R.done = true;
  const box = document.createElement('pre');
  box.style.cssText = 'position:fixed;z-index:99999;top:0;left:0;right:0;margin:0;padding:8px;font:12px monospace;background:' + (R.failed ? '#400' : '#032') + ';color:#fff;white-space:pre-wrap';
  box.textContent = log.join('\n'); document.body.appendChild(box);
  console.log('TEST:\n' + log.join('\n'));
})();
