# Sykerö Alarm

Pebble watchapp — **Sykerö Alarm** ("Smart Alarm" on the watch launcher, "Sykerö
Smart Alarm" in the appstore).

An alarm clock watchapp with two features that set it apart from the built-in
Pebble alarms: a **smart alarm** that wakes you at the best moment inside a
configurable window before your set time (using the watch's activity/sleep
tracking to pick a lighter-sleep moment), and a **gradual escalation** wake
sequence (vibration first, tightening and strengthening over time, with sound
joining later on watches that have a speaker) instead of one abrupt buzz.

All configuration — alarm time, smart-alarm window and sensitivity, wake
profile, escalation timing, snooze behaviour, stop gesture, idle exit — is done
**phone-side via a Clay config page**, not on the watch.

The smart alarm requires the watch's activity tracking to be enabled and is
**not available on aplite** (no accelerometer-based sleep data on that
platform generation). The escalation sequence's **sound stage needs emery or
flint** (the colour/speaker-equipped boards) — earlier hardware gets
vibration-only escalation.

Developed as a submodule of the private `pebble-timetracking` superrepo, alongside
the other Sykerö Pebble apps (TimeStyle, Track Work Time, MIDI Recorder,
Countdown timer, Tuya Lights).

## Status

Build scaffolding only — the app currently shows a placeholder "Smart Alarm"
screen. Design docs (specs/plans) live in the superrepo under
`docs/superpowers/`, not here — this repository is intended to become public.

## Build

```bash
pebble build
pebble install --emulator basalt
```
