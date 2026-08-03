# How Sykerö Smart Alarm works

*Suomeksi: [how-it-works.fi.md](how-it-works.fi.md)*

This describes what the app actually does, in enough detail that you can predict
its behaviour — and argue with it — without reading the code. It is written for
someone using the alarm, not maintaining it.

**The promise:** it rings. Everything clever in here sits *in front of* an ordinary
alarm that goes off at the time you set, and none of it can push that time back. If
the movement data is missing, the watch was off your wrist, or the detector simply
never finds a good moment, you still get woken at your alarm time.

There is exactly one exception, and it is worth knowing before you rely on this app:
**if the battery drops far enough for the watch's own low-power mode, no app alarm
can ring** — the watch switches off the service every app alarm depends on. See
§5. Charge the watch, or keep a built-in alarm as a backstop on a night that matters.

---

## The short version

You set alarms on your phone. The watch keeps them, and shortly before an alarm is
due it starts watching how much you move, minute by minute. When it sees you stir —
measured against how still *you* have been tonight, not against a fixed number — it
rings then, on the theory that waking out of light sleep is easier than being
dragged out of deep sleep a few minutes later. If it never sees a good moment, the
alarm time itself rings it.

---

## 1. What the alarm time means

This is the first thing to get right, because all three answers are reasonable and
the app cannot guess which one you meant.

![The three meanings of the alarm time](img/semantics.svg)

- **The latest — may ring earlier.** *(default)* A 07:50 alarm means "have me up by
  07:50". The watch watches 07:20–07:50 and rings at the first good moment.
- **The earliest — may ring later.** 07:50 means "don't wake me before 07:50". The
  watch watches 07:50–08:20 and rings by 08:20 at the latest.
- **When I must be fully awake.** 07:50 is the moment the alarm should be at *full
  strength*, so the ramp starts early enough to have developed by then.

Switching the smart alarm off makes all three ring at exactly the time you set.

The window length (10–60 minutes, default 30) is what "shortly before" means, and
it is the app's central trade: a longer window gives the detector more chances to
find a light-sleep moment, and gives up more of your morning to do it.

---

## 2. What the watch measures

The watch's health service records, for every minute of the day, a **VMC** — a
"vector magnitude count", roughly how much the wrist moved that minute — plus a
coarse **orientation** (which way the wrist is facing). This happens all the time,
whether or not this app is running, which is what makes the whole design possible:
the app can be asleep all night and still read the whole night back when it wakes.

Here is a real night, exactly as the watch recorded it — the data below is not an
illustration, it is a dump of one night's minutes off the author's own watch:

![Movement through one night, with the trigger level and the moment the alarm rang](img/night.svg)

### The threshold is built from your own night

There is no fixed "this much movement means awake". What counts as a stir depends
entirely on how still you have been:

1. **Find where sleep started.** The watch's own sleep session is used when it has
   one; otherwise the app looks for the first long quiet stretch. The first
   **20 minutes** after that are thrown away — falling asleep is not representative
   of the night.
2. **Take everything from there up to the start of the window** — and *only* up to
   the start of the window. The window is what is being judged, so letting it into
   its own reference population would raise the bar every time you stirred.
3. **Drop wake episodes.** Any run of **8 or more consecutive minutes** above
   4 × the resting median (plus a small fixed margin) is not sleep — it is you
   being awake — and it is removed
   entirely. A single restless hour would otherwise set a threshold so high that
   nothing later in the night could clear it. Short bursts are kept: an ordinary
   turn-over is data, not an outlier.
4. **Take a percentile of what remains.** That is the **trigger level**. On the
   night above, the population was 175 minutes, 86 % of them exactly zero, and the
   90th percentile came out at **587**.

The consequence is worth stating plainly: **on a very still night the threshold is
low, and an ordinary turn-over will clear it.** That is the design working, not
misfiring — but it does mean a still sleeper gets woken nearer the start of the
window than a restless one.

### Magnitude *and* duration

Clearing the threshold for one minute is not enough. Each minute inside the window,
an accumulator gets:

- **the excess**, if the minute's movement is above the trigger level, and
- **a bonus of 400**, if the wrist has turned over — its orientation differs from
  the rolling mode of the preceding ten minutes by at least 2 of 16 steps.

A minute that contributes neither **resets the accumulator to zero**. The alarm
fires when the accumulator has reached one full trigger level *and* the run of
contributing minutes is long enough (1–5 minutes; 2 by default). So a single spike —
a cough, the watch knocking the headboard — cannot ring it, however large.

There is no separate decay constant: because the run resets on any quiet minute,
the accumulator drains by construction.

### Two vetoes

- **If the firmware says you are in deep sleep right now**, an early wake is
  refused. The deadline still applies.
- **If there are fewer than 60 usable minutes** to build a distribution from, the
  smart alarm stands down entirely and the alarm time rings it. The watch says
  *"smart alarm unavailable"* rather than pretending.

### Sensitivity

The only thing this setting changes is which percentile becomes the trigger level:

| Setting | Percentile | Effect |
|---|---|---|
| Low — only a clear stir | 95 | Fires rarely; you are more likely to be woken by the alarm time itself |
| **Medium** *(default)* | 90 | |
| High — the slightest stir | 82 | Fires early and often |
| Custom | your choice, 70–99 | Also lets you set the sustained-for length, 1–5 minutes |

The **Last night** screen on the watch exists to make this decidable rather than
guessy: it shows what the trigger level was, when the alarm actually fired, and
**when each of the other sensitivities would have fired on the same night**. If
Low says `--:--`, it would not have fired at all — you would have been woken by
the alarm time.

---

## 3. How it wakes you

![How vibration and sound escalate during a ring](img/escalation.svg)

A ring is a series of **bursts** — a few vibration pulses, then a gap, repeat. Sound
joins later, if the watch has a speaker (only some models do).

**By default the vibration does not ramp.** It is at full strength from the first
buzz, at a constant gap, and only the sound ramps up. This is deliberate and it is a
safety argument rather than a preference: a repeated too-gentle stimulus trains a
sleeper to ignore that channel, and degrades the stronger pulses that follow it.
Turning **"Ramp the vibration up"** on restores a gentle start that tightens over
several minutes.

The **wake style** presets differ in gap, how long the ring lasts before it gives
up, when sound joins, and how loud it gets:

| Style | Gap between buzzes | Sound joins | Max volume | Gives up after |
|---|---|---|---|---|
| Gentle | 45 s | 8 min | 70 | 20 min |
| **Normal** *(default)* | 30 s | 5 min | 100 | 15 min |
| Insistent | 15 s | 1 min | 100 | 15 min |
| Custom | all twelve numbers are yours | | | |

When the ring gives up, it stops making noise but leaves **"Alarm missed"** on the
screen, so the morning tells you what happened.

### Stopping and snoozing

On the ringing screen: **UP snoozes**, and **two presses of the bottom button stop
it**. One press never stops an alarm — a half-asleep hand finds one button by feel,
and that is exactly how an alarm gets dismissed by accident.

Snooze is 10 minutes by default, 5 snoozes maximum; when they run out, the snooze
button behaves as Stop rather than going inert. **Each snooze restarts the
escalation from the beginning.** (You can change that: raising *"each snooze starts
this far along"* makes a second alarm pick up further along the ramp — i.e. start
stronger than the first one did.)

---

## 4. The screens on the watch

Alarms are set on the phone. The watch has three screens and no way to create an
alarm time — deliberately, since a phone keyboard beats four buttons.

**The menu** — the next alarm and how far away it is, the alarm list, "Last night",
and a Test alarm that rings two minutes from now so you can feel your settings
without waiting for morning.

**The alarm list** — every alarm with its state in words (`in 23 h`, `skip, in 1 d`,
`off`). Pressing SELECT on one opens a small menu that spells out what it will do:
*Skip Mon 07:50* (this one occurrence only), *Turn off*, *Turn on*.

**The waiting screen** — shown while the window is open, white on black so it is not
a torch in the face at 3 a.m. Two ways off it, both needing two presses so neither
happens by accident:

- **DOWN twice — cancel the alarm.** It will not ring, and it will not come back.
- **BACK twice — go to the watchface.** The window stays open, so the screen
  returns within about three minutes.

**The ringing screen** — the time, the alarm, and how to stop it.

---

## 5. When the app is actually running

![When the alarm app actually runs during a night](img/process.svg)

Pebble apps have no background thread: only one app runs at a time, and the
watchface counts as one. This app is therefore *not running* for almost the whole
night. It works by asking the watch to wake it at specific moments:

- **at the window opening**, and then every three minutes while the window is open,
- **at the alarm time**, as the deadline that must always fire,
- **once a night at 03:00**, purely to re-arm everything after a possible
  daylight-saving shift.

This is why the movement data is read back from the firmware's own history rather
than sampled live — and why the app survives being killed, the watch rebooting, or
you opening something else: the wakeups are stored by the watch, not held in the
app's memory. It also means that when the app is *not* running, it looks at your
movement every three minutes rather than every minute.

### What can still go wrong

| Situation | What happens |
|---|---|
| Watch off your wrist, or activity tracking off | No usable data → "smart alarm unavailable", the alarm time rings |
| Watch switched off over the alarm | The alarm is missed; the watch reports it when it boots |
| Phone away or Bluetooth off | No effect. Alarms live on the watch; the phone is only for editing them |
| Quiet Time on | **No effect on the vibration** — this is an alarm, and it deliberately ignores Quiet Time. Sound can still be silenced if you have switched on "mute speaker during Quiet Time" |
| Battery low enough for the watch's low-power mode | **The alarm cannot ring.** In that mode the watch switches its wakeup service off entirely, and every app alarm — this one included — depends on it. The watch's *built-in* alarm keeps working, because it is wired to survive that mode and an app cannot be. Charge the watch, or set a built-in alarm as a backstop on a night that matters |

---

## 6. Every setting, and what it actually changes

Sections marked *Custom* only matter if you have set the corresponding picker to
Custom; otherwise the preset supplies those numbers.

### Alarms

| Setting | Default | What it changes |
|---|---|---|
| Alarm list | one 07:00 alarm | Up to 8 alarms, each with a time, weekdays, and an on/off switch |

### Smart alarm

| Setting | Default | What it changes |
|---|---|---|
| Smart alarm | on | Off makes every alarm ring exactly at its time and nothing below matters |
| Smart window length | 30 min | How much of your morning the detector is allowed to use |
| The alarm time is | the latest | Which side of the alarm time the window sits on — see §1 |
| Sensitivity | Medium | Which percentile of your own night becomes the trigger level |
| *Custom:* stir percentile | 90 | The percentile itself, 70–99 |
| *Custom:* sustained for | 2 min | How many consecutive contributing minutes are required |

### How it wakes you

| Setting | Default | What it changes |
|---|---|---|
| Ramp the vibration up | **off** | On: a gentle start that tightens. Off: full strength from the first buzz |
| Wake style | Normal | A preset for all twelve numbers below |
| *Custom:* gap between buzzes | 30 s | Silence between bursts at the start |
| *Custom:* final gap | 5 s | Silence between bursts once fully ramped (only reachable with the ramp on) |
| *Custom:* tighten over | 360 s | How long the ramp takes (only with the ramp on) |
| *Custom:* first pulse | 80 ms | Pulse length at the start (only with the ramp on) |
| *Custom:* pulse length | 700 ms | Pulse length once fully ramped — and from the very first buzz with the ramp off |
| *Custom:* first burst pulses | 1 | Pulses per burst at the start (only with the ramp on) |
| *Custom:* pulses per buzz | 3 | Pulses per burst once ramped — and from the first buzz with the ramp off |
| *Custom:* sound joins after | 300 s | When sound starts. **No effect on a watch without a speaker** |
| *Custom:* volume ramp | 300 s | How long volume takes to reach its maximum. Speaker only |
| *Custom:* first volume | 15 | Volume when sound joins. Speaker only |
| *Custom:* max volume | 100 | Loudest it gets. Speaker only |
| *Custom:* give up after | 900 s | How long the ring lasts before it stops making noise |

### Snooze and stopping

| Setting | Default | What it changes |
|---|---|---|
| Snooze length | 10 min | Off disables snoozing entirely (the button then stops the alarm) |
| Snoozes allowed | 5 | Unlimited is allowed; the ring never goes silent because of it |
| Each snooze starts this far along | 0 s | 0 restarts the ramp each time. Higher makes each snooze start stronger |

---

## 7. What it deliberately does not do

- **It does not respect Quiet Time.** An alarm exists to ring while the watch is
  otherwise silent. The watch's own API calls that setting advisory; for an alarm
  the right answer is to ignore it.
- **It does not stage sleep.** There is no REM/deep/light model here. It measures
  movement, which correlates with sleep depth well enough to pick a moment, and it
  says so rather than dressing it up.
- **It does not use the accelerometer directly.** That would need the app running
  all night, which Pebble does not allow and your battery would not survive. The
  firmware's own minute history is both cheaper and more complete.
- **It does not create alarms on the watch**, apart from the two-minute Test alarm.

---

## Appendix — where the numbers came from

The night chart is real data, and it can be checked: `img/night-2026-08-01.txt` is
the 640 per-minute values the watch recorded, and `img/make_diagrams.py` redraws
every diagram in this document from that file plus the constants in the source. The
threshold drawn on the chart (587) is what the watch itself computed and recorded
that morning, and recomputing the 90th percentile from the published data
reproduces it exactly.
