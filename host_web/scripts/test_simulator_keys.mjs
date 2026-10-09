// 验证单键 BOOT 虚拟按键在 pointer 事件、键盘事件和取消场景下的按下/释放传递。
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { tr, setText } from '../i18n.js';

const source = readFileSync(new URL('../simulator-ui.js', import.meta.url), 'utf8');
const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
assert.ok(html.indexOf('id="simBoot"') > 0);
assert.ok(html.indexOf('id="simSel"') < 0, '旧 SEL 键不应存在');
assert.ok(html.indexOf('id="simBack"') < 0, '旧 BACK 键不应存在');
let now = 0;
const timers = new Map();
const calls = [];
const target = () => ({
  listeners: {}, disabled: false, hidden: false,
  setAttribute(name, value) { this[name] = value; },
  addEventListener(name, handler) { this.listeners[name] = handler; },
  setPointerCapture() {}, contains() { return false; },
});
const buttons = { simBoot: target(), simPortalToggle: target(), simPortalPanel: target() };
const device = target();
const window = target();
const context = vm.createContext({
  tr, setText,
  runtime: {}, failed: false, running: () => true, heldKeys: new Map(),
  performance: { now: () => now },
  setTimeout(fn, delay) { const id = {}; timers.set(id, { fn, at: now + delay }); return id; },
  clearTimeout(id) { timers.delete(id); },
  press: (key, long) => calls.push([key, long ? 1 : 0]), window,
  loadPortal: () => {}, refreshVisibility: () => {},
  document: { getElementById: id => buttons[id], querySelector: () => device },
});
vm.runInContext(source.slice(source.indexOf('function cancelKey('), source.indexOf("document.getElementById('simReset').addEventListener")), context);
const advance = ms => {
  now += ms;
  for (const [id, timer] of timers) if (timer.at <= now) { timers.delete(id); timer.fn(); }
};

// 短按：pointerdown → 立即 press(0,1) → pointerup → press(0,0)
const down = () => buttons.simBoot.listeners.pointerdown({ button: 0, isPrimary: true, pointerId: 1 });
down();
assert.deepEqual(calls.at(-1), [0, 1], 'BOOT pointerdown emits pressed');
advance(100);
buttons.simBoot.listeners.pointerup();
assert.deepEqual(calls.at(-1), [0, 0], 'BOOT pointerup emits released');
calls.length = 0;

// 取消事件仍需通知 WASM 释放，避免卡在按住态
down();
advance(50);
for (const cancel of ['pointercancel', 'lostpointercapture']) {
  down();
  advance(50);
  buttons.simBoot.listeners[cancel]();
  assert.deepEqual(calls.at(-1), [0, 0], `${cancel} must release BOOT`);
  calls.length = 0;
}
down();
advance(50);
window.listeners.blur();
assert.deepEqual(calls.at(-1), [0, 0], 'blur must release BOOT');
calls.length = 0;

// 键盘 B 键：keydown → press(0,1)，keyup → press(0,0)
const bootKeyEvent = { target: { matches: () => false }, code: 'KeyB', repeat: false, preventDefault() {} };
device.listeners.keydown(bootKeyEvent);
assert.deepEqual(calls.at(-1), [0, 1], 'KeyB keydown emits pressed');
device.listeners.keydown({ ...bootKeyEvent, repeat: true });
advance(100);
assert.equal(calls.length, 1, 'KeyB autorepeat must not re-emit press');
device.listeners.keyup(bootKeyEvent);
assert.deepEqual(calls.at(-1), [0, 0], 'KeyB keyup emits released');

// 已删除的 KeyK/KeyL 不应有任何绑定
assert.ok(!('KeyK' in (device.listeners.keydown_codes || {})), 'KeyK should not be bound');
assert.ok(!('KeyL' in (device.listeners.keydown_codes || {})), 'KeyL should not be bound');

// Portal 切换保持可用
buttons.simPortalToggle.listeners.click({currentTarget: buttons.simPortalToggle});
assert.equal(device.hidden, true);
assert.equal(buttons.simPortalPanel.hidden, false);
buttons.simPortalToggle.listeners.click({currentTarget: buttons.simPortalToggle});
assert.equal(device.hidden, false);
assert.equal(buttons.simPortalPanel.hidden, true);
assert.equal(buttons.simPortalToggle['aria-pressed'], 'false');
console.log('Single-key BOOT simulator input and portal tests passed.');
