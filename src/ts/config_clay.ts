// SPDX-License-Identifier: GPL-3.0-only

// The AlarmSet wire string a fresh phone proposes: 07:00 Mon-Fri.
export const ALARM_LIST_DEFAULT = '07:00|1111100';

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

  // One custom component, not eight fixed slot blocks: rows are added and deleted
  // explicitly, so all eight alarms are reachable instead of the next empty one
  // being revealed only once the previous had a time. `AlarmList` is a CLAY-ONLY
  // key -- it is not in package.json messageKeys and never reaches the watch; its
  // value is the AlarmSet wire string, which index.ts forwards under that name.
  items.push({ type: 'section', items: [
    { type: 'heading', defaultValue: 'Alarms' },
    { type: 'alarmList', messageKey: 'AlarmList', id: 'alarm-list',
      defaultValue: ALARM_LIST_DEFAULT },
    { type: 'text', defaultValue:
        'One-time picks the next matching day and switches itself off after '
      + 'ringing. On the watch you can switch an alarm off or skip its next '
      + 'occurrence; times are set here.' },
  ] });

  items.push({ type: 'section', items: [
    { type: 'heading', defaultValue: 'Smart alarm' },
    { type: 'text', defaultValue:
        'Wakes you a little early, at a moment you are already stirring. '
      + 'Needs activity tracking switched on in the watch settings. '
      + 'Not available on Pebble Classic / Steel.' },
    { type: 'toggle', messageKey: 'SmartEnabled', label: 'Smart alarm',
      defaultValue: true, id: 'smart-on' },
    { type: 'select', messageKey: 'SmartWindowMin', label: 'Smart window length',
      defaultValue: '30', id: 'smart-window',
      options: [
        { label: '10 min', value: '10' }, { label: '15 min', value: '15' },
        { label: '20 min', value: '20' }, { label: '30 min', value: '30' },
        { label: '45 min', value: '45' }, { label: '60 min', value: '60' },
      ] },
    { type: 'select', messageKey: 'PreAlarmMin', label: 'Show alarm screen before',
      defaultValue: '60', id: 'pre-alarm-min',
      options: [
        { label: 'Off', value: '0' }, { label: '15 min', value: '15' },
        { label: '30 min', value: '30' }, { label: '60 min', value: '60' },
        { label: '90 min', value: '90' },
      ],
      description: 'Opens the alarm\'s waiting screen this long before the '
                 + 'alarm, so you can cancel it with two presses of the bottom '
                 + 'button if you wake up early. Independent of the smart '
                 + 'alarm: it works with the smart alarm off too, and it never '
                 + 'makes the alarm ring any earlier.' },
    // Three modes, spelled out with both ends of each window, because the old
    // two-option wording ("Ringing starts then") was read as "not before then"
    // when it meant the opposite. Say which direction the window moves the ring.
    { type: 'select', messageKey: 'TimeSemantics', label: 'The alarm time is',
      defaultValue: '0', id: 'smart-semantics',
      options: [
        { label: 'The latest — may ring earlier', value: '0' },
        { label: 'The earliest — may ring later', value: '2' },
        { label: 'When I must be fully awake', value: '1' },
      ],
      description: 'With a 30 min window and a 07:50 alarm: '
                 + '"the latest" rings between 07:20 and 07:50; '
                 + '"the earliest" never rings before 07:50 and rings by 08:20; '
                 + '"fully awake" starts the ramp early enough to be at full '
                 + 'strength at 07:50. Switching the smart alarm off makes all '
                 + 'three ring at exactly 07:50.' },
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
        'Vibration buzzes at full strength from the very first buzz, with a '
      + 'steady gap between buzzes. Sound joins later and gets louder.' },
    { type: 'toggle', messageKey: 'EscRampVib',
      label: 'Ramp the vibration up', defaultValue: false, id: 'esc-ramp',
      description: 'Off is recommended. When on, the vibration starts as a faint '
                 + 'tap and grows — which can teach you to sleep through it, and '
                 + 'through the stronger buzzes that follow.' },
    { type: 'select', messageKey: 'WakeProfile', label: 'Wake style',
      defaultValue: '1', id: 'wake-profile',
      options: [
        { label: 'Gentle', value: '0' },
        { label: 'Normal', value: '1' },
        { label: 'Insistent', value: '2' },
        { label: 'Custom', value: '3' },
      ] },
    // Labels are worded for the ramp-OFF default, where these three ARE the buzz:
    // lead_gap is the only gap, vib_max the only pulse length, pulses_max the only
    // count. With the ramp on they become the starting/final ends of it, which the
    // descriptions say -- the four ramp-only sliders below are hidden when it is off.
    { type: 'slider', messageKey: 'EscLeadGapS', label: 'Custom: gap between buzzes (s)',
      defaultValue: 30, min: 2, max: 120, step: 1, id: 'esc-lead',
      description: 'With the ramp on, this is the first gap only.' },
    { type: 'slider', messageKey: 'EscMinGapS', label: 'Custom: final gap (s)',
      defaultValue: 5, min: 1, max: 60, step: 1, id: 'esc-min' },
    { type: 'slider', messageKey: 'EscTightenS', label: 'Custom: tighten over (s)',
      defaultValue: 360, min: 30, max: 1800, step: 10, id: 'esc-tighten',
      description: 'Also bounded by "give up after" below -- reduced to fit if '
                 + 'that is short.' },
    { type: 'slider', messageKey: 'EscVibStartMs', label: 'Custom: first pulse (ms)',
      defaultValue: 80, min: 40, max: 2000, step: 10, id: 'esc-vibstart' },
    { type: 'slider', messageKey: 'EscVibMaxMs', label: 'Custom: pulse length (ms)',
      defaultValue: 700, min: 40, max: 2000, step: 10, id: 'esc-vibmax',
      description: 'With the ramp on, this is the longest pulse only.' },
    { type: 'slider', messageKey: 'EscPulsesStart', label: 'Custom: first burst pulses',
      defaultValue: 1, min: 1, max: 8, step: 1, id: 'esc-pstart' },
    { type: 'slider', messageKey: 'EscPulsesMax', label: 'Custom: pulses per buzz',
      defaultValue: 3, min: 1, max: 8, step: 1, id: 'esc-pmax',
      description: 'With the ramp on, this is the final count only.' },
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
      defaultValue: 0, min: 0, max: 600, step: 10, id: 'snooze-ramp',
      description: 'Default 0: every snooze restarts the ramp from the '
                 + 'beginning. Raise it to make each snooze pick up further '
                 + 'along, so a second alarm starts stronger than the first.' },
    { type: 'text', defaultValue:
        'Press the bottom button TWICE to stop the alarm. A single press only '
      + 'snoozes, so a half-asleep tap cannot end it by accident.' },
  ] });

  // Runtime gate for the watch's Diagnostics row. Default OFF: a released app
  // must not show a debug row unasked. "Debugging" names the area, "Diagnostics"
  // names what the row produces -- both words come from the Pebble app's own
  // vocabulary. Deliberately NOT called "Debug mode": nothing about the app
  // behaves differently, one menu item appears.
  items.push({ type: 'section', items: [
    { type: 'heading', defaultValue: 'Debugging' },
    { type: 'toggle', messageKey: 'DebugFeatures',
      label: 'Show the Diagnostics menu item',
      defaultValue: false, id: 'debug-features',
      description: 'Adds a Diagnostics item to the app\'s menu on the watch. '
                 + 'It writes a report of the last night to the Pebble log, so '
                 + 'you can send it with a bug report. Takes a few seconds.' },
  ] });

  items.push({ type: 'submit', defaultValue: 'Save' });
  return items;
}
