// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "alarm_calc.h"
#include "alarm_store.h"
#include "sleep_eval.h"

// TEMPORARY DEBUG INSTRUMENTATION -- flip to 0 (or delete both files) before
// publishing. Added 2026-08-01 to diagnose a ring that started ~30 minutes
// before a 07:50 alarm on the real watch: the smart window opens exactly then,
// so the question is whether se_evaluate fired on real movement or fires at
// window start no matter what. Neither the emulator nor a host test can answer
// that -- only the watch's own recorded night can, and the only channel out of
// the watch is `pebble logs`.
#define SA_DEBUG_DUMP 1

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
