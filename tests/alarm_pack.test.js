// SPDX-License-Identifier: GPL-3.0-only
//
// CONTRACT WITH THE WATCH: the packed strings this file asserts (leading '-'
// = disabled, "HH:MM|DDDDDDD" with bit0 = Monday) are the exact wire format
// `ac_parse_set` in src/c/alarm_calc.c parses. tests/test_pack_contract.c
// re-parses several of these SAME literal strings through the real C parser
// and asserts the resulting Alarm fields, so the two sides cannot silently
// drift apart. If you change what this file asserts a packed string looks
// like, update tests/test_pack_contract.c's literals to match (and vice
// versa) -- see that file's own header comment.
const test = require('node:test');
const assert = require('node:assert');
const { packAlarmSet, unpackAlarmSet, SLOT_COUNT } = require('../src/pkjs/alarm_pack.js');

const D = (s) => s.split('').map((c) => c === '1');

test('SLOT_COUNT is 8', () => {
  assert.strictEqual(SLOT_COUNT, 8);
});

test('packs enabled and disabled slots', () => {
  const packed = packAlarmSet([
    { enabled: true,  time: '07:00', days: D('1111100') },
    { enabled: false, time: '08:30', days: D('0000011') },
  ]);
  assert.strictEqual(packed, '07:00|1111100;-08:30|0000011');
});

test('skips slots with no time set', () => {
  const packed = packAlarmSet([
    { enabled: true, time: '',      days: D('1111111') },
    { enabled: true, time: '06:15', days: D('1111111') },
    { enabled: true, time: null,    days: D('1111111') },
  ]);
  assert.strictEqual(packed, '06:15|1111111');
});

test('a one-time alarm has an all-zero weekday field', () => {
  assert.strictEqual(
    packAlarmSet([{ enabled: true, time: '05:15', days: D('0000000') }]),
    '05:15|0000000');
});

test('normalises a short or missing days array to all-zero', () => {
  assert.strictEqual(
    packAlarmSet([{ enabled: true, time: '09:00', days: [true, false] }]),
    '09:00|1000000');
  assert.strictEqual(
    packAlarmSet([{ enabled: true, time: '09:00', days: undefined }]),
    '09:00|0000000');
});

test('zero-pads single-digit hours and minutes', () => {
  assert.strictEqual(
    packAlarmSet([{ enabled: true, time: '7:5', days: D('1000000') }]),
    '07:05|1000000');
});

test('rejects an out-of-range time rather than emitting garbage', () => {
  assert.strictEqual(packAlarmSet([{ enabled: true, time: '25:00', days: D('1') }]), '');
  assert.strictEqual(packAlarmSet([{ enabled: true, time: '12:99', days: D('1') }]), '');
  assert.strictEqual(packAlarmSet([{ enabled: true, time: 'nope', days: D('1') }]), '');
});

test('truncates to SLOT_COUNT slots', () => {
  const many = [];
  for (let i = 0; i < 12; i++) {
    many.push({ enabled: true, time: '0' + (i % 10) + ':00', days: D('1111111') });
  }
  assert.strictEqual(packAlarmSet(many).split(';').length, SLOT_COUNT);
});

test('an empty slot list packs to the empty string (the watch clears all alarms)', () => {
  assert.strictEqual(packAlarmSet([]), '');
});

test('round-trips all SLOT_COUNT (8) slots by content, not just the truncated count', () => {
  // Exactly 8 slots, alternating enabled/disabled and weekday/one-time -- this
  // exercises every field combination at the real SLOT_COUNT boundary, not
  // just the count assertion 'truncates to SLOT_COUNT slots' above makes.
  // The packed literal below is also asserted against the real ac_parse_set
  // in tests/test_pack_contract.c (search that file for this exact string).
  const slots = [];
  for (let i = 0; i < SLOT_COUNT; i++) {
    slots.push({
      enabled: i % 2 === 0,
      time: '0' + i + ':00',
      days: i % 2 === 0 ? D('1111100') : D('0000000'),
    });
  }
  const packed = packAlarmSet(slots);
  assert.strictEqual(packed,
    '00:00|1111100;-01:00|0000000;02:00|1111100;-03:00|0000000;'
    + '04:00|1111100;-05:00|0000000;06:00|1111100;-07:00|0000000');
  const round = unpackAlarmSet(packed);
  assert.strictEqual(round.length, SLOT_COUNT);
  assert.deepStrictEqual(round, slots);
  assert.strictEqual(packAlarmSet(round), packed);
});

test('round-trips through unpackAlarmSet', () => {
  const packed = '07:00|1111100;-08:30|0000011;05:15|0000000';
  const slots = unpackAlarmSet(packed);
  assert.strictEqual(slots.length, 3);
  assert.deepStrictEqual(slots[0], { enabled: true,  time: '07:00', days: D('1111100') });
  assert.deepStrictEqual(slots[1], { enabled: false, time: '08:30', days: D('0000011') });
  assert.deepStrictEqual(slots[2], { enabled: true,  time: '05:15', days: D('0000000') });
  assert.strictEqual(packAlarmSet(slots), packed);
});

test('unpack tolerates empty and malformed input', () => {
  assert.deepStrictEqual(unpackAlarmSet(''), []);
  assert.deepStrictEqual(unpackAlarmSet(undefined), []);
  assert.deepStrictEqual(unpackAlarmSet(';;'), []);
  assert.deepStrictEqual(unpackAlarmSet('garbage'), []);
  // a good slot next to a bad one survives
  assert.strictEqual(unpackAlarmSet('bad;07:00|1111100').length, 1);
});
