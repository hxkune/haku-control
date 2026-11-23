// SPDX-License-Identifier: GPL-3.0-only
// Checks for the phone bridge (ui/remote.js) against a fake PC: node ui/mock.remote.test.js (not shipped: build.cmd drops mock.*)
'use strict';
const fs = require('fs'), path = require('path'), assert = require('assert');

// querySelector, events, focus and select: enough for the PIN form
const el = () => ({ className: '', textContent: '', value: '', on: {}, q: {}, classList: { add() { }, remove() { }, toggle() { } }, setAttribute() { }, appendChild() { },
  querySelector(s) { return this.q[s] = this.q[s] || el(); }, addEventListener(t, f) { this.on[t] = f; }, focus() { }, select() { } });
const timers = [];
global.window = global;
global.document = { hidden: false, body: el(), documentElement: el(), createElement: el };
global.location = { hash: '', pathname: '/', search: '' };
global.history = { replaceState() { } };
global.localStorage = { getItem: () => 'tok', setItem() { } };
global.setInterval = f => timers.push(f);
global.setTimeout = () => 0; global.clearTimeout = () => { };

// the fake PC: what it answers, and what it was asked
const pc = { rev: 1, up: true, paired: true, profile: '1', calls: [], prof: { 'profile.1': { name: 'Day' }, 'profile.2': { name: 'Night', pin: '1' } } };
global.fetch = async (url, o = {}) => {
  pc.calls.push(url + (o.body ? ' ' + o.body : ''));
  if (!pc.up) throw new Error('offline');
  if (url === '/api/pair') { pc.paired = true; return { ok: true, status: 200, json: async () => ({ token: 'new' }) }; }
  if (!pc.paired) return { ok: false, status: 401, json: async () => ({}) };
  // what the PC does with a copy (app_profile_dup): the next free number, the source as it was
  if (url === '/api/cmd' && /"cmd":"profile_dup"/.test(o.body)) {
    const c = JSON.parse(o.body); let n = 1; while (pc.prof['profile.' + n]) n++;
    pc.prof['profile.' + n] = { ...pc.prof['profile.' + c.id], name: c.name, pin: undefined }; pc.rev++;
  }
  const body = url === '/api/state' ? { type: 'state', rev: pc.rev, cfg: { general: { profile: pc.profile }, ...pc.prof } }
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

  // Duplicate on the phone: the PC gets it, and the copy shows up next to the original
  window.chrome.webview.postMessage(JSON.stringify({ cmd: 'profile_dup', id: '2', name: 'Night (copy)' }));
  await new Promise(r => setImmediate(r));
  assert.ok(pc.calls.some(c => /\/api\/cmd .*"cmd":"profile_dup","id":"2"/.test(c)), 'the copy is asked of the PC');
  await tick();
  const cfg = got[got.length - 1].cfg;
  assert.equal(cfg['profile.3'] && cfg['profile.3'].name, 'Night (copy)', 'the phone shows the copy');
  assert.equal(cfg['profile.2'].name, 'Night', 'and the original is still there');

  window.chrome.webview.postMessage(JSON.stringify({ cmd: 'quit' }));
  assert.ok(!pc.calls.some(c => /quit/.test(c)), 'PC-only commands stay on the phone');
  // pairing: the PC gone while Connect is pressed says so; a second press while the first is out sends nothing more
  const made = []; document.createElement = () => { const e = el(); made.push(e); return e; };
  pc.paired = false; await tick();
  const box = made.find(e => e.className === 'wizard');
  assert.ok(box, 'a 401 asks for the PIN');
  const pin = box.querySelector('.pin-in'), msg = box.querySelector('.pin-msg'), go = box.querySelector('.pin-go').on.click;
  pin.value = '123456'; pc.up = false;
  await go();
  assert.equal(msg.textContent, 'PC not reachable', 'an unreachable PC is said, not swallowed');
  pc.up = true; const n = pc.calls.length;
  await Promise.all([go(), go()]);
  assert.equal(pc.calls.slice(n).filter(c => c.startsWith('/api/pair')).length, 1, 'one pairing per press: ' + pc.calls.slice(n));
  assert.equal(msg.textContent, '', 'and the old message goes');
  console.log('remote: all ok');
})().catch(e => { console.error('remote: FAIL', e.message); process.exit(1); });
