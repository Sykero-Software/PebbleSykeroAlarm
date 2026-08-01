// SPDX-License-Identifier: GPL-3.0-only
import { packAlarmSet, unpackAlarmSet, SLOT_COUNT, SlotFields } from './alarm_pack';
import { buildConfig } from './config_clay';
import clayConfigCustom from './config_clay_custom';
import alarmListComponent from './config_alarm_list';
import { CFG_STORE_KEY, resendDict, saveDict } from './config_sync';

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
    clayInstance.registerComponent(alarmListComponent);
  }
  return clayInstance;
}

// One-time seed of the alarmList component's key from whatever held the alarms
// before it existed. Without this the first open of the new page would show
// `defaultValue` (a single 07:00 Mon-Fri) and saving would WIPE every other alarm
// the user had -- a consolidated Clay key never inherits the keys it replaces.
//
// Preferred source is the dict config_sync last sent, because its AlarmSet is by
// definition what the watch currently holds. The legacy per-slot Clay keys are the
// fallback for a phone that has the config page's state but never completed a send.
export function migrateAlarmList(get: (k: string) => string | null,
                                 set: (k: string, v: string) => void): void {
  let stored: any;
  try {
    stored = JSON.parse(get('clay-settings') || '{}') || {};
  } catch (e) {
    return;
  }
  if (typeof stored !== 'object' || stored === null) { return; }
  if (stored.AlarmList !== undefined) { return; }   // already migrated

  let seed: string | null = null;
  try {
    const raw = get(CFG_STORE_KEY);
    if (raw) {
      const d = JSON.parse(raw);
      if (d && typeof d.AlarmSet === 'string' && d.AlarmSet !== '') { seed = d.AlarmSet; }
    }
  } catch (e) {
    seed = null;
  }
  if (seed === null) {
    // slotsFromSettings works on the flattened clay-settings shape too: val()
    // returns a raw value unchanged and only unwraps {value:X} when present.
    const packed = packAlarmSet(slotsFromSettings(stored));
    if (packed !== '') { seed = packed; }
  }
  if (seed === null) { return; }   // nothing to inherit: defaultValue is correct

  stored.AlarmList = seed;
  set('clay-settings', JSON.stringify(stored));
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
];

export const BOOL_KEYS = ['SmartEnabled', 'EscRampVib'];

export function buildDict(settings: any): any {
  const dict: any = {};
  // The alarmList component's value IS the wire string, so this is a pass-through
  // -- but it goes through unpack+pack rather than being trusted verbatim: that
  // canonicalizes it and drops anything malformed, using the same host-tested
  // packer the watch's parser is contract-tested against (test_pack_contract.c).
  // A component reading raw DOM must not be able to put garbage on the wire.
  const list = val(settings, 'AlarmList');
  if (typeof list === 'string') {
    dict.AlarmSet = packAlarmSet(unpackAlarmSet(list));
  } else {
    // Legacy per-slot settings (a config page from before the list component).
    dict.AlarmSet = packAlarmSet(slotsFromSettings(settings));
  }
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

if (typeof Pebble !== 'undefined') {
  Pebble.addEventListener('showConfiguration', function () {
    // Before generateUrl(), which bakes clay-settings into the page: the seed has
    // to be in the store by the time the page is built, or the first open of the
    // list would show defaultValue and saving would wipe the user's other alarms.
    migrateAlarmList(
      function (k) { return window.localStorage.getItem(k); },
      function (k, v) { window.localStorage.setItem(k, v); });
    Pebble.openURL(getClay().generateUrl());
  });

  Pebble.addEventListener('webviewclosed', function (e: any) {
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

  // Send the last-saved config to the watch. Called from BOTH directions of the
  // launch handshake below, because either one alone can lose the race:
  //
  //  - The watch asks (main.c's request_config, key CfgRequest) as the last thing
  //    it does in main(). But PKJS is started BY the watchapp launching and dies
  //    with it, so it is cold-starting at exactly that moment -- verified on the
  //    diorite emulator, where two CfgRequests were sent by the watch and NEITHER
  //    reached this listener, because it was not registered yet.
  //  - So the phone also pushes, unprompted, as soon as it is ready (the same
  //    thing PebbleTuyaControl does in its `ready` handler). By then the watch
  //    has long since called app_message_open.
  //
  // Sending twice is harmless: the watch's inbox handler is idempotent (it parses
  // the same AlarmSet and saves the same config), so whichever of the two lands
  // wins and the other is a no-op.
  // A `var`-assigned function expression, not a function declaration: this block
  // is inside the `typeof Pebble !== 'undefined'` guard, and TS/ES5 strict mode
  // forbids a function DECLARATION inside a block (TS1252).
  var sendSavedConfig = function (why: string): void {
    const dict = resendDict(function (k) { return window.localStorage.getItem(k); });
    if (!dict) {
      // Nothing was ever saved on this phone: stay silent rather than send an
      // empty dict, which would clobber the watch's own persisted state.
      console.log('SmartAlarm: ' + why + ' but nothing saved yet -- staying silent');
      return;
    }
    console.log('SmartAlarm: ' + why + ' -> sending saved AlarmSet=' + dict.AlarmSet);
    Pebble.sendAppMessage(dict,
      function () { console.log('SmartAlarm: saved config delivered'); },
      function (err: any) { console.log('SmartAlarm: saved config send failed ' + JSON.stringify(err)); });
  };

  Pebble.addEventListener('appmessage', function (e: any) {
    const p = e && e.payload;
    if (!p || p.CfgRequest === undefined || p.CfgRequest === null) {
      return;
    }
    sendSavedConfig('CfgRequest');
  });

  Pebble.addEventListener('ready', function () {
    console.log('SmartAlarm PKJS ready');
    sendSavedConfig('PKJS ready');
  });
}
