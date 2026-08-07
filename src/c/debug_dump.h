// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "alarm_calc.h"
#include "alarm_store.h"
#include "sleep_eval.h"

// Must stay 1 in any build shipped with the phone's Debugging toggle: that
// toggle only reveals the Diagnostics menu row, it does not compile the dump
// in -- with this at 0 the toggle would show, and do nothing.
#define SA_DEBUG_DUMP 1

// There is deliberately NO compile flag for the developer menu rows any more.
// Both of them -- "Diagnostics" and "Test alarm" -- are gated at RUNTIME by the
// phone's Debugging toggle (Config.debug_features, see main.c's main_rows()),
// because a user has to be able to produce a bug report from a released build,
// and because a release that depends on someone remembering to set a compile
// flag is a hazard nothing enforces. A release build needs no flag edit at all.
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
