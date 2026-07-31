// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "alarm_calc.h"
#include "alarm_store.h"
#include <stdint.h>
#include <time.h>

// Wakeup cookies. Five of the eight per-app slots are used; three stay spare.
typedef enum {
  WC_DEADLINE = 1,   // the hard alarm time — always rings, never dropped
  WC_WINDOW   = 2,   // start of the smart window
  WC_SNOOZE   = 3,   // snooze expiry, and re-entry while ringing
  WC_REENTRY  = 4,   // rolling: revive the app if something killed it mid-window
  WC_DST      = 5,   // ~03:00 daily clock-shift check
} WakeCookie;

// How far ahead the rolling re-entry wakeup is placed. Must be > 60 s: PebbleOS
// refuses a wakeup within one minute of an already-scheduled one, system-wide.
#define SC_REENTRY_GAP_S  180

// Schedule `cookie` at `when`. On E_RANGE (another app already owns a wakeup
// within a minute) the time is shifted by +1, -1, +2, -2 minutes and retried;
// returns the time actually scheduled, or 0 if every attempt failed.
time_t sc_schedule(time_t when, WakeCookie cookie);

// Cancel every wakeup this app owns.
void sc_cancel_all(void);

// Re-arm all wakeups from scratch: cancel everything, then schedule in priority
// order with WC_DEADLINE first so that if slots run out the alarm still rings.
// Called at launch, after any settings change, and after every wakeup fires.
//
// Returns false if a wakeup this function treats as CRITICAL could not be
// scheduled (currently: the pending snooze wakeup, when RunState says one is
// due) -- true otherwise, including when there was nothing critical to arm.
// A caller that is about to exit the app on the strength of "the wakeup is
// armed" (ring_snooze_now) must check this rather than assume success.
bool sc_rearm(const Alarm *alarms, int count, const Config *cfg,
              const RunState *rs, time_t now);

// (Re)place the rolling re-entry wakeup SC_REENTRY_GAP_S from now.
void sc_arm_reentry(time_t now);

// When the ring must start at the latest for an alarm at `alarm_time`.
// SEMANTICS_RING_STARTS: alarm_time itself.
// SEMANTICS_AWAKE_BY:    alarm_time minus esc_full_development_s.
time_t sc_ring_deadline(const Config *cfg, time_t alarm_time);

// When the smart window opens: sc_ring_deadline minus smart_window_min. Equals
// sc_ring_deadline when the smart alarm is disabled or health is unavailable.
time_t sc_window_start(const Config *cfg, time_t alarm_time);
