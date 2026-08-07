# Sykerö Alarm

Pebble watchapp — **Sykerö Alarm** ("Smart Alarm" on the watch launcher, "Sykerö
Smart Alarm" in the appstore).

An alarm clock built around two things the built-in Pebble alarm doesn't do
well:

- **A smart alarm that actually works.** PebbleOS's built-in smart alarm
  triggers on any detected movement at all, so it tends to fire on the very
  first check and wakes you the full window early, every time. This one reads
  the firmware's per-minute movement history, derives a wake threshold from
  **the user's own night** (a percentile over the night's movement, excluding
  wake episodes), and requires *sustained* above-normal movement over a
  leaky integrator before firing — a real lighter-sleep signal, not a twitch.
  There is always a hard deadline behind it, so a quiet night still rings on
  time.
- **A configurable wake escalation.** By default the vibration is at full
  strength from the first buzz at a constant gap, and only the sound ramps —
  a repeated too-gentle stimulus trains a sleeper to ignore the channel, so
  the gentle start is opt-in rather than the default. Sound joins later and
  ramps in volume on watches with a speaker. Three presets (Gentle / Normal /
  Insistent) plus a Custom profile expose the individual timing parameters.

Other features:

- Up to **8 alarm slots**, each with weekday repeat or one-time firing,
  enable/disable, skip-next and snooze.
- **Two presses of any ring-screen button** — top to snooze, bottom to stop,
  middle to pick a snooze length from a menu (5–60 min) — a single press only
  shows what the second would do, so a sleepy hand can't dismiss or snooze it
  by accident; `BACK` deliberately does nothing while it's ringing. Once inside
  the menu, choosing acts on one press: opening it was the deliberate act. A
  snooze keeps its own screen on the watch rather than dropping back to the
  watchface.
- **A waiting screen before the alarm**, an hour ahead by default (Off/15/30/60/
  90 min). Wake up by yourself at 06:40 and the screen is already there: two
  presses of the bottom button end the alarm, with no menus to find in the dark.
  It names both the alarm and the deadline when they differ (`Alarm 07:00` /
  `Rings by 07:30`), begins no smart window, and works with the smart alarm off
  — where it is the only screen there is before the ring.
- A **"Last night" summary** on the watch: sleep onset, the movement
  baseline, what triggered the alarm, and when it would have fired at each
  other sensitivity — the built-in calibration tool for deciding whether to
  move the percentile.

All configuration — alarm times, smart-alarm window and sensitivity, wake
profile, escalation timing, snooze behaviour — is done **phone-side via a Clay
config page**; the watch shows state and offers quick actions, but there is no
on-watch editing.

## How it works

**[docs/how-it-works.md](docs/how-it-works.md)** — suomeksi
**[docs/how-it-works.fi.md](docs/how-it-works.fi.md)** — explains what the app actually
does — what the alarm time means, how the wake threshold is derived from your
own night, how the escalation and snooze behave, when the app is running at
all, and what every setting changes. It is written for someone using the alarm
rather than maintaining it, and its charts are drawn from a real recorded
night rather than from illustrative data.

## Platform limits

- **The smart alarm needs the watch's activity/sleep tracking switched on**,
  and is **not available on aplite** (Pebble Classic / Pebble Steel have no
  motion-sensing API to read sleep data from). On aplite the app is a plain
  alarm clock with the same gradual escalation, just without the smart
  timing.
- **The escalation's sound stage needs a speaker** — Pebble Time 2 (emery) and
  Pebble 2 Duo (flint). On other platforms the escalation is vibration and
  backlight only; when there's no speaker (or it's muted), the vibration ramp
  compresses so it still reaches full strength instead of silently stopping
  halfway.
- **No app alarm can ring in the watch's low-power mode.** PebbleOS disables the
  wakeup service below `RunLevel_Normal`/`Stationary`, and every alarm this app
  schedules is a wakeup; the *built-in* alarm service is explicitly enabled at
  `RunLevel_LowPower` and a third-party app cannot be. Keep the watch charged.
- Target platforms: aplite, basalt, diorite, emery, flint.

Developed as a submodule of the private `pebble-timetracking` superrepo,
alongside the other Sykerö Pebble apps (TimeStyle, Track Work Time, MIDI
Recorder, Countdown timer, Tuya Lights).

## Build

```bash
pebble build
pebble install --emulator diorite   # or emery for the 200px colour boards
```

(`basalt` crashes headless during clock-face rendering — an unrelated,
board-specific pebble-fctx issue seen across this project's apps; use
diorite/emery for emulator work.)

## Test

```bash
npm test
```

Runs the JS test suite (`node --test tests/*.test.js`, covering the pack
contract and Clay config) **and** every host-side C test suite
(`tests/run_c_tests.sh`, plain-gcc host builds of `alarm_calc`, `escalation`,
`sleep_eval` and the phone↔watch pack contract) — no Pebble SDK or emulator
needed for either.

> The **Test alarm** and **Diagnostics** rows ship in every build and are both
> hidden until the phone-side "Debugging" toggle is switched on — there is no
> compile flag to remember at release time. See "Sending a diagnostic report"
> below.

## Design docs

Specs and implementation plans live in the superrepo, under
`docs/superpowers/{specs,plans}/`, not in this repository — this repo is
public, and design docs are kept in the (permanently private) superrepo by
convention across all of Sykerö's Pebble apps.

## Sending a diagnostic report

If the smart alarm wakes you at a bad moment — or does not wake you — the watch can
write down exactly what it measured and decided. The report is plain text: you can read
it before you send it, and you send it yourself.

It needs a terminal and a GitHub account, because the watch's log is only reachable
through the Pebble developer tool. If that is not for you, open **Last night** on the
watch and photograph the screen instead; it carries the numbers that matter most.

1. On your phone, in this app's settings, turn on **Debugging → Show the Diagnostics
   and Test alarm menu items**.
2. Install the Pebble tool (needs Python 3.10–3.13):
   `uv tool install --python 3.13 pebble-tool`
3. `pebble login` — sign in with **the same GitHub account as the Pebble app on your
   phone**.
4. In the Pebble app: **Devices → ⋯ → Enable Dev Connect**, signing in with that same
   account.
5. `pebble logs --cloudpebble`
6. On the watch, open **Smart Alarm → Diagnostics**. Leave the app open for about ten
   seconds; the report is written in small bursts.
7. Copy everything from `DBG ---- dump begin ----` to `DBG ---- dump end ----` from
   the terminal and attach it to your report. `pebble logs` prefixes every line with a
   timestamp and source location, so a line does not begin with `DBG` — it contains it.

**Run it the same morning.** The minute-by-minute movement data covers only the night
around the alarm that rang most recently, and it is not stored — the watch re-reads it
from Pebble Health each time. The seven night summaries are stored and survive, so a
later report is still useful, just less detailed.

## Licence

GPL-3.0 (see `LICENSE`).
