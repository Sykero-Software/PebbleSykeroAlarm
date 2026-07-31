// SPDX-License-Identifier: GPL-3.0-only
import { SLOT_COUNT } from './alarm_pack';

// PLAIN STRINGS, not {label, value} objects. Clay's checkboxgroup template renders
// each option with `{{{this}}}` (node_modules/pebble-clay/src/templates/components/
// checkboxgroup.tpl) and its README says "options | array of strings", so an object
// renders as the literal text "[object Object]" on the phone. The value half was dead
// anyway: a checkboxgroup returns a boolean[] read POSITIONALLY, so the labels are the
// only thing Clay needs. Reported from a real phone 2026-07-31 — the config page cannot
// render headless, so tests/config_clay.test.js now asserts this instead.
const DAY_OPTIONS = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];

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

  items.push({ type: 'section', items: [
    { type: 'heading', defaultValue: 'Smart alarm' },
    { type: 'text', defaultValue:
        'Wakes you a little early, at a moment you are already stirring. '
      + 'Needs activity tracking switched on in the watch settings. '
      + 'Not available on Pebble Classic / Steel.' },
    { type: 'toggle', messageKey: 'SmartEnabled', label: 'Smart alarm',
      defaultValue: true, id: 'smart-on' },
    { type: 'select', messageKey: 'SmartWindowMin', label: 'Look back at most',
      defaultValue: '30', id: 'smart-window',
      options: [
        { label: '10 min', value: '10' }, { label: '15 min', value: '15' },
        { label: '20 min', value: '20' }, { label: '30 min', value: '30' },
        { label: '45 min', value: '45' }, { label: '60 min', value: '60' },
      ] },
    { type: 'select', messageKey: 'TimeSemantics', label: 'The alarm time means',
      defaultValue: '0', id: 'smart-semantics',
      options: [
        { label: 'Ringing starts then', value: '0' },
        { label: 'Awake by then', value: '1' },
      ],
      description: '"Awake by then" starts the ramp early enough to be at full '
                 + 'strength by the set time.' },
    { type: 'select', messageKey: 'Sensitivity', label: 'Sensitivity',
      defaultValue: '1', id: 'smart-sens',
      options: [
        { label: 'Low — only a clear stir', value: '0' },
        { label: 'Medium', value: '1' },
        { label: 'High — the slightest stir', value: '2' },
        { label: 'Custom', value: '3' },
      ] },
    { type: 'slider', messageKey: 'SensPercentile', label: 'Custom: stir percentile',
      defaultValue: 90, min: 70, max: 99, step: 1, id: 'sens-pct',
      description: 'Wake me on a stir among tonight’s most active '
                 + '(100 minus this) percent. Higher = fussier.' },
    { type: 'select', messageKey: 'SensMinutes', label: 'Custom: sustained for',
      defaultValue: '2', id: 'sens-min',
      options: [
        { label: '1 min', value: '1' }, { label: '2 min', value: '2' },
        { label: '3 min', value: '3' }, { label: '4 min', value: '4' },
        { label: '5 min', value: '5' },
      ],
      description: 'Raise this if a brief turn-over wakes you.' },
  ] });

  items.push({ type: 'section', items: [
    { type: 'heading', defaultValue: 'How it wakes you' },
    { type: 'text', defaultValue:
        'Vibration starts gently with long gaps, so there is time to shift '
      + 'position if you cannot feel it, then tightens. Sound joins later and '
      + 'gets louder.' },
    { type: 'select', messageKey: 'WakeProfile', label: 'Wake style',
      defaultValue: '1', id: 'wake-profile',
      options: [
        { label: 'Gentle', value: '0' },
        { label: 'Normal', value: '1' },
        { label: 'Insistent', value: '2' },
        { label: 'Custom', value: '3' },
      ] },
    { type: 'slider', messageKey: 'EscLeadGapS', label: 'Custom: first gap (s)',
      defaultValue: 30, min: 2, max: 120, step: 1, id: 'esc-lead' },
    { type: 'slider', messageKey: 'EscMinGapS', label: 'Custom: final gap (s)',
      defaultValue: 5, min: 1, max: 60, step: 1, id: 'esc-min' },
    { type: 'slider', messageKey: 'EscTightenS', label: 'Custom: tighten over (s)',
      defaultValue: 360, min: 30, max: 1800, step: 10, id: 'esc-tighten',
      description: 'Also bounded by "give up after" below -- reduced to fit if '
                 + 'that is short.' },
    { type: 'slider', messageKey: 'EscVibStartMs', label: 'Custom: first pulse (ms)',
      defaultValue: 80, min: 40, max: 2000, step: 10, id: 'esc-vibstart' },
    { type: 'slider', messageKey: 'EscVibMaxMs', label: 'Custom: longest pulse (ms)',
      defaultValue: 700, min: 40, max: 2000, step: 10, id: 'esc-vibmax' },
    { type: 'slider', messageKey: 'EscPulsesStart', label: 'Custom: first burst pulses',
      defaultValue: 1, min: 1, max: 8, step: 1, id: 'esc-pstart' },
    { type: 'slider', messageKey: 'EscPulsesMax', label: 'Custom: final burst pulses',
      defaultValue: 3, min: 1, max: 8, step: 1, id: 'esc-pmax' },
    { type: 'slider', messageKey: 'EscSoundAfterS', label: 'Custom: sound joins after (s)',
      defaultValue: 300, min: 1, max: 1800, step: 10, id: 'esc-sndafter',
      description: 'Also bounded by "give up after" below -- reduced to fit if '
                 + 'that is short.' },
    { type: 'slider', messageKey: 'EscSoundRampS', label: 'Custom: volume ramp (s)',
      defaultValue: 300, min: 10, max: 1800, step: 10, id: 'esc-sndramp',
      description: 'Also bounded by "give up after" below -- reduced to fit if '
                 + 'that is short.' },
    { type: 'slider', messageKey: 'EscVolStart', label: 'Custom: first volume',
      defaultValue: 15, min: 1, max: 100, step: 1, id: 'esc-volstart' },
    { type: 'slider', messageKey: 'EscVolMax', label: 'Custom: max volume',
      defaultValue: 100, min: 1, max: 100, step: 1, id: 'esc-volmax' },
    { type: 'slider', messageKey: 'EscCapS', label: 'Custom: give up after (s)',
      defaultValue: 900, min: 120, max: 3600, step: 30, id: 'esc-cap' },
    { type: 'toggle', messageKey: 'LightPulse', label: 'Light up with each buzz',
      defaultValue: true, id: 'light-pulse',
      description: 'Only during a buzz, never between them.' },
  ] });

  items.push({ type: 'section', items: [
    { type: 'heading', defaultValue: 'Snooze and stopping' },
    { type: 'select', messageKey: 'SnoozeMin', label: 'Snooze length',
      defaultValue: '10', id: 'snooze-min',
      options: [
        { label: 'Off', value: '0' }, { label: '5 min', value: '5' },
        { label: '9 min', value: '9' }, { label: '10 min', value: '10' },
        { label: '15 min', value: '15' }, { label: '20 min', value: '20' },
      ] },
    { type: 'select', messageKey: 'SnoozeMax', label: 'Snoozes allowed',
      defaultValue: '5', id: 'snooze-max',
      options: [
        { label: 'Unlimited', value: '0' }, { label: '1', value: '1' },
        { label: '2', value: '2' }, { label: '3', value: '3' },
        { label: '5', value: '5' }, { label: '10', value: '10' },
      ] },
    { type: 'slider', messageKey: 'SnoozeRampOffsetS',
      label: 'Each snooze starts this far along (s)',
      defaultValue: 120, min: 0, max: 600, step: 10, id: 'snooze-ramp',
      description: '0 restarts the ramp from the very beginning each time.' },
    { type: 'select', messageKey: 'StopGesture', label: 'To stop the alarm',
      defaultValue: '1', id: 'stop-gesture',
      options: [
        { label: 'Hold the bottom button', value: '0' },
        { label: 'Press the bottom button twice', value: '1' },
        { label: 'Press the bottom button three times', value: '2' },
      ],
      description: 'A single press only snoozes, so a half-asleep tap cannot '
                 + 'end the alarm.' },
  ] });

  items.push({ type: 'section', items: [
    { type: 'heading', defaultValue: 'Other' },
    { type: 'select', messageKey: 'IdleExitSec',
      label: 'Return to the watchface when idle', defaultValue: '15',
      id: 'idle-exit',
      options: [
        { label: 'Off', value: '0' }, { label: '10 s', value: '10' },
        { label: '15 s', value: '15' }, { label: '30 s', value: '30' },
        { label: '60 s', value: '60' },
      ] },
    { type: 'toggle', messageKey: 'DstCheck', label: 'Daily clock-change check',
      defaultValue: true, id: 'dst-check',
      description: 'Re-arms alarms after a daylight-saving change. Leave on.' },
  ] });

  items.push({ type: 'submit', defaultValue: 'Save' });
  return items;
}
