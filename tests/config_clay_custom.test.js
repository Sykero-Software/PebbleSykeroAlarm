// SPDX-License-Identifier: GPL-3.0-only
//
// A mocked-Clay test for config_clay_custom.ts, mirroring TimeStylePebble's
// tests/config_clay_custom.test.js pattern: the mock exposes Clay 1.0.4's
// REAL event set (there is NO AFTER_RENDER) and its on() throws exactly the
// way the real Clay does on a non-string event name, so a typo'd event
// constant reproduces the "blank config page, no Save button" crash instead
// of silently passing. See this file's `on()` below and the CLAUDE.md lesson
// it encodes.
const test = require('node:test');
const assert = require('node:assert');
// config_clay_custom.ts uses `export default`, not TS's `export =`, so the
// compiled CommonJS puts the function under .default (esModuleInterop).
const clayConfigCustom = require('../src/pkjs/config_clay_custom.js').default;
const { buildConfig } = require('../src/pkjs/config_clay.js');

function flatten(items, out) {
  out = out || [];
  for (const it of items) {
    if (it.type === 'section' && Array.isArray(it.items)) {
      flatten(it.items, out);
    } else {
      out.push(it);
    }
  }
  return out;
}

function makeItem(cfg) {
  return {
    config: cfg,
    id: cfg.id || null,
    messageKey: cfg.messageKey || null,
    value: cfg.defaultValue,
    shown: true,
    changeHandlers: [],
    get() { return this.value; },
    show() { this.shown = true; },
    hide() { this.shown = false; },
    on(ev, fn) { if (ev === 'change') { this.changeHandlers.push(fn); } },
  };
}

function makeClay(overrides) {
  const items = flatten(buildConfig()).map(makeItem);
  const byId = {};
  items.forEach((it) => { if (it.id) { byId[it.id] = it; } });
  if (overrides) {
    Object.keys(overrides).forEach((id) => {
      if (byId[id]) { byId[id].value = overrides[id]; }
    });
  }
  return {
    // Mirror Clay 1.0.4's real event set. There is NO AFTER_RENDER -- a
    // missing/wrong constant passes undefined into on(), which Clay's real
    // _transformEventNames crashes on (undefined.split). Reproduced below.
    EVENTS: { BEFORE_BUILD: 'BEFORE_BUILD', AFTER_BUILD: 'AFTER_BUILD',
              BEFORE_DESTROY: 'BEFORE_DESTROY', AFTER_DESTROY: 'AFTER_DESTROY' },
    _handlers: {},
    getItemById(id) { return byId[id]; },
    on(ev, fn) {
      if (typeof ev !== 'string') {
        throw new TypeError("Cannot read properties of undefined (reading 'split')");
      }
      this._handlers[ev] = fn;
    },
    byId,
    items,
  };
}

// overrides: { [id]: value } applied to items' initial `.value` before
// AFTER_BUILD fires (as if the webview had pre-filled them from clay-settings).
function render(overrides) {
  const clay = makeClay(overrides);
  clayConfigCustom.call(clay, {});
  assert.ok(clay._handlers.AFTER_BUILD,
    'custom fn must register an AFTER_BUILD handler');
  clay._handlers.AFTER_BUILD();
  return clay;
}

test('registers against Clay\'s REAL event set and does not throw', () => {
  // If config_clay_custom.ts used a non-existent event constant (e.g. the
  // brief's own cautionary AFTER_RENDER), makeClay's on() throws before this
  // line even completes -- this test fails immediately, exactly reproducing
  // the "blank page, no Save button" symptom instead of masking it.
  const c = render();
  assert.ok(c._handlers.AFTER_BUILD, 'AFTER_BUILD handler registered');
});

test('a gated item hides when its gate is off: Custom sensitivity fields hidden at the default (Medium)', () => {
  const c = render();   // default Sensitivity = '1' (Medium), not Custom
  assert.strictEqual(c.byId['sens-pct'].shown, false);
  assert.strictEqual(c.byId['sens-min'].shown, false);
});

test('Custom sensitivity fields shown when Sensitivity == Custom', () => {
  const c = render({ 'smart-sens': '3' });
  assert.strictEqual(c.byId['sens-pct'].shown, true);
  assert.strictEqual(c.byId['sens-min'].shown, true);
});

test('a gated item hides when its gate is off: Custom escalation fields hidden at the default (Normal)', () => {
  const c = render();   // default WakeProfile = '1' (Normal), not Custom
  assert.strictEqual(c.byId['esc-lead'].shown, false);
  assert.strictEqual(c.byId['esc-cap'].shown, false);
});

test('Custom escalation fields shown when Wake style == Custom', () => {
  const c = render({ 'wake-profile': '3' });
  assert.strictEqual(c.byId['esc-lead'].shown, true);
  assert.strictEqual(c.byId['esc-cap'].shown, true);
});

test('smart-alarm detail hidden when the smart alarm is off', () => {
  const c = render({ 'smart-on': false });
  assert.strictEqual(c.byId['smart-window'].shown, false);
  assert.strictEqual(c.byId['smart-semantics'].shown, false);
  assert.strictEqual(c.byId['smart-sens'].shown, false);
});

test('smart-alarm off suppresses Custom sensitivity fields even when Sensitivity==Custom underneath', () => {
  const c = render({ 'smart-on': false, 'smart-sens': '3' });
  assert.strictEqual(c.byId['sens-pct'].shown, false,
    'must not leak visible when the smart alarm itself is off');
});

test('live change: switching Sensitivity to Custom reveals the percentile/duration fields', () => {
  const c = render();
  assert.strictEqual(c.byId['sens-pct'].shown, false);
  c.byId['smart-sens'].value = '3';
  c.byId['smart-sens'].changeHandlers.forEach((fn) => fn());
  assert.strictEqual(c.byId['sens-pct'].shown, true);
  assert.strictEqual(c.byId['sens-min'].shown, true);
});

test('live change: switching Wake style to Custom reveals the escalation fields', () => {
  const c = render();
  assert.strictEqual(c.byId['esc-lead'].shown, false);
  c.byId['wake-profile'].value = '3';
  c.byId['wake-profile'].changeHandlers.forEach((fn) => fn());
  assert.strictEqual(c.byId['esc-lead'].shown, true);
  assert.strictEqual(c.byId['esc-cap'].shown, true);
});

test('live change: turning the smart alarm off hides its detail fields', () => {
  const c = render({ 'smart-on': true });
  assert.strictEqual(c.byId['smart-window'].shown, true);
  c.byId['smart-on'].value = false;
  c.byId['smart-on'].changeHandlers.forEach((fn) => fn());
  assert.strictEqual(c.byId['smart-window'].shown, false);
  assert.strictEqual(c.byId['smart-semantics'].shown, false);
  assert.strictEqual(c.byId['smart-sens'].shown, false);
});

// The per-slot progressive reveal this file used to test is GONE: the alarmList
// component owns its own rows, so there is no slot to show or hide. That also
// removed the reveal's dependence on a `change` event firing from the Android time
// picker, which was never verified and would have stalled it at slot 2.
// config_clay.test.js asserts the page has no Slot<N>* items left.

test('ramp-only Custom fields are hidden when the vibration ramp is off', () => {
  // With the ramp off these four control nothing: the gap it would tighten TO, the
  // time it would tighten OVER, and the pulse length/count it would start FROM.
  // esc-ramp has to be set explicitly: it defaults to ON since 2026-08-20.
  const c = render({ 'wake-profile': '3', 'esc-ramp': false });
  assert.strictEqual(c.byId['esc-min'].shown, false);
  assert.strictEqual(c.byId['esc-tighten'].shown, false);
  assert.strictEqual(c.byId['esc-vibstart'].shown, false);
  assert.strictEqual(c.byId['esc-pstart'].shown, false);
  // ... while the three that ARE the flat buzz stay visible.
  assert.strictEqual(c.byId['esc-lead'].shown, true, 'the constant gap');
  assert.strictEqual(c.byId['esc-vibmax'].shown, true, 'the pulse length');
  assert.strictEqual(c.byId['esc-pmax'].shown, true, 'the pulses per buzz');
});

test('ramp-only Custom fields appear when the vibration ramp is on (the default)', () => {
  const c = render({ 'wake-profile': '3' });   // Custom, esc-ramp defaults to true
  assert.strictEqual(c.byId['esc-min'].shown, true);
  assert.strictEqual(c.byId['esc-tighten'].shown, true);
  assert.strictEqual(c.byId['esc-vibstart'].shown, true);
  assert.strictEqual(c.byId['esc-pstart'].shown, true);
});

test('ramp-only fields stay hidden outside Custom even with the ramp on', () => {
  const c = render({ 'wake-profile': '1', 'esc-ramp': true });
  assert.strictEqual(c.byId['esc-min'].shown, false,
    'must not leak visible when the profile is not Custom');
  assert.strictEqual(c.byId['esc-tighten'].shown, false);
});

test('live change: switching the ramp off hides the ramp-only fields', () => {
  const c = render({ 'wake-profile': '3' });   // ramp on by default
  assert.strictEqual(c.byId['esc-tighten'].shown, true);
  c.byId['esc-ramp'].value = false;
  c.byId['esc-ramp'].changeHandlers.forEach((fn) => fn());
  assert.strictEqual(c.byId['esc-tighten'].shown, false);
  assert.strictEqual(c.byId['esc-vibstart'].shown, false);
});

test('live change: switching the ramp back on reveals them again', () => {
  const c = render({ 'wake-profile': '3', 'esc-ramp': false });
  assert.strictEqual(c.byId['esc-tighten'].shown, false);
  c.byId['esc-ramp'].value = true;
  c.byId['esc-ramp'].changeHandlers.forEach((fn) => fn());
  assert.strictEqual(c.byId['esc-tighten'].shown, true);
  assert.strictEqual(c.byId['esc-vibstart'].shown, true);
});

test('a fixed Save bar is injected, with its clearance on #main-form and NOT on body', () => {
  // Why this matters: an edit left unsaved on a page whose Save button is several
  // screens down is lost silently, which on 2026-07-31 looked exactly like the watch
  // ignoring a deleted alarm. And why #main-form: Clay's base CSS sets
  // html,body{height:100%} with border-box, so a body padding-bottom sits INSIDE the
  // fixed-height body box and gives zero real clearance -- the last setting then sits
  // permanently under the bar. TimeStyle shipped that bug once.
  const appended = [];
  const byId = {};
  global.document = {
    getElementById: (id) => byId[id] || null,
    createElement: () => ({ id: '', textContent: '' }),
    head: { appendChild: (el) => { appended.push(el); byId[el.id] = el; } },
  };
  try {
    const c = render();
    assert.strictEqual(appended.length, 1, 'exactly one <style> appended');
    const css = appended[0].textContent;
    assert.ok(/\.component-submit\{[^}]*position:fixed/.test(css),
      'the Save button must be fixed to the bottom');
    assert.ok(/#main-form\{[^}]*padding-bottom:\s*\d+px/.test(css),
      'clearance must be reserved on #main-form (the in-flow scrolling content)');
    assert.ok(!/(^|[^-])body\s*\{[^}]*padding-bottom/.test(css),
      'clearance must NOT be put on body -- it yields zero real clearance');
    // Idempotent: a second AFTER_BUILD must not stack a second <style>.
    c._handlers.AFTER_BUILD();
    assert.strictEqual(appended.length, 1, 'injection must be idempotent');
  } finally {
    delete global.document;
  }
});
