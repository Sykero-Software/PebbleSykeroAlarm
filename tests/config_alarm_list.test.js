// SPDX-License-Identifier: GPL-3.0-only
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const component = require('../src/pkjs/config_alarm_list.js');
const { migrateAlarmList } = require('../src/pkjs/index.js');
const { CFG_STORE_KEY } = require('../src/pkjs/config_sync.js');

const GENERATED = path.join(__dirname, '..', 'src', 'pkjs', 'config_alarm_list.js');

test('the component exposes what Clay registerComponent needs', () => {
  assert.strictEqual(component.name, 'alarmList');
  assert.strictEqual(typeof component.template, 'string');
  assert.strictEqual(typeof component.style, 'string');
  assert.strictEqual(typeof component.initialize, 'function');
  assert.strictEqual(typeof component.manipulator.get, 'function');
  assert.strictEqual(typeof component.manipulator.set, 'function');
});

test('the component is SELF-CONTAINED: no TS downlevel helper, no module scope', () => {
  // Clay serializes the component object with toSource() and re-evals it inside the
  // config webview. Anything it references from module scope -- a sibling function,
  // or a TypeScript downlevel helper such as __spreadArray/__assign/_this -- is
  // undefined there and throws, which blanks the config page with no Save button.
  // TimeStyle shipped exactly that bug (a manipulator.set calling a module-scope
  // wlNormalize threw ReferenceError in the webview), so this is a real regression
  // guard, not a style check.
  // Scan the WHOLE generated file, comments stripped. Whole-file, because
  // `initialize: alarmListInitialize` pulls that function's body into what gets
  // serialized, and it is emitted above the component literal -- slicing from the
  // literal would skip the code most likely to contain a helper. Comments stripped,
  // because this file's own header names the helpers it must avoid, and matching
  // that would be a permanently failing test that says nothing.
  const src = fs.readFileSync(GENERATED, 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, ' ')
    .replace(/(^|[^:])\/\/[^\n]*/g, '$1');
  for (const helper of ['__spreadArray', '__assign', '__rest', '__values', '__read']) {
    assert.ok(!src.includes(helper),
      'the component references ' + helper + ', a TypeScript downlevel helper that '
      + 'lives in module scope and is undefined in the config webview');
  }
  assert.ok(!/\b_this\b/.test(src),
    'the component captures _this: an arrow function used `this`, which downlevels '
    + 'to a module-scope variable that is undefined in the config webview');
  assert.ok(!src.includes('require('),
    'the component must not require() anything -- it cannot resolve a module in the '
    + 'config webview');
});

test('the row buttons override Clay\'s 12rem min-width', () => {
  // Clay's base theme sets `button { min-width: 12rem }`. Without an override each
  // row button is forced to 12rem, overflows the row and squeezes the time input to
  // zero width -- it looks like "huge buttons, no field". Verified the hard way in
  // TimeStyle's widgetList.
  assert.ok(/\.al-row \.al-line button\{[^}]*min-width:0/.test(component.style),
    'row buttons must set min-width:0');
  assert.ok(/\.al-add\{[^}]*min-width:0/.test(component.style),
    'the Add button must set min-width:0');
  // A raw <input>/<select> renders in the OS LIGHT theme inside the Core app's dark
  // config page unless it is themed explicitly.
  assert.ok(component.style.includes('color-scheme:dark'),
    'the time input and repeat select must be themed dark');
});

test('the template has a list container, an Add button and an empty-state', () => {
  assert.ok(component.template.includes('al-list'));
  assert.ok(component.template.includes('al-add'));
  assert.ok(component.template.includes('al-empty'));
});

// --- migrateAlarmList: a consolidated Clay key inherits nothing on its own, so
// without this the first open of the new page would show defaultValue and saving
// would wipe every alarm the user had.

function store(initial) {
  const m = new Map(Object.entries(initial || {}));
  return {
    get: (k) => (m.has(k) ? m.get(k) : null),
    set: (k, v) => m.set(k, v),
    read: () => JSON.parse(m.get('clay-settings') || '{}'),
    raw: m,
  };
}

test('migration seeds AlarmList from the last dict actually sent to the watch', () => {
  // Preferred source: by definition this is what the watch currently holds.
  const s = store({
    'clay-settings': JSON.stringify({ SmartEnabled: true }),
    [CFG_STORE_KEY]: JSON.stringify({ AlarmSet: '06:45|1111100;-08:30|0000011' }),
  });
  migrateAlarmList(s.get, s.set);
  assert.strictEqual(s.read().AlarmList, '06:45|1111100;-08:30|0000011');
});

test('migration falls back to the legacy per-slot Clay keys', () => {
  // A phone that has config-page state but never completed a send.
  const s = store({
    'clay-settings': JSON.stringify({
      Slot1On: true, Slot1Time: '07:15',
      Slot1Days: [true, true, true, true, true, false, false],
      Slot2On: false, Slot2Time: '09:00',
      Slot2Days: [false, false, false, false, false, true, true],
    }),
  });
  migrateAlarmList(s.get, s.set);
  assert.strictEqual(s.read().AlarmList, '07:15|1111100;-09:00|0000011');
});

test('migration prefers the sent dict over the legacy keys when both exist', () => {
  const s = store({
    'clay-settings': JSON.stringify({ Slot1On: true, Slot1Time: '07:15', Slot1Days: [true] }),
    [CFG_STORE_KEY]: JSON.stringify({ AlarmSet: '05:30|1111111' }),
  });
  migrateAlarmList(s.get, s.set);
  assert.strictEqual(s.read().AlarmList, '05:30|1111111');
});

test('migration is idempotent and never overwrites an existing list', () => {
  const s = store({
    'clay-settings': JSON.stringify({ AlarmList: '04:00|1111111' }),
    [CFG_STORE_KEY]: JSON.stringify({ AlarmSet: '05:30|1111111' }),
  });
  migrateAlarmList(s.get, s.set);
  assert.strictEqual(s.read().AlarmList, '04:00|1111111');
  migrateAlarmList(s.get, s.set);
  assert.strictEqual(s.read().AlarmList, '04:00|1111111');
});

test('migration leaves the store untouched when there is nothing to inherit', () => {
  // A fresh phone: defaultValue is the correct thing to show, so writing an empty
  // AlarmList would be wrong (it would read as "the user has zero alarms").
  const s = store({ 'clay-settings': JSON.stringify({ SmartEnabled: true }) });
  migrateAlarmList(s.get, s.set);
  assert.strictEqual(s.read().AlarmList, undefined);
});

test('migration survives a corrupt store rather than throwing', () => {
  // This runs in showConfiguration; an exception here would mean the config page
  // never opens at all.
  const s = store({ 'clay-settings': 'not json{' });
  assert.doesNotThrow(() => migrateAlarmList(s.get, s.set));
  const s2 = store({
    'clay-settings': JSON.stringify({}),
    [CFG_STORE_KEY]: 'not json{',
  });
  assert.doesNotThrow(() => migrateAlarmList(s2.get, s2.set));
  assert.strictEqual(s2.read().AlarmList, undefined);
});
