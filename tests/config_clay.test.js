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

test('TimeSemantics offers all three modes, and says which way each moves', () => {
  // The C side reads this as SEMANTICS_RING_LATEST(0) / AWAKE_BY(1) /
  // RING_FROM(2). A value that is not one of those falls through
  // ac_ring_deadline's default and silently becomes "the latest", so a typo here
  // is invisible on the watch. The wording matters as much as the values: the
  // two-option version said "Ringing starts then", which a real user read as
  // "not before then" when it means the opposite, and got woken 30 minutes early
  // (2026-08-01). Each label must therefore state which direction the smart
  // window can move the ring.
  const sem = byKey.get('TimeSemantics');
  assert.ok(sem, 'TimeSemantics missing from the config page');
  const values = sem.options.map((o) => o.value).sort();
  assert.deepStrictEqual(values, ['0', '1', '2']);
  const latest = sem.options.find((o) => o.value === '0').label;
  const from = sem.options.find((o) => o.value === '2').label;
  assert.match(latest, /earlier/i);
  assert.match(from, /later/i);
  // ... and the description must show both ends of a concrete example, since the
  // label alone cannot express "07:20 to 07:50".
  assert.match(sem.description, /07:20/);
  assert.match(sem.description, /08:20/);
});

test('every checkboxgroup option is a plain STRING', () => {
  // Clay's checkboxgroup template renders each option with {{{this}}} and its
  // README specifies "options | array of strings". A {label, value} object
  // therefore renders as the literal text "[object Object]" next to every
  // checkbox — which is exactly what a real phone showed for the weekday
  // Repeat field on 2026-07-31. The config page cannot render headless, so this
  // assertion is the only thing standing between that bug and a user.
  //
  // The page HAS no checkboxgroup any more: the weekday checkboxes moved inside
  // the alarmList component, which renders its own <input type="checkbox"> and
  // cannot hit Clay's template at all. The loop is kept so re-adding a
  // checkboxgroup is still covered, and the count is asserted as zero rather than
  // dropped, so this deliberate change reads as deliberate.
  let seen = 0;
  for (const i of items) {
    if (i.type === 'checkboxgroup') {
      for (const o of i.options || []) {
        assert.strictEqual(typeof o, 'string',
          i.messageKey + ' checkboxgroup option must be a plain string, got ' +
          typeof o + ' (' + JSON.stringify(o) + ')');
        seen++;
      }
    }
  }
  assert.strictEqual(seen, 0,
    'the page is expected to have no checkboxgroup (the weekdays live in the '
    + 'alarmList component); if one was added, keep it to plain-string options');
});

test('the alarm list is one alarmList component, not per-slot items', () => {
  // The regression this guards: the eight Slot<N>{On,Time,Days} blocks were only
  // ever revealed one at a time (slot i visible iff slot i-1 had a time), so the
  // page looked like it held two alarms rather than eight. Reported 2026-07-31.
  const list = items.filter((i) => i.type === 'alarmList');
  assert.strictEqual(list.length, 1, 'expected exactly one alarmList component');
  assert.strictEqual(list[0].messageKey, 'AlarmList');
  assert.strictEqual(typeof list[0].defaultValue, 'string',
    "the component's value is the AlarmSet wire string, so defaultValue must be one");
  const slotItems = keyed.filter((i) => /^Slot\d+(On|Time|Days)$/.test(i.messageKey));
  assert.strictEqual(slotItems.length, 0,
    'per-slot alarm items are replaced by the alarmList component, found: '
    + slotItems.map((i) => i.messageKey).join(', '));
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
  // page's NUMERIC_KEYS (TimeSemantics, Sensitivity, SensMinutes,
  // WakeProfile, SnoozeMin, SnoozeMax, StopGesture) are
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

test('every toggle is in BOOL_KEYS', () => {
  // Every toggle on this page (SmartEnabled, EscRampVib, LightPulse, DstCheck) is
  // a real message key and must convert through BOOL_KEYS or the watch never
  // hears it. The per-alarm enabled flags are not toggles any more -- they are
  // buttons inside the alarmList component, folded into the AlarmSet string.
  for (const i of keyed) {
    if (i.type === 'toggle') {
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
    AlarmList: { value: '07:00|1111100' },
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
  const sparse = buildDict({ AlarmList: { value: '06:00|0000000' } });
  assert.strictEqual(sparse.SmartWindowMin, undefined);
  assert.ok(!Object.values(sparse).some((v) => typeof v === 'number' && isNaN(v)));
});

test('buildDict canonicalizes the list value and drops malformed entries', () => {
  // The component reads raw DOM, so its value must not be trusted verbatim: it
  // goes through the same host-tested packer the watch's parser is contract-tested
  // against. Garbage must be dropped here, not parsed on an alarm clock.
  assert.strictEqual(buildDict({ AlarmList: { value: '7:00|1111100' } }).AlarmSet,
    '07:00|1111100', 'a single-digit hour is normalized');
  assert.strictEqual(buildDict({ AlarmList: { value: '25:00|1111111' } }).AlarmSet,
    '', 'an impossible hour is dropped');
  assert.strictEqual(buildDict({ AlarmList: { value: '07:00|111' } }).AlarmSet,
    '', 'a short weekday mask is dropped');
  assert.strictEqual(
    buildDict({ AlarmList: { value: '07:00|1111100;nonsense;-08:30|0000011' } }).AlarmSet,
    '07:00|1111100;-08:30|0000011', 'a bad entry is dropped, the good ones survive');
  assert.strictEqual(buildDict({ AlarmList: { value: '' } }).AlarmSet, '',
    'no alarms is a legitimate value, not a reason to fall back to the legacy path');
});

test('buildDict still reads legacy per-slot settings when there is no list value', () => {
  // A phone whose clay-settings predates the list component and whose migration
  // has not run yet must not lose its alarms.
  const dict = buildDict({
    Slot1On: { value: true }, Slot1Time: { value: '06:45' },
    Slot1Days: { value: [true, true, true, true, true, false, false] },
  });
  assert.strictEqual(dict.AlarmSet, '06:45|1111100');
});
