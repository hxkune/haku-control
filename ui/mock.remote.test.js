// SPDX-License-Identifier: GPL-3.0-only
// Checks for the phone bridge (ui/remote.js) against a fake PC: node ui/mock.remote.test.js (not shipped: build.cmd drops mock.*)
'use strict';
const fs = require('fs'), path = require('path'), assert = require('assert');

const el = () => ({ className: '', textContent: '', classList: { add() { }, remove() { }, toggle() { } }, setAttribute() { }, appendChild() { } });
const timers = [];
global.window = global;
global.document = { hidden: false, body: el(), documentElement: el(), createElement: el };
global.location = { hash: '', pathname: '/', search: '' };
global.history = { replaceState() { } };
global.localStorage = { getItem: () => 'tok', setItem() { } };
global.setInterval = f => timers.push(f);
global.setTimeout = () => 0; global.clearTimeout = () => { };

// the fake PC: what it answers, and what it was asked
const pc = { rev: 1, up: true, profile: '1', calls: [] };
global.fetch = async (url, o = {}) => {
  pc.calls.push(url + (o.body ? ' ' + o.body : ''));
  if (!pc.up) throw new Error('offline');
  const body = url === '/api/state' ? { type: 'state', rev: pc.rev, cfg: { general: { profile: pc.profile } } }
    : url === '/api/status' ? { type: 'status', rev: pc.rev } : url === '/api/cmd' && /"cmd":"open"/.test(o.body) ? null : {};
  return { ok: !!body, status: body ? 200 : 400, json: async () => body };
};
eval(fs.readFileSync(path.join(__dirname, 'remote.js'), 'utf8'));

const got = [];
window.chrome.webview.addEventListener('message', e => got.push(e.data));
const tick = async () => { await timers[0](); await new Promise(r => setImmediate(r)); };
const states = () => got.filter(d => d.type === 'state').length;

(async () => {
  window.chrome.webview.postMessage(JSON.stringify({ cmd: 'hello' }));
  await new Promise(r => setImmediate(r));
  assert.equal(states(), 1, 'hello loads the state');

  await tick();
  assert.equal(states(), 1, 'a plain status poll does not reload');

  pc.rev = 2; pc.profile = '3';   // the PC switched profile (clock, program, or this phone)
  await tick();
  assert.equal(states(), 2, 'a new rev reloads the state');
  assert.equal(got[got.length - 1].cfg.general.profile, '3', 'the page gets the new profile');

  pc.up = false; await tick(); await tick();
  pc.up = true; await tick();
  assert.equal(states(), 3, 'the PC back after being away reloads the state');
  await tick();
  assert.equal(states(), 3, 'and only once');

  window.chrome.webview.postMessage(JSON.stringify({ cmd: 'quit' }));
  assert.ok(!pc.calls.some(c => /quit/.test(c)), 'PC-only commands stay on the phone');
  console.log('remote: all ok');
})().catch(e => { console.error('remote: FAIL', e.message); process.exit(1); });
