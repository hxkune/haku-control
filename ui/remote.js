// SPDX-License-Identifier: GPL-3.0-only
'use strict';
// Remote control: when this page is opened from a phone (served by haku control's own small web server),
// it has no WebView2 bridge. This file provides one on top of the HTTP API:
//   page -> core: POST /api/cmd          core -> page: GET /api/state, /api/status (1 s), /api/frame (~8/s)
// The first visit asks for the PIN shown on the PC (Settings -> Phone) and keeps the token it gets.
(function () {
  const listeners = [];
  const emit = data => listeners.forEach(f => f({ data }));
  let token = '';
  try { token = localStorage.getItem('haku_token') || ''; } catch (e) { }
  const headers = () => token ? { 'X-Haku-Token': token } : {};

  async function api(path, body) {
    const r = await fetch(path, body ? { method: 'POST', headers: { ...headers(), 'Content-Type': 'application/json' }, body }
                                     : { headers: headers(), cache: 'no-store' });
    if (r.status === 401) { askPin(); throw new Error('pair'); }
    if (!r.ok) throw new Error(r.status);
    return r.json();
  }

  let polling = false;
  async function loadState() { emit(await api('/api/state')); startPolling(); }
  function startPolling() {
    if (polling) return;
    polling = true;
    let frameBusy = false, statusBusy = false;
    setInterval(async () => {
      if (document.hidden || statusBusy) return;
      statusBusy = true;
      try { emit(await api('/api/status')); setOffline(false); } catch (e) { setOffline(e.message !== 'pair'); } finally { statusBusy = false; }
    }, 1000);
    setInterval(async () => {
      if (document.hidden || frameBusy) return;
      frameBusy = true;
      try { emit(await api('/api/frame')); } catch (e) { } finally { frameBusy = false; }
    }, 125);
  }

  const REFRESH = ['dev_add', 'dev_remove', 'remote_pin', 'remote_forget'];
  const LOCAL_ONLY = ['open', 'quit', 'close', 'autostart'];
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    postMessage(s) {
      const o = JSON.parse(s);
      if (o.cmd === 'hello' || o.cmd === 'get') { loadState().catch(() => { }); return; }
      if (LOCAL_ONLY.includes(o.cmd)) return;
      api('/api/cmd', s).then(() => { if (REFRESH.includes(o.cmd)) return loadState(); }).catch(() => { });
    },
    addEventListener(type, f) { if (type === 'message') listeners.push(f); },
  };
  document.documentElement.classList.add('remote');

  // ---- connection lost banner
  let banner;
  function setOffline(off) {
    if (!banner) {
      banner = document.createElement('div');
      banner.className = 'remote-offline hidden';
      document.body.appendChild(banner);
    }
    banner.textContent = window.t ? t('remote.offline') : 'PC not reachable';
    banner.classList.toggle('hidden', !off);
  }

  // ---- pairing
  // the QR code on the PC links to /#pin=123456: pair with it straight away, and drop it from the address bar
  let linkPin = (/[#&]pin=(\d{6})/.exec(location.hash) || [])[1] || '';
  if (location.hash) history.replaceState(null, '', location.pathname + location.search);
  let pinBox;
  function askPin() {
    if (pinBox) { pinBox.classList.remove('hidden'); return; }
    const tt = (k, d) => (window.t ? t(k) : d);
    pinBox = document.createElement('div');
    pinBox.className = 'wizard';
    pinBox.innerHTML = `<div class="card wiz pin-card">
        <div class="wz-mark"></div>
        <h2>${tt('remote.pair.title', 'Connect to your PC')}</h2>
        <p class="muted">${tt('remote.pair.text', 'Enter the PIN shown in haku control on your PC: Settings → Phone.')}</p>
        <input type="text" inputmode="numeric" maxlength="6" autocomplete="one-time-code" class="pin-in" placeholder="000000">
        <p class="note pin-msg"></p>
        <button class="btn primary pin-go"><span>${tt('remote.pair.go', 'Connect')}</span></button>
      </div>`;
    document.body.appendChild(pinBox);
    const input = pinBox.querySelector('.pin-in'), msg = pinBox.querySelector('.pin-msg');
    const go = async () => {
      const pin = input.value.replace(/\D/g, '');
      if (pin.length !== 6) { input.focus(); return; }
      const r = await fetch('/api/pair', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ pin }) });
      if (r.ok) {
        token = (await r.json()).token;
        try { localStorage.setItem('haku_token', token); } catch (e) { }
        pinBox.classList.add('hidden');
        loadState().catch(() => { });
      } else {
        msg.textContent = r.status === 429 ? tt('remote.pair.wait', 'Too many tries — wait 30 s.') : tt('remote.pair.bad', 'Wrong PIN. Check the number on the PC.');
        input.select();
      }
    };
    pinBox.querySelector('.pin-go').addEventListener('click', go);
    input.addEventListener('keydown', e => { if (e.key === 'Enter') go(); });
    if (linkPin) { input.value = linkPin; linkPin = ''; go(); }
    else setTimeout(() => input.focus(), 50);
  }
})();
