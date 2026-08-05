// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "alarm_calc.h"
#include "alarm_store.h"
#include "sleep_eval.h"

#define SA_DEBUG_DUMP 1

// Development-build rows in the main menu: "Test alarm" and "Dump last night".
// Both are deliberate user actions that a shipped alarm clock has no business
// offering, and the dump in particular used to run on EVERY launch -- measured
// on the watch, that held the event loop for 1.5-4 s (paced APP_LOG bursts plus
// a synchronous read of up to 640 minutes of health history), which is the
// "buttons do not respond when I open the app" the user reported. A release
// build sets this to 0; the instrumentation itself can then stay in the tree.
#define SA_DEV_MENU 1

#if SA_DEBUG_DUMP

// Dump everything needed to reconstruct the past night from `pebble logs`:
// config, runstate, every alarm with the occurrences the app itself computes,
// a localtime/mktime round-trip probe, the stored night summaries, and the raw
// per-minute movement history around the alarm that last rang -- plus what
// se_evaluate makes of that history at every sensitivity.
//
// Emission is spread over app_timer ticks: a burst of ~100 log lines at once
// gets dropped by the debug link, and a dropped line looks exactly like a
// minute that was never recorded.
//
// `scratch` is borrowed (main's s_night buffer), so this must not run while a
// smart window is open -- it checks `rs` and declines rather than trusting the
// caller.
void dbg_dump(const Alarm *alarms, int count, const Config *cfg,
              const RunState *rs, SleepMinute *scratch, int scratch_len);

#else
#define dbg_dump(a, c, cfg, rs, s, sl) ((void)0)
#endif
