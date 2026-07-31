// SPDX-License-Identifier: GPL-3.0-only

// One Clay alarm slot as the config page holds it. `time` is the raw value of an
// <input type="time">, i.e. "HH:MM" (or "" when the user has not set one).
export interface SlotFields {
  enabled: boolean;
  time: string;
  days: boolean[];   // length 7, Monday first
}

export const SLOT_COUNT = 8;

function pad2(n: number): string {
  return n < 10 ? '0' + n : '' + n;
}

// Parse "HH:MM" strictly. Returns null when it is not a valid time of day, so a
// malformed slot is dropped rather than sent to the watch as garbage.
function parseHhMm(time: string | null | undefined): { h: number; m: number } | null {
  if (typeof time !== 'string' || time.length === 0) {
    return null;
  }
  const parts = time.split(':');
  if (parts.length !== 2) {
    return null;
  }
  if (!/^\d{1,2}$/.test(parts[0]) || !/^\d{1,2}$/.test(parts[1])) {
    return null;
  }
  const h = parseInt(parts[0], 10);
  const m = parseInt(parts[1], 10);
  if (h < 0 || h > 23 || m < 0 || m > 59) {
    return null;
  }
  return { h: h, m: m };
}

function daysToDigits(days: boolean[] | null | undefined): string {
  let out = '';
  for (let i = 0; i < 7; i++) {
    out += days && days[i] ? '1' : '0';
  }
  return out;
}

// Pack the slots into the AlarmSet wire format:
//     "07:00|1111100;-08:30|0000011"
// A leading '-' means the slot is disabled but its time is remembered. Slots with
// no time, or an unparseable one, are omitted. At most SLOT_COUNT slots are sent.
export function packAlarmSet(slots: SlotFields[]): string {
  const out: string[] = [];
  for (let i = 0; i < slots.length && out.length < SLOT_COUNT; i++) {
    const s = slots[i];
    if (!s) {
      continue;
    }
    const hm = parseHhMm(s.time);
    if (hm === null) {
      continue;
    }
    out.push((s.enabled ? '' : '-') + pad2(hm.h) + ':' + pad2(hm.m)
             + '|' + daysToDigits(s.days));
  }
  return out.join(';');
}

// Inverse of packAlarmSet, used to seed the Clay page from what was last sent.
export function unpackAlarmSet(packed: string | null | undefined): SlotFields[] {
  const out: SlotFields[] = [];
  if (typeof packed !== 'string' || packed.length === 0) {
    return out;
  }
  const chunks = packed.split(';');
  for (let i = 0; i < chunks.length; i++) {
    let c = chunks[i];
    if (c.length === 0) {
      continue;
    }
    let enabled = true;
    if (c.charAt(0) === '-') {
      enabled = false;
      c = c.substring(1);
    }
    const bar = c.indexOf('|');
    if (bar < 0) {
      continue;
    }
    const hm = parseHhMm(c.substring(0, bar));
    const digits = c.substring(bar + 1);
    if (hm === null || digits.length !== 7 || !/^[01]{7}$/.test(digits)) {
      continue;
    }
    const days: boolean[] = [];
    for (let d = 0; d < 7; d++) {
      days.push(digits.charAt(d) === '1');
    }
    out.push({ enabled: enabled, time: pad2(hm.h) + ':' + pad2(hm.m), days: days });
  }
  return out;
}
