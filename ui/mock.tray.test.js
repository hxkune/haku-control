// SPDX-License-Identifier: GPL-3.0-only
// Checks on the tray panel against its fake core (mock.tray.js; not shipped). Open tray.html?mock&test; results in a
// box at the top, the console, and window.TEST = { done, failed, log }. tools/sim/ui-tests.ps1 runs them headless.
(async function () {
  const sent = window.MOCK_SENT, S = window.MOCK_STATE;
  const wait = ms => new Promise(r => setTimeout(r, ms));
  const log = [], R = window.TEST = { done: false, failed: 0, log };
  const ok = (c, msg) => { if (!c) throw new Error(msg); };
  const q = s => document.querySelector(s), qa = s => [...document.querySelectorAll(s)];
  const cmds = c => sent.filter(o => o.cmd === c);
  const esc = () => document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
  const until = async (f, what, ms = 5000) => { for (const end = Date.now() + ms; !f(); await wait(4)) if (Date.now() > end) throw new Error('never came: ' + what); };
  // the fake core's answers all delivered (mock.tray.js counts them), not a guessed time: a slow PC lags its timers
  const settle = async () => { await wait(0); await until(() => !window.mockBusy, 'the fake core answers'); };
  const changed = async () => { window.mockState(); await settle(); };
  const reopen = async () => { sent.length = 0; window.mockOpen(); await settle(); };
  const base = JSON.parse(JSON.stringify(S));
  const reset = async () => {   // the fake core back to how it started, the panel on its first view
    window.mockSilent = 0; window.mockProfFail = 0;
    for (const k of Object.keys(S)) delete S[k];
    Object.assign(S, JSON.parse(JSON.stringify(base)));
    esc(); esc(); await settle(); await reopen();   // back from a list if in one, then closed
  };
  q('#panel').style.width = '340px';   // the window's width (PANEL_W), not the test browser's
  // hello, the state, open, shown; a failure here shows in the first test
  await until(() => cmds('hello').length, 'hello').then(settle).catch(() => {});
  const tests = [];
  const test = (name, fn) => tests.push([name, fn]);

  test('opens: hello, the state, its height, then the entrance; a second open sends the height again', async () => {
    ok(q('#panel').classList.contains('in'), 'no entrance after "shown"');
    ok(document.activeElement === q('#power'), 'the focus is not on the power button: ' + document.activeElement.id);
    await reopen();
    ok(cmds('height').length === 1 && cmds('height')[0].h > 200, 'no height on the second open: ' + JSON.stringify(sent));
  });

  test('Escape closes from the first view; a ⋯ action asks the app and the panel goes back to its first view', async () => {
    await reset();
    sent.length = 0; esc(); await settle();
    ok(cmds('close').length === 1, 'Escape did not ask to close');
    ok(!q('#panel').classList.contains('in'), 'still marked open after "closed"');
    await reopen();
    q('[data-go="more"]').click(); await wait(10);
    q('[data-menu="help"]').click(); await settle();
    ok(cmds('menu').length === 1 && cmds('menu')[0].do === 'help', 'help not asked for');
    ok(!q('#v-main').hidden && q('#v-more').hidden, 'not back on the first view after closing');
    await reopen(); q('[data-go="more"]').click(); await wait(10);
    q('#v-more [data-key="m-window"]').click(); await wait(20);
    q('[data-menu="window"]').click(); await settle();
    ok(JSON.stringify(cmds('menu').map(m => m.do)) === '["settings","window"]', 'Settings / Open asked for: ' + JSON.stringify(cmds('menu')));
  });

  test('Escape in a list goes back first, the focus on what opened it', async () => {
    await reset();
    q('[data-go="effects"]').click(); await wait(10);
    ok(!q('#v-effects').hidden && document.activeElement.matches('[data-back]'), 'the effects list did not open with focus on Back');
    sent.length = 0; esc(); await wait(10);
    ok(!q('#v-main').hidden && !cmds('close').length, 'Escape closed the panel instead of going back');
    ok(document.activeElement === q('[data-go="effects"]'), 'the focus went elsewhere: ' + document.activeElement.outerHTML.slice(0, 60));
  });

  test('brightness: a drag sends few commands, never closes, and the last value always goes', async () => {
    await reset();
    const br = q('#bri'); sent.length = 0;
    for (let v = 70; v >= 31; v--) { br.value = v; br.dispatchEvent(new Event('input', { bubbles: true })); await wait(5); }
    const during = cmds('brightness').length;
    ok(during > 0 && during < 10, 'commands during a 40-step drag: ' + during);
    br.value = 30; br.dispatchEvent(new Event('input', { bubbles: true })); br.dispatchEvent(new Event('change', { bubbles: true }));
    await wait(100);
    const all = cmds('brightness');
    ok(all[all.length - 1].v === 30, 'the final value was not sent: ' + JSON.stringify(all.slice(-2)));
    ok(!cmds('close').length, 'the slider closed the panel');
    ok(q('#bri-v').textContent === '30%' && br.getAttribute('aria-valuetext') === '30%', 'the value beside it: ' + q('#bri-v').textContent);
    ok(S.bri === 30, 'the fake core did not get 30');
  });

  test('brightness: the state arriving mid-drag does not move the thumb; later it follows the core', async () => {
    await reset();
    const br = q('#bri');
    br.value = 20; br.dispatchEvent(new Event('input', { bubbles: true }));
    S.bri = 55; await changed();
    ok(+br.value === 20, 'the thumb jumped under the pointer: ' + br.value);
    br.dispatchEvent(new Event('change', { bubbles: true })); await wait(700);
    S.bri = 55; await changed();
    ok(+br.value === 55 && q('#bri-v').textContent === '55%', 'it does not follow the core afterwards: ' + br.value);
  });

  test('brightness 0 stays a brightness, not the off switch', async () => {
    await reset();
    const br = q('#bri'); sent.length = 0;
    br.value = 0; br.dispatchEvent(new Event('input', { bubbles: true })); br.dispatchEvent(new Event('change', { bubbles: true })); await wait(60);
    ok(!cmds('power').length && cmds('brightness').pop().v === 0, 'sent ' + JSON.stringify(sent));
  });

  test('what shows is the core\'s: profile, effect, palette, lighting on', async () => {
    await reset();
    ok(q('[data-prof="1"]').getAttribute('aria-pressed') === 'true' && q('[data-prof="2"]').getAttribute('aria-pressed') === 'false', 'the active profile');
    ok(q('#fx .name').textContent === 'Aurora', 'effect: ' + q('#fx .name').textContent);
    ok(qa('#fx .pal i').length === 3, 'the palette swatches');
    ok(q('#power').getAttribute('aria-pressed') === 'true' && q('#state-txt').textContent === 'Lighting is on', 'the power state');
  });

  test('power: "sending" until the core says so, then off; the effect row says off without swatches', async () => {
    await reset();
    q('#power').click();
    ok(q('#state-txt').textContent === 'Sending…' && q('#power').getAttribute('aria-pressed') === 'true', 'shown as done before the core answered');
    await settle();
    ok(q('#power').getAttribute('aria-pressed') === 'false' && q('#state-txt').textContent === 'Lighting is off', 'not off after the state');
    ok(q('#fx .name').textContent === 'Off' && !q('#fx .pal'), 'the effect row');
    q('#power').click(); await settle();
    ok(q('#fx .name').textContent === 'Aurora', 'the effect did not come back');
  });

  test('a profile switch the core refuses: an error read out, the one in use stays selected', async () => {
    await reset();
    window.mockProfFail = 2;
    q('[data-prof="2"]').click(); await settle();
    ok(q('[data-prof="1"]').getAttribute('aria-pressed') === 'true' && q('[data-prof="2"]').getAttribute('aria-pressed') === 'false', 'the refused one is shown selected');
    const e = q('#prof .err');
    ok(e && e.getAttribute('role') === 'alert' && e.textContent.includes('Games'), 'no error, or not about the one in use: ' + (e && e.textContent));
    window.mockProfFail = 0; q('[data-prof="2"]').click(); await settle();
    ok(q('[data-prof="2"]').getAttribute('aria-pressed') === 'true' && !q('#prof .err'), 'the retry did not switch or the error stays');
  });

  test('a command the core never acts on: after 5 s it says so and shows the real state', async () => {
    await reset();
    window.mockSilent = 1;
    q('#power').click(); await wait(100);
    ok(q('#state-txt').textContent === 'Sending…', 'not marked as sent');
    await wait(5200);
    ok(q('#state-txt').textContent === 'Lighting is on' && q('#power').getAttribute('aria-pressed') === 'true', 'shown as off');
    ok(q('#err-power .err') && q('#live').textContent.includes('did not confirm'), 'no error said');
    esc(); await settle(); await reopen();
    ok(!q('#err-power .err'), 'the old error is still there after reopening');
  });

  test('a change from elsewhere (window, schedule, phone) shows at once and keeps the focus', async () => {
    await reset();
    q('[data-prof="2"]').focus();
    S.profile = 3; S.fx = 'fire'; S.pal = ['#ff3d00', '#ff9a00']; S.devs[0].s = 'off'; await changed();
    ok(q('[data-prof="3"]').getAttribute('aria-pressed') === 'true', 'the profile');
    ok(q('#fx .name').textContent === 'Fire' && qa('#fx .pal i').length === 2, 'the effect');
    ok(q('#devs .switch').getAttribute('aria-checked') === 'false', 'the device');
    ok(document.activeElement === q('[data-prof="2"]'), 'the focus was lost');
    q('[data-go="profiles"]').click(); await wait(10);
    q('#v-profiles [data-prof="5"]').focus();
    S.profile = 2; await changed();
    ok(document.activeElement === q('#v-profiles [data-prof="5"]') && q('#v-profiles [data-prof="2"]').getAttribute('aria-current') === 'true', 'the list lost its focus or state');
  });

  test('devices: a word for each state, a switch where haku can change it; the switch sends the existing commands', async () => {
    await reset();
    q('[data-go="devices"]').click(); await wait(10);
    const row = n => qa('#v-devices .dev').find(r => r.querySelector('.name').textContent === n);
    ok(row('Desk lamp').querySelector('small').textContent === 'Control on · no connection', 'offline');
    ok(row('Corsair K70 RGB PRO').querySelector('small').textContent === 'Driven another way in haku', 'held by its other path in haku, not by another program');
    ok(row('Desk lamp').querySelector('.switch').getAttribute('aria-checked') === 'true', 'an unreachable one keeps the user\'s switch');
    ok(row('Corsair K70 RGB PRO').querySelector('.switch').getAttribute('aria-checked') === 'true', 'the switch is not the status');
    sent.length = 0;
    row('Memory lighting').querySelector('.switch').click(); await settle();
    row('Lian Li UNI HUB').querySelector('.switch').click(); await settle();
    ok(JSON.stringify(sent) === JSON.stringify([{ cmd: 'toggle', k: 'ram_enabled' }, { cmd: 'set', s: 'dev.6', k: 'enabled', v: '1' }]), 'sent ' + JSON.stringify(sent));
    ok(row('Memory lighting').querySelector('small').textContent === 'Off', 'not off after the state');
  });

  test('a device switch: a second click before the answer sends nothing; a device that vanished is not taken as done', async () => {
    await reset();
    q('[data-go="devices"]').click(); await wait(10);
    const row = n => qa('#v-devices .dev').find(r => r.querySelector('.name').textContent === n);
    window.mockSilent = 1; sent.length = 0;
    row('Lian Li UNI HUB').querySelector('.switch').click(); row('Lian Li UNI HUB').querySelector('.switch').click(); await wait(20);
    ok(cmds('set').length === 1, 'a toggle went twice: ' + JSON.stringify(sent));
    S.devs = S.devs.filter(d => d.id !== 6); await changed();
    const e = q('#v-devices .err');
    ok(e && e.textContent === 'The device disconnected. The change could not be confirmed.', 'no error, or the wrong one: ' + (e && e.textContent));
    await wait(60); ok(q('#live').textContent === e.textContent, 'not read out: ' + q('#live').textContent);
    S.devs.push({ id: 6, n: 'Lian Li UNI HUB', s: 'on' }); await changed();   // back, and the change did happen
    ok(!q('#v-devices .err') && row('Lian Li UNI HUB').querySelector('small').textContent === 'On', 'the late answer did not clear it');
  });

  test('a device switch never confirmed: an error after 5 s, then a late answer clears it; Russian wording', async () => {
    await reset();
    S.lang = 'ru'; await changed();
    q('[data-go="devices"]').click(); await wait(10);
    const sw = () => qa('#v-devices .dev').find(r => r.querySelector('.name').textContent === 'Memory lighting').querySelector('.switch');
    window.mockSilent = 1;
    sw().click(); await wait(5300);
    ok(q('#v-devices .err') && q('#v-devices .err').textContent.startsWith('haku не подтвердил'), 'no timeout error');
    ok(sw().getAttribute('aria-checked') === 'true' && !sw().hasAttribute('aria-busy'), 'still shown as sending or as done');
    S.devs[0].s = 'off'; await changed();
    ok(!q('#v-devices .err') && sw().getAttribute('aria-checked') === 'false', 'the answer that came late did not clear the error');
    sent.length = 0; sw().click(); await wait(20);
    ok(cmds('toggle').length === 1, 'after the error a new click must go again: ' + JSON.stringify(sent));
    S.devs[0].s = 'on'; await changed();
    S.devs = S.devs.filter(d => d.k !== 'ram_enabled'); await changed();
    ok(!q('#v-devices .err'), 'a confirmed change was reported again when the device went');
  });

  test('no devices, no profiles: said plainly, nothing made up', async () => {
    await reset();
    S.devs = []; S.profiles = []; S.profile = 0; await changed();
    ok(q('#devs .muted').textContent === 'No devices yet' && !q('[data-go="devices"]'), 'the devices card');
    ok(q('#sec-prof').hidden, 'an empty profile card shows');
  });

  test('many devices with long names: 3 on the first view, all one step in, nothing wider than the panel', async () => {
    await reset();
    for (let i = 7; i < 40; i++) S.devs.push({ id: i, n: 'Устройство с очень длинным названием, которое не помещается — №' + i + ' ✨', s: i % 2 ? 'on' : 'connecting' });
    await changed();
    ok(qa('#devs .dev').length === 3 && q('[data-go="devices"]').textContent.includes('Show all 38 devices'), 'the first view');
    q('[data-go="devices"]').click(); await wait(10);
    ok(qa('#v-devices .dev').length === 38, 'the full list');
    const p = q('#panel'), n = q('#v-devices .dev:last-child .name');
    ok(p.scrollWidth <= p.clientWidth, 'wider than the panel: ' + p.scrollWidth);
    ok(n.title === n.textContent && n.scrollWidth > n.clientWidth, 'a cut name has no full title');
    ok(cmds('height').pop().h > 1000, 'the height is not the list\'s');
  });

  test('Pro not active: such devices say "Needs Pro"', async () => {
    await reset();
    S.pro_off = 1; S.devs.push({ id: 9, n: 'Govee lamp', s: 'pro' }); await changed();
    q('[data-go="devices"]').click(); await wait(10);
    ok(qa('#v-devices .dev').some(r => r.querySelector('small').textContent === 'Pro is needed to control it'), 'no Pro wording');
  });

  test('effects: the list and the presets apart, the current one marked, a pick sends effect / preset', async () => {
    await reset();
    q('[data-go="effects"]').click(); await wait(10);
    ok(q('[data-fx="aurora"]').getAttribute('aria-current') === 'true', 'the current effect');
    ok(qa('#v-effects h3').pop().textContent === 'Your presets' && q('[data-preset="1"] .pal'), 'the presets with their colours');
    sent.length = 0;
    q('[data-fx="fire"]').click(); await settle();
    q('[data-preset="2"]').click(); await settle();
    ok(JSON.stringify(sent) === JSON.stringify([{ cmd: 'effect', id: 'fire' }, { cmd: 'preset', id: 2 }]), 'sent ' + JSON.stringify(sent));
    ok(q('[data-preset="2"]').getAttribute('aria-current') === 'true' && q('[data-fx="ocean"]').getAttribute('aria-current') === 'false', 'the preset is not the current one, or its effect still is');
  });

  test('effects: favourites and recent first, each effect once; a search narrows them; arrows, letters and Enter work', async () => {
    await reset();
    const key = (k, el = document.activeElement) => el.dispatchEvent(new KeyboardEvent('keydown', { key: k, bubbles: true, cancelable: true }));
    const find = () => q('#fx-q'), ids = () => qa('#fx-list [data-fx]').map(b => b.dataset.fx);
    const type = async v => { find().value = v; find().dispatchEvent(new Event('input', { bubbles: true })); await wait(10); };
    q('[data-go="effects"]').click(); await wait(10);
    ok(JSON.stringify(qa('#v-effects h3').map(h => h.textContent)) === '["Favourites","Recent","All effects","Your presets"]', 'headings ' + qa('#v-effects h3').map(h => h.textContent));
    ok(ids().slice(0, 3).join() === 'fire,aurora,ocean' && ids().length === S.fxs.length && new Set(ids()).size === ids().length, 'order or repeats: ' + ids());
    find().focus(); await type('OCE');
    ok(ids().join() === 'ocean' && !q('#v-effects h3') && document.activeElement === find(), 'the search: ' + ids() + ' focus ' + document.activeElement.id);
    sent.length = 0; key('Enter'); await settle();
    ok(JSON.stringify(cmds('effect')) === '[{"cmd":"effect","id":"ocean"}]', 'Enter did not take the first match: ' + JSON.stringify(sent));
    await type('zzz');
    ok(q('#fx-list .muted').textContent === 'Nothing matches “zzz”' && !ids().length, 'nothing found not said');
    key('Escape'); await wait(10);
    ok(!q('#v-effects').hidden && find().value === '' && ids().length === S.fxs.length, 'Escape did not clear the search first');
    ok(ids()[1] === 'ocean', 'the pick is not first among the recent ones: ' + ids());
    key('ArrowDown'); ok(document.activeElement === q('#fx-list [data-fx="fire"]'), 'ArrowDown did not reach the first row');
    key('ArrowDown'); ok(document.activeElement === q('#fx-list [data-fx="ocean"]'), 'ArrowDown did not move on');
    key('ArrowUp'); key('ArrowUp'); ok(document.activeElement === find(), 'ArrowUp did not go back to the search');
    q('#fx-list [data-fx="fire"]').focus(); key('r'); await wait(10);
    ok(document.activeElement === find() && find().value === 'r' && ids().every(id => S.fxs.find(f => f.id === id).t.toLowerCase().includes('r') || id.includes('r')), 'a letter on a row did not go to the search');
    key('Escape'); key('Escape'); await wait(10);
    ok(!q('#v-main').hidden, 'Escape did not go back after the search was cleared');
    q('[data-go="effects"]').click(); await wait(10);
    ok(find().value === '' && ids().length === S.fxs.length, 'the search was kept into the next visit');
  });

  test('devices: up to 3 pinned by their own key, shown first on the first view whatever the order', async () => {
    await reset();
    q('[data-go="devices"]').click(); await wait(10);
    ok(q('#v-devices .hint').textContent.includes('up to 3'), 'no word on pinning');
    sent.length = 0;
    for (const k of ['id:6', 'k:nanoleaf_enabled', 'id:4']) { q(`[data-pin="${k}"]`).click(); await settle(); }
    ok(JSON.stringify(cmds('set').map(c => [c.s, c.k, c.v])) === '[["general","tray_pin","id:6"],["general","tray_pin","id:6,k:nanoleaf_enabled"],["general","tray_pin","id:6,k:nanoleaf_enabled,id:4"]]',
      'sent ' + JSON.stringify(cmds('set')));
    ok(q('[data-pin="id:4"]').getAttribute('aria-pressed') === 'true' && q('[data-pin="k:ram_enabled"]').disabled, 'a fourth one can be pinned');
    ok(q('[data-pin="id:4"]').getAttribute('aria-label') === 'Keep Desk lamp on the first screen', 'the pin has no name');
    q('#v-devices [data-back]').click(); await wait(10);
    const names = () => qa('#devs .dev .name').map(n => n.textContent).join('|');
    ok(names() === 'Lian Li UNI HUB|Nanoleaf Shapes|Desk lamp', 'the first view: ' + names());
    S.devs.reverse(); S.devs = S.devs.filter(d => d.id !== 4); await changed();
    ok(names() === 'Lian Li UNI HUB|Nanoleaf Shapes' && q('[data-go="devices"]').textContent.includes('Show all 4'), 'by key, a removed one left out: ' + names());
    q('[data-go="devices"]').click(); await wait(10);
    ok(!q('[data-pin="k:ram_enabled"]').disabled, 'a removed device still holds a pin');
    q('[data-pin="id:6"]').click(); await settle();
    ok(S.dpin === 'k:nanoleaf_enabled', 'unpinning: ' + S.dpin);
  });

  test('⋯: PC lighting hand-over shows what the core did; quit is apart from power', async () => {
    await reset();
    q('[data-go="more"]').click(); await wait(10);
    const pc = () => q('[data-pc]');
    ok(pc().textContent.includes('Hand over') && pc().textContent.includes('0 of 3'), 'before: ' + pc().textContent);
    pc().click(); await settle();
    ok(pc().textContent.includes('Light with haku') && pc().textContent.includes('3 of 3'), 'after: ' + pc().textContent);
    ok(!q('#v-main').contains(q('[data-menu="quit"]')), 'quit is on the first view');
  });

  test('themes: dark, grey, light, a hidden one only once opened', async () => {
    await reset();
    const bg = () => getComputedStyle(document.body).backgroundColor, th = () => document.documentElement.dataset.theme;
    S.theme = 'light'; await changed(); ok(th() === 'light' && bg() === 'rgb(255, 255, 255)', 'light ' + bg());
    S.theme = 'grey'; await changed(); ok(th() === 'grey' && bg() === 'rgb(48, 49, 55)', 'grey ' + bg());
    S.theme = 'dark'; await changed(); ok(th() === undefined && bg() === 'rgb(18, 18, 18)', 'dark ' + bg());
    S.theme = 'verity'; await changed(); ok(th() === undefined, 'a hidden theme not opened was used');
    S.theme = 'kyoka'; await changed(); ok(th() === 'kyoka', 'an opened hidden theme was not used');
  });

  test('motion off (haku or Windows): the panel opens without its entrance', async () => {
    await reset();
    S.motion = '0'; await changed(); await reopen(); await wait(20);
    ok(document.body.classList.contains('still') && getComputedStyle(q('#panel')).animationName === 'none', 'still animated');
    S.motion = '1'; await changed(); await reopen(); await wait(20);
    ok(getComputedStyle(q('#panel')).animationName !== 'none', 'motion on: no entrance');
  });

  test('keyboard and screen readers: every control has a name and a role, the states are said', async () => {
    await reset();
    for (const v of ['main', 'profiles', 'effects', 'devices', 'more']) {
      if (v !== 'main') { q(`[data-go="${v}"]`).click(); await wait(10); }
      const sec = q('#v-' + v);
      for (const b of sec.querySelectorAll('button, input')) {
        const name = (b.getAttribute('aria-label') || b.textContent || (b.labels && b.labels[0] && b.labels[0].textContent) || '').trim();
        ok(name, v + ': a control without a name ' + b.outerHTML.slice(0, 80));
        ok(b.tabIndex >= 0, v + ': not reachable with Tab ' + b.outerHTML.slice(0, 60));
      }
      if (v !== 'main') { q(`#v-${v} [data-back]`).click(); await wait(10); }
    }
    ok(q('#bri').labels[0].textContent === 'Brightness' && q('#bri').getAttribute('aria-valuetext') === '70%', 'the slider');
    ok(qa('.switch').every(s => s.getAttribute('role') === 'switch' && /^(true|false)$/.test(s.getAttribute('aria-checked'))), 'switches');
    ok(q('#live').getAttribute('aria-live') === 'polite', 'no live region');
  });

  test('the language: Russian and French from the core', async () => {
    await reset();
    S.lang = 'ru'; await changed();
    ok(q('#state-txt').textContent === 'Подсветка включена' && q('#bri-l').textContent === 'Яркость' && document.documentElement.lang === 'ru', 'ru');
    S.lang = 'fr'; await changed();
    ok(q('#state-txt').textContent === 'L’éclairage est allumé', 'fr');
  });

  const only = decodeURIComponent(location.hash.slice(1));
  for (const [name, fn] of tests) {
    if (only && !name.includes(only)) continue;
    document.title = 'running: tray: ' + name;   // what a run that never ends was on (tools/sim/ui-tests.ps1 prints it)
    try { await fn(); log.push('ok   tray: ' + name); }
    catch (e) { R.failed++; log.push('FAIL tray: ' + name + ': ' + e.message); }
  }
  R.done = true;
  const box = document.createElement('pre');
  box.style.cssText = 'position:fixed;z-index:99999;top:0;left:0;right:0;margin:0;padding:8px;font:12px monospace;background:' + (R.failed ? '#400' : '#032') + ';color:#fff;white-space:pre-wrap';
  box.textContent = log.join('\n'); document.body.appendChild(box);
  console.log('TEST:\n' + log.join('\n'));
})();
