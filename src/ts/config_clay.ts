// SPDX-License-Identifier: GPL-3.0-only
import { SLOT_COUNT } from './alarm_pack';

const DAY_OPTIONS = [
  { label: 'Mon', value: 'mon' }, { label: 'Tue', value: 'tue' },
  { label: 'Wed', value: 'wed' }, { label: 'Thu', value: 'thu' },
  { label: 'Fri', value: 'fri' }, { label: 'Sat', value: 'sat' },
  { label: 'Sun', value: 'sun' },
];

function alarmSlotItems(i: number): any[] {
  const n = i + 1;
  return [
    {
      type: 'toggle', messageKey: 'Slot' + n + 'On',
      label: 'Alarm ' + n + ' enabled', defaultValue: i === 0,
      id: 'slot' + n + '-on',
    },
    {
      type: 'input', messageKey: 'Slot' + n + 'Time',
      label: 'Time', attributes: { type: 'time' },
      defaultValue: i === 0 ? '07:00' : '',
      id: 'slot' + n + '-time',
    },
    {
      type: 'checkboxgroup', messageKey: 'Slot' + n + 'Days',
      label: 'Repeat', options: DAY_OPTIONS,
      defaultValue: i === 0 ? [true, true, true, true, true, false, false]
                            : [false, false, false, false, false, false, false],
      description: 'No days selected = a one-time alarm.',
      id: 'slot' + n + '-days',
    },
  ];
}

export function buildConfig(): any[] {
  const items: any[] = [
    { type: 'heading', defaultValue: 'Sykerö Smart Alarm' },
    {
      type: 'text',
      defaultValue: 'Alarms are configured here, not on the watch. '
                  + 'On the watch you can switch an alarm off or skip its next '
                  + 'occurrence.',
    },
  ];

  const alarmItems: any[] = [{ type: 'heading', defaultValue: 'Alarms' }];
  for (let i = 0; i < SLOT_COUNT; i++) {
    const slot = alarmSlotItems(i);
    for (let j = 0; j < slot.length; j++) {
      alarmItems.push(slot[j]);
    }
  }
  items.push({ type: 'section', items: alarmItems });
  items.push({ type: 'submit', defaultValue: 'Save' });
  return items;
}
