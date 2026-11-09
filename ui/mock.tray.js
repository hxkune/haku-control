// SPDX-License-Identifier: GPL-3.0-only
// The tray panel without the app (open tray.html?mock; not shipped: build.cmd drops mock.*). It answers like
// ui_tray.cpp + ui_dispatch: the state after hello and after each command, "shown" after the page's height, "closed"
// on close or a ⋯ action. Nothing here reaches a device.
//   tray.html?mock#light / #grey   a theme     #none  no devices or profiles     #many  many devices, long names
//   #pro   Pro not active          #ru / #fr  the language
//   window.mockSilent = 1  commands are taken and never acted on      window.mockProfFail = 2|3|4  profile.c refuses
(function () {
  const FX = [['flow', 'Flow'], ['caustic', 'Caustics'], ['aurora', 'Aurora'], ['breathe', 'Breathe'], ['rainbow', 'Rainbow'],
    ['wave', 'Colour wave'], ['fire', 'Fire'], ['ocean', 'Ocean'], ['twinkle', 'Constellation'], ['plasma', 'Plasma'],
    ['candle', 'Candle'], ['temperature', 'Temperature'], ['audio', 'Sound'], ['static', 'Static colour']].map(([id, t]) => ({ id, t }));
  const PAL = { flow: ['#5b8cff', '#a45bff', '#ff5bb0'], aurora: ['#3dd68c', '#3db8d6', '#8a5bff'], fire: ['#ff3d00', '#ff9a00', '#ffd23d'],
    ocean: ['#003d7a', '#0a84ff', '#5ee0ff'], static: ['#ffffff'] };
  const h = location.search === '?mock' ? location.hash.slice(1) : '';   // with &test the hash picks tests
  const S = window.MOCK_STATE = {
    type: 'tray', lang: ['ru', 'fr'].includes(h) ? h : 'en', theme: ['light', 'grey', 'kyoka'].includes(h) ? h : 'dark', unlocked: 'kyoka', motion: 'sys',
    pro_off: h === 'pro' ? 1 : 0, fx: 'aurora', off: 'off', bri: 70, fav: 'fire', recent: 'aurora,fire,ocean', dpin: '', pal: PAL.aurora, fxs: FX,
    profile: 1, profiles: [{ id: 1, n: 'Games', pin: 1 }, { id: 2, n: 'Work', pin: 1 }, { id: 3, n: 'Night', pin: 1 }, { id: 4, n: 'Streaming', pin: 0 },
      { id: 5, n: 'Ночной режим для долгих сессий 🌙', pin: 0 }],
    prof: { err: 0, id: 0, n: 0 }, preset: 0,
    presets: [{ id: 1, n: 'Sunset', fx: 'flow', pal: '#ff5b3d, #ff9a3d, #a45bff' }, { id: 2, n: 'Deep sea', fx: 'ocean', pal: '#003d7a, #0a84ff' }],
    devs: [{ k: 'ram_enabled', n: 'Memory lighting', s: 'on' }, { k: 'nanoleaf_enabled', n: 'Nanoleaf Shapes', s: 'on' },
      { id: 4, n: 'Desk lamp', s: 'offline' }, { id: 5, n: 'Corsair K70 RGB PRO', s: 'busy' }, { id: 6, n: 'Lian Li UNI HUB', s: 'off' }],
    pc: [3, 0],
  };
  if (h === 'none') { S.devs = []; S.profiles = []; S.profile = 0; S.presets = []; S.pc = [0, 0]; }
  if (h === 'many') for (let i = 7; i < 30; i++)
    S.devs.push({ id: i, n: i % 3 ? 'Device ' + i : 'A very long device name that does not fit on one line, number ' + i, s: ['on', 'off', 'connecting', 'nosup', 'own', 'pro'][i % 6] });
  if (h === 'pro') S.devs.push({ k: 'lights_enabled', n: 'AiDot bulbs', s: 'pro' }, { id: 8, n: 'Govee lamp', s: 'pro' });
  const wasOn = {};   // a [dev.N] device's state when it was switched off, given back when switched on again
  let prev = 'aurora', opening = true;
  const L = [], sent = window.MOCK_SENT = [];
  const emit = d => setTimeout(() => L.forEach(f => f({ data: JSON.parse(JSON.stringify(d)) })), 0);
  const state = (ms = 40) => setTimeout(() => emit(S), ms);
  const palOf = id => PAL[id] || ['#888888'];
  window.mockOpen = () => { opening = true; emit(S); emit({ type: 'open' }); };   // a click on the icon, the page kept
  window.mockState = () => emit(S);                                              // a change made elsewhere
  window.chrome = { webview: {
    addEventListener(t, f) { if (t === 'message') L.push(f); },
    postMessage(s) {
      const o = JSON.parse(s); sent.push(o);
      if (o.cmd === 'hello') { setTimeout(() => { emit(S); emit({ type: 'open' }); }, 30); return; }
      if (o.cmd === 'height') { if (opening) { opening = false; emit({ type: 'shown', side: 'b' }); } return; }
      if (o.cmd === 'close' || o.cmd === 'menu') { emit({ type: 'closed' }); return; }
      if (o.cmd === 'get') return state(0);
      if (window.mockSilent) return;
      if (o.cmd === 'power') { if (S.fx === S.off) { S.fx = prev; S.pal = palOf(prev); } else { prev = S.fx; S.fx = S.off; } }
      if (o.cmd === 'brightness') S.bri = Math.max(0, Math.min(100, +o.v));
      if (o.cmd === 'effect') { S.fx = o.id; S.preset = 0; S.pal = palOf(o.id); S.recent = [o.id].concat(S.recent.split(',').filter(x => x && x !== o.id)).slice(0, 5).join(','); }   // as app_fx_recent
      if (o.cmd === 'set' && o.s === 'general' && o.k === 'tray_pin') S.dpin = o.v;
      if (o.cmd === 'preset') { const p = S.presets.find(x => x.id === o.id); if (p) { S.fx = p.fx; S.preset = p.id; S.pal = p.pal.match(/#[0-9a-f]{6}/gi); } }
      if (o.cmd === 'profile') {
        if (window.mockProfFail) S.prof = { err: window.mockProfFail, id: window.mockProfFail === 2 ? S.profile : o.id, n: S.prof.n + 1 };
        else { S.profile = o.id; S.prof = { err: 0, id: 0, n: S.prof.n }; return state(120); }
      }
      if (o.cmd === 'toggle') { const d = S.devs.find(x => x.k === o.k); if (d) d.s = d.s === 'off' ? (S.pro_off && d.k !== 'ram_enabled' ? 'pro' : 'on') : 'off'; }
      if (o.cmd === 'set' && /^dev\.\d+$/.test(o.s) && o.k === 'enabled') {
        const d = S.devs.find(x => x.id === +o.s.slice(4));
        if (d) { if (o.v === '0') { if (d.s !== 'off') wasOn[d.id] = d.s; d.s = 'off'; } else d.s = wasOn[d.id] || 'connecting'; }
      }
      if (o.cmd === 'pc_own') S.pc[1] = o.v === '1' ? S.pc[0] : 0;
      state();
    },
  } };
})();
