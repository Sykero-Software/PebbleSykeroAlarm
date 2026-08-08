# Sending a diagnostic report

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

Send the report to <pebble.smartalarm@sykero.fi>, or attach it to a GitHub issue.

## What the report contains

Only what this app itself knows: your alarm times and settings, the watch's clock and
time zone, the last seven night summaries, and — for the night around the most recent
alarm — the sleep sessions and per-minute movement figures the watch read back from
Pebble Health, the threshold it derived from them, and what it decided to do. There is
no location, no account identity and no data from any other app. It is plain text —
read it first if you want to.

See [how-it-works.md](how-it-works.md) for what those numbers mean.
