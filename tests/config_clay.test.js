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

function isNumericOptionValue(v) {
  return typeof v === 'string' && /^-?\d+$/.test(v);
}

test('every numeric-valued control (slider, or an all-numeric select) is in NUMERIC_KEYS', () => {
  // A slider is unambiguously numeric. A select is numeric only if ALL its
  // option values parse as an integer -- a select with even one non-numeric
  // option (there are none today, but the rule must be honest) must NOT be
  // forced into NUMERIC_KEYS, since buildDict would parseInt it to NaN and
  // silently drop it. Covering 'select' here (not just 'slider') closes the
  // hole a plain `numericTypes = new Set(['slider'])` check leaves: 8 of this
  // page's 23 NUMERIC_KEYS (TimeSemantics, Sensitivity, SensMinutes,
  // WakeProfile, SnoozeMin, SnoozeMax, StopGesture, IdleExitSec) are
  // select-typed, not sliders, and a select silently missing from
  // NUMERIC_KEYS is exactly the "silently dead control" bug class this
  // project has hit before (a sibling watchface shipped it).
  for (const i of keyed) {
    if (i.type === 'slider') {
      assert.ok(NUMERIC_KEYS.includes(i.messageKey),
        i.messageKey + ' is a slider but is not in NUMERIC_KEYS');
    } else if (i.type === 'select') {
      const opts = i.options || [];
      const allNumeric = opts.length > 0 && opts.every((o) => isNumericOptionValue(o.value));
      if (allNumeric) {
        assert.ok(NUMERIC_KEYS.includes(i.messageKey),
          i.messageKey + ' is an all-numeric select but is not in NUMERIC_KEYS');
      } else {
        assert.ok(!NUMERIC_KEYS.includes(i.messageKey),
          i.messageKey + ' has a non-numeric option value but IS in NUMERIC_KEYS -- '
          + 'buildDict would parseInt it to NaN and silently drop it');
      }
    }
  }
});

test('every toggle that is a real settable key (not an alarm-slot toggle folded into AlarmSet) is in BOOL_KEYS', () => {
  // Slot<N>On toggles are deliberately excluded: they are not sent as their
  // own message key at all (there is no MESSAGE_KEY_Slot1On etc. -- see
  // package.json), they are packed into the AlarmSet string by
  // slotsFromSettings/packAlarmSet instead. Every OTHER toggle on this page
  // (SmartEnabled, LightPulse, DstCheck) is a real message key and must
  // convert through BOOL_KEYS or the watch never hears it.
  for (const i of keyed) {
    if (i.type === 'toggle' && !/^Slot\d+On$/.test(i.messageKey)) {
      assert.ok(BOOL_KEYS.includes(i.messageKey),
        i.messageKey + ' is a toggle but is not in BOOL_KEYS');
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
