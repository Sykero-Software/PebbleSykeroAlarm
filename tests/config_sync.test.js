// SPDX-License-Identifier: GPL-3.0-only
//
// The CfgRequest reply path (whole-branch review's Critical 2). The watch asks
// for its config on launch because a `webviewclosed` send only lands if the
// watchapp happened to be running at Save time -- and the alarm times live only
// on the phone, so losing that message means the alarm rings at the wrong time
// with nothing on either side reporting a problem.
//
// Requires src/pkjs to be generated (npm test's pretest runs tsc).

const test = require('node:test');
const assert = require('node:assert');

const { saveDict, resendDict, CFG_STORE_KEY } = require('../src/pkjs/config_sync.js');

function store() {
  const m = {};
  return {
    get: (k) => (Object.prototype.hasOwnProperty.call(m, k) ? m[k] : null),
    set: (k, v) => { m[k] = v; },
    raw: m,
  };
}

test('nothing ever saved -> null, so the phone stays silent', () => {
  const s = store();
  // Silence is the point: an empty reply would clobber the watch's own
  // persisted config with nothing.
  assert.strictEqual(resendDict(s.get), null);
});

test('a saved dict round-trips exactly, values and types intact', () => {
  const s = store();
  const dict = {
    AlarmSet: '06:45|1111100;-08:30|0000011',
    SmartEnabled: 1, SmartWindowMin: 30, TimeSemantics: 1,
    Sensitivity: 2, SensPercentile: 88, SensMinutes: 3,
    WakeProfile: 0, SnoozeMin: 9, SnoozeMax: 0,
    StopGesture: 2,
  };
  saveDict(s.set, dict);
  const back = resendDict(s.get);
  assert.deepStrictEqual(back, dict);
  // The alarm times specifically -- the field whose loss is the Critical.
  assert.strictEqual(back.AlarmSet, '06:45|1111100;-08:30|0000011');
  // Ints must come back as ints: the watch reads value->int32, and a string
  // would be misread as garbage.
  assert.strictEqual(typeof back.SmartWindowMin, 'number');
});

test('a later save replaces the earlier one', () => {
  const s = store();
  saveDict(s.set, { AlarmSet: '07:00|1111100' });
  saveDict(s.set, { AlarmSet: '06:45|1111100' });
  assert.strictEqual(resendDict(s.get).AlarmSet, '06:45|1111100');
});

test('a corrupt or truncated store -> null, never garbage', () => {
  const s = store();
  s.set(CFG_STORE_KEY, '{"AlarmSet":"07:00|11111');
  assert.strictEqual(resendDict(s.get), null);
  s.set(CFG_STORE_KEY, '');
  assert.strictEqual(resendDict(s.get), null);
});

test('a stored dict without AlarmSet is not usable', () => {
  const s = store();
  // Every other setting present but no alarm set: replying would change the
  // whole configuration while leaving the watch on its seeded demo alarms.
  s.set(CFG_STORE_KEY, JSON.stringify({ SmartEnabled: 1, SnoozeMin: 9 }));
  assert.strictEqual(resendDict(s.get), null);
  s.set(CFG_STORE_KEY, JSON.stringify({ AlarmSet: 42 }));
  assert.strictEqual(resendDict(s.get), null);
  s.set(CFG_STORE_KEY, 'null');
  assert.strictEqual(resendDict(s.get), null);
});

test('an empty alarm set IS a legitimate saved value (all alarms off)', () => {
  const s = store();
  saveDict(s.set, { AlarmSet: '', SnoozeMin: 9 });
  const back = resendDict(s.get);
  assert.notStrictEqual(back, null);
  assert.strictEqual(back.AlarmSet, '');
});

test('the reply never carries CfgRequest back to the watch', () => {
  const s = store();
  // buildDict must not produce it: echoing CfgRequest would invite a reply loop.
  // (CfgOpen was removed with the idle auto-exit it existed to pause -- it is no
  // longer a message key at all, so there is nothing left to assert about it.)
  const { buildDict } = require('../src/pkjs/index.js');
  const dict = buildDict({});
  assert.strictEqual(dict.CfgRequest, undefined);
  saveDict(s.set, dict);
  const back = resendDict(s.get);
  assert.strictEqual(back.CfgRequest, undefined);
});
