# Sykerö Smart Alarm

A Pebble alarm clock that wakes you a few minutes early, at a moment you are already
stirring — and never a minute later than the time you set.

PebbleOS's built-in smart alarm fires on the first movement it detects, which in
practice means the very start of the window, every night. This one reads the watch's
per-minute movement history, derives a threshold from **your own night** rather than a
fixed number, and waits for sustained above-baseline movement before it rings. A hard
deadline always sits behind it, so a quiet night still wakes you on time.

App name: **Sykerö Smart Alarm** · UUID `cba6bb36-e731-46b3-b5c7-915a997b8c61`.

**[Install from the Pebble appstore →](https://apps.repebble.com/a993645f974e4673a215a9e0)**

**How it works: [English](docs/how-it-works.md) · [suomeksi](docs/how-it-works.fi.md)**
— what the watch measures, how it decides, what every setting changes, and what the app
deliberately does not do. Worth ten minutes before your first night; its charts are
drawn from a real recorded night rather than from illustrative data.

## What it does

- **A smart alarm built from your own baseline** — a percentile over the night's
  movement (wake episodes excluded) sets the threshold, and a leaky integrator requires
  the rise to be *sustained* rather than a twitch. Window 10–60 min, four sensitivities.
- **A configurable wake escalation** — sound joins after a few minutes and ramps in
  volume on watches with a speaker, while the vibration is at full strength from the
  first buzz. (A gentle vibration start is available but off by default: a repeated
  too-gentle stimulus trains a sleeper to ignore the channel.) Presets Gentle / Normal /
  Insistent differ in buzz gap, when sound joins and how long the ring lasts; a Custom
  profile exposes all twelve numbers.
- **Up to 8 alarms**, each with weekday repeat or one-time firing, enable/disable,
  skip-next and snooze.
- **A stop gesture you cannot perform in your sleep** — two presses of any ring-screen
  button: top snoozes, bottom stops, middle opens a snooze-length menu (5–60 min). A
  single press only shows what the second would do, and `BACK` deliberately does nothing
  while the alarm rings.
- **A waiting screen before the alarm** (Off/15/30/60/90 min, default an hour ahead). Wake
  by yourself at 06:40 and the screen is already there — two presses end the alarm, with
  no menu to find in the dark.
- **A "Last night" summary on the watch** — sleep onset, the movement baseline, what
  triggered the alarm, and when it would have fired at each other sensitivity. This is the
  calibration tool: tune the sensitivity from your own data instead of guessing.
- **Phone-side configuration** via a Clay config page. The watch shows state and offers
  quick actions; there is no on-watch editing.

## Requirements

- **The smart alarm needs the watch's activity/sleep tracking switched on.** Without it
  the app is a plain alarm clock with the same escalation.
- **The sound stage needs a speaker** — Pebble Time 2 (emery) and Pebble 2 Duo (flint).
  Elsewhere the escalation is vibration and backlight, and a vibration ramp you turned on
  compresses so it still reaches full strength instead of stopping halfway.
- **Keep the watch charged: no app alarm can ring in the watch's low-power mode.**
  PebbleOS disables the wakeup service every app alarm depends on; only the built-in
  alarm service survives there. See
  [how it works §5](docs/how-it-works.md#5-when-the-app-is-actually-running).
- Platforms: **basalt, diorite, emery, flint**. aplite has no Health API for the smart
  alarm to read; chalk's round display clips the left edge of every screen this app draws.

## Building & running

```sh
pebble build                          # build for all targetPlatforms
pebble install --emulator diorite     # or emery for the 200px colour boards
pebble install --cloudpebble app.pbw  # install to a paired phone via the cloud relay
```

`basalt` crashes headless during clock-face rendering — an unrelated, board-specific
pebble-fctx issue seen across this project's apps; use diorite or emery for emulator work.

## Test

```sh
npm test
```

Runs the JS suite (`node --test tests/*.test.js` — AppMessage packing, config sync and the
Clay config page) **and** every host-side C suite (`tests/run_c_tests.sh` — plain-gcc
builds of `alarm_calc`, `escalation`, `sleep_eval`, the sleep-session reader, `night_text`
and the phone↔watch pack contract). Neither needs the Pebble SDK or an emulator.

## Project layout

```
src/c/alarm_calc.c    when an alarm is next due, and what its window is   ] pure C,
src/c/escalation.c    the vibration/sound/backlight ramp                  ] no SDK
src/c/sleep_eval.c    baseline, threshold and the fire decision           ] calls,
src/c/night_text.c    the "Last night" summary text                       ] host-tested
src/c/scheduler.c     wakeups, run state, the alarm cycle
src/c/health_read.c   reading Pebble Health's minute history
src/c/alarm_store.c   persisted alarms + config
src/c/main.c          windows, menus and the ring screen
src/ts/               phone side in TypeScript — Clay config + AppMessage packing
src/pkjs/             GENERATED from src/ts/ by tsc (gitignored — never edit)
docs/                 how-it-works (EN/FI) and the diagnostic-report guide
```

The four pure modules hold the hard logic and are the ones worth testing; keep SDK calls
out of them. Only modules touching `struct tm` need the `#ifdef __ARM_EABI__` include seam
in their header — the SDK ships no `<time.h>`.

## Documentation

- [How it works](docs/how-it-works.md) · [Miten se toimii](docs/how-it-works.fi.md) — the
  user-facing reference, and the argument for every decision the app makes.
- [Sending a diagnostic report](docs/diagnostic-report.md) — how to capture what the watch
  measured on a night it got wrong.
- Full SDK docs, tutorials and API reference: <https://developer.repebble.com>

Specs and implementation plans are kept outside this repository, in the private superrepo
this app is developed in — a convention shared by all of Sykerö's Pebble apps.

## Support

Questions, feedback or bug reports: <pebble.smartalarm@sykero.fi>

Browse all Sykerö Software apps on the Pebble appstore:
<https://apps.repebble.com/apps/dev/syker-software_9f6c9c6e9ce88af6a0db953e>

## Licence

GPL-3.0 (see `LICENSE`).
