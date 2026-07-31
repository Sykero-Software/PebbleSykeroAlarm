// SPDX-License-Identifier: GPL-3.0-only
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
