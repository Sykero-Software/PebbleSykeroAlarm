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

test('alarm-slot reveal (Task 8) is unaffected by this task\'s additions', () => {
  const c = render();
  // Defaults: slot1 already has a time ('07:00') -> slot2 also revealed;
  // slot2 has no time -> slot3 stays hidden.
  assert.strictEqual(c.byId['slot1-time'].shown, true);
  assert.strictEqual(c.byId['slot2-time'].shown, true);
  assert.strictEqual(c.byId['slot3-time'].shown, false);
});
