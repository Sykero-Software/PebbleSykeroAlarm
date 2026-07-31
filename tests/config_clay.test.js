// SPDX-License-Identifier: GPL-3.0-only
const test = require('node:test');
const assert = require('node:assert');
const { buildConfig } = require('../src/pkjs/config_clay.js');
const { NUMERIC_KEYS, BOOL_KEYS, buildDict } = require('../src/pkjs/index.js');

function allItems(items, out) {
  out = out || [];
  for (const it of items) {
    out.push(it);
    if (it.items) allItems(it.items, out);
  }
  return out;
}

const items = allItems(buildConfig());
const keyed = items.filter((i) => i.messageKey);
const byKey = new Map(keyed.map((i) => [i.messageKey, i]));

test('every settable key appears exactly once', () => {
  const seen = new Set();
  for (const i of keyed) {
    assert.ok(!seen.has(i.messageKey), 'duplicate key ' + i.messageKey);
    seen.add(i.messageKey);
  }
});

test('every radiogroup and select option value is a STRING', () => {
  // A non-string value makes Clay's radiogroup manipulator call value.replace(),
  // which throws and silently aborts rendering at that item — the page then has
  // no Save button and nothing can be saved.
  for (const i of items) {
    if (i.type === 'radiogroup' || i.type === 'select') {
      for (const o of i.options || []) {
        assert.strictEqual(typeof o.value, 'string',
          i.messageKey + ' option value must be a string, got ' + typeof o.value);
      }
      if (i.defaultValue !== undefined) {
        assert.strictEqual(typeof i.defaultValue, 'string',
          i.messageKey + ' defaultValue must be a string');
      }
    }
  }
});

test('section sub-labels are type:text, never a second heading', () => {
  // Every type:'heading' starts a new accordion row, so a sub-label written as a
  // heading splits the section in two.
  for (const s of items.filter((i) => i.type === 'section')) {
    const headings = (s.items || []).filter((i) => i.type === 'heading');
    assert.ok(headings.length <= 1,
      'a section has ' + headings.length + ' headings; sub-labels must be type:text');
  }
});

test('every numeric key in the config is in NUMERIC_KEYS', () => {
  const numericTypes = new Set(['slider']);
  for (const i of keyed) {
    if (numericTypes.has(i.type)) {
      assert.ok(NUMERIC_KEYS.includes(i.messageKey),
        i.messageKey + ' is a slider but is not in NUMERIC_KEYS');
    }
  }
});

test('NUMERIC_KEYS and BOOL_KEYS only name keys the page actually has', () => {
  for (const k of NUMERIC_KEYS) {
    assert.ok(byKey.has(k), 'NUMERIC_KEYS names unknown key ' + k);
  }
  for (const k of BOOL_KEYS) {
    assert.ok(byKey.has(k), 'BOOL_KEYS names unknown key ' + k);
  }
});

test('buildDict converts strings to ints and leaves AlarmSet a string', () => {
  const settings = {
    Slot1On: { value: true }, Slot1Time: { value: '07:00' },
    Slot1Days: { value: [true, true, true, true, true, false, false] },
    SmartEnabled: { value: true },
    SmartWindowMin: { value: '30' },
    TimeSemantics: { value: '0' },
    Sensitivity: { value: '3' },
    SensPercentile: { value: '88' },
    SensMinutes: { value: '3' },
    WakeProfile: { value: '1' },
    SnoozeMin: { value: '10' },
    SnoozeMax: { value: '5' },
    StopGesture: { value: '1' },
    LightPulse: { value: true },
    DstCheck: { value: false },
    IdleExitSec: { value: '15' },
  };
  const dict = buildDict(settings);
  assert.strictEqual(dict.AlarmSet, '07:00|1111100');
  assert.strictEqual(dict.SmartWindowMin, 30);
  assert.strictEqual(typeof dict.SmartWindowMin, 'number');
  assert.strictEqual(dict.SensPercentile, 88);
  assert.strictEqual(dict.Sensitivity, 3);
  assert.strictEqual(dict.SmartEnabled, 1);
  assert.strictEqual(dict.DstCheck, 0);
  // a missing key is omitted rather than sent as NaN
  const sparse = buildDict({ Slot1Time: { value: '06:00' }, Slot1On: { value: true },
                             Slot1Days: { value: [] } });
  assert.strictEqual(sparse.SmartWindowMin, undefined);
  assert.ok(!Object.values(sparse).some((v) => typeof v === 'number' && isNaN(v)));
});
