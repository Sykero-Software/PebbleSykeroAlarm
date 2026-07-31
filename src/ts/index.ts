// SPDX-License-Identifier: GPL-3.0-only
import { packAlarmSet, unpackAlarmSet, SLOT_COUNT, SlotFields } from './alarm_pack';
import { buildConfig } from './config_clay';
import clayConfigCustom from './config_clay_custom';
import { resendDict, saveDict } from './config_sync';

declare const require: any;

// Clay itself is only requireable inside the pebble-tool build (its config
// webview HTML + the generated `message_keys` module do not exist under a
// plain `node --test`), and `Pebble` is a runtime global the phone provides,
// not one Node has. Both are deferred behind getClay()/the typeof guard below
// so this file's pure exports (buildDict, NUMERIC_KEYS, BOOL_KEYS) can be
// `require`d directly by tests/config_clay.test.js without ever touching
// either — on the real watch `Pebble` is always defined by the time this
// bundle runs, so the guard is a no-op there and behaviour is unchanged.
let clayInstance: any = null;
function getClay(): any {
  if (!clayInstance) {
    const Clay = require('pebble-clay');
    clayInstance = new Clay(buildConfig(), clayConfigCustom, { autoHandleEvents: false });
  }
  return clayInstance;
}

// Clay returns getSettings(resp, false) in its unflattened {key:{value:X}} form.
function val(settings: any, key: string): any {
  const e = settings[key];
  if (e === undefined || e === null) {
    return undefined;
  }
  return e && typeof e === 'object' && 'value' in e ? e.value : e;
}

function slotsFromSettings(settings: any): SlotFields[] {
  const out: SlotFields[] = [];
  for (let i = 1; i <= SLOT_COUNT; i++) {
    const days = val(settings, 'Slot' + i + 'Days');
    const time = val(settings, 'Slot' + i + 'Time');
    out.push({
      enabled: val(settings, 'Slot' + i + 'On') === true,
      // Guarded the same way as `days` below: an unexpected non-string value
      // (Clay is expected to hand back a DOM input value, i.e. always a
      // string, but nothing enforces that at this boundary) falls back to ''
      // rather than being blindly cast.
      time: typeof time === 'string' ? time : '',
      days: Array.isArray(days) ? (days as boolean[]) : [],
    });
  }
  return out;
}

// Clay returns select/slider/radiogroup/input values as DOM STRINGS. The watch
// reads ints (value->int32), so every numeric key must be parseInt'ed before
// sendAppMessage or the watch reads garbage.
export const NUMERIC_KEYS = [
  'SmartWindowMin', 'TimeSemantics', 'Sensitivity', 'SensPercentile', 'SensMinutes',
  'WakeProfile',
  'EscLeadGapS', 'EscMinGapS', 'EscTightenS', 'EscVibStartMs', 'EscVibMaxMs',
  'EscPulsesStart', 'EscPulsesMax', 'EscSoundAfterS', 'EscSoundRampS',
  'EscVolStart', 'EscVolMax', 'EscCapS',
  'SnoozeMin', 'SnoozeMax', 'SnoozeRampOffsetS',
  'StopGesture', 'IdleExitSec',
];

export const BOOL_KEYS = ['SmartEnabled', 'LightPulse', 'DstCheck'];

export function buildDict(settings: any): any {
  const dict: any = {};
  dict.AlarmSet = packAlarmSet(slotsFromSettings(settings));
  for (let i = 0; i < NUMERIC_KEYS.length; i++) {
    const k = NUMERIC_KEYS[i];
    const v = val(settings, k);
    if (v === undefined || v === null || v === '') {
      continue;
    }
    const n = parseInt(String(v), 10);
    if (!isNaN(n)) {
      dict[k] = n;
    }
  }
  for (let i = 0; i < BOOL_KEYS.length; i++) {
    const k = BOOL_KEYS[i];
    const v = val(settings, k);
    if (v === undefined || v === null) {
      continue;
    }
    dict[k] = v === true || v === 'true' || v === 1 ? 1 : 0;
  }
  return dict;
}

// Pauses/resumes the watch's idle auto-exit around the config page (Task 13):
// without this, the idle timer could fire while the config webview is open and
// pop the app's window stack out from under the user, closing the config page
// on them.
function sendCfgOpen(open: boolean): void {
  Pebble.sendAppMessage({ CfgOpen: open ? 1 : 0 },
    function () {}, function () {});
}

if (typeof Pebble !== 'undefined') {
  Pebble.addEventListener('showConfiguration', function () {
    sendCfgOpen(true);
    Pebble.openURL(getClay().generateUrl());
  });

  Pebble.addEventListener('webviewclosed', function (e: any) {
    sendCfgOpen(false);
    if (!e || !e.response) {
      return;
    }
    const settings = getClay().getSettings(e.response, false);
    const dict = buildDict(settings);
    // Persist FIRST, then send: the send is the unreliable half (the watchapp is
    // very likely not running), so the saved copy is what actually delivers this
    // config -- on the watch's next launch, via the CfgRequest reply below.
    // Saving only in the success callback would lose exactly the case this
    // exists for.
    saveDict(function (k, v) { window.localStorage.setItem(k, v); }, dict);
    console.log('SmartAlarm: sending AlarmSet=' + dict.AlarmSet);
    Pebble.sendAppMessage(dict,
      function () { console.log('SmartAlarm: settings delivered'); },
      function (err: any) { console.log('SmartAlarm: send failed ' + JSON.stringify(err)); });
  });

  // The watch asks for its config on every launch (main.c's request_config) --
  // see config_sync.ts for why that handshake exists at all.
  Pebble.addEventListener('appmessage', function (e: any) {
    const p = e && e.payload;
    if (!p || p.CfgRequest === undefined || p.CfgRequest === null) {
      return;
    }
    const dict = resendDict(function (k) { return window.localStorage.getItem(k); });
    if (!dict) {
      // Nothing was ever saved on this phone: stay silent rather than send an
      // empty dict, which would clobber the watch's own persisted state.
      console.log('SmartAlarm: CfgRequest but nothing saved yet -- staying silent');
      return;
    }
    console.log('SmartAlarm: CfgRequest -> resending AlarmSet=' + dict.AlarmSet);
    Pebble.sendAppMessage(dict,
      function () { console.log('SmartAlarm: config resent'); },
      function (err: any) { console.log('SmartAlarm: resend failed ' + JSON.stringify(err)); });
  });

  Pebble.addEventListener('ready', function () {
    console.log('SmartAlarm PKJS ready');
  });
}
