// SPDX-License-Identifier: GPL-3.0-only
import { packAlarmSet, unpackAlarmSet, SLOT_COUNT, SlotFields } from './alarm_pack';
import { buildConfig } from './config_clay';
import clayConfigCustom from './config_clay_custom';

declare const require: any;
const Clay = require('pebble-clay');
const clay = new Clay(buildConfig(), clayConfigCustom, { autoHandleEvents: false });

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
    out.push({
      enabled: val(settings, 'Slot' + i + 'On') === true,
      time: (val(settings, 'Slot' + i + 'Time') || '') as string,
      days: Array.isArray(days) ? (days as boolean[]) : [],
    });
  }
  return out;
}

Pebble.addEventListener('showConfiguration', function () {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e: any) {
  if (!e || !e.response) {
    return;
  }
  const settings = clay.getSettings(e.response, false);
  const dict: any = {};
  dict.AlarmSet = packAlarmSet(slotsFromSettings(settings));
  console.log('SmartAlarm: sending AlarmSet=' + dict.AlarmSet);
  Pebble.sendAppMessage(dict,
    function () { console.log('SmartAlarm: settings delivered'); },
    function (err: any) { console.log('SmartAlarm: send failed ' + JSON.stringify(err)); });
});

Pebble.addEventListener('ready', function () {
  console.log('SmartAlarm PKJS ready');
});
