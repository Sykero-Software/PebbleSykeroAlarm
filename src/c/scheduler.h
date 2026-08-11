// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "alarm_calc.h"
#include "alarm_store.h"
#include <stdint.h>
#include <time.h>

// Wakeup cookies. Six of the eight per-app slots are used; two stay spare.
typedef enum {
  WC_DEADLINE = 1,   // the hard alarm time — always rings, never dropped
  WC_WINDOW   = 2,   // start of the smart window
  WC_SNOOZE   = 3,   // snooze expiry, and re-entry while ringing
  WC_REENTRY  = 4,   // rolling: revive the app if something killed it mid-window
  WC_DST      = 5,   // ~03:00 daily clock-shift check
  WC_PREALARM = 6,   // the pre-alarm waiting screen opens (no cycle, no ring)
} WakeCookie;

// How far ahead the rolling re-entry wakeup is placed. Must be > 60 s: PebbleOS
// refuses a wakeup within one minute of an already-scheduled one, system-wide.
#define SC_REENTRY_GAP_S  180

// Schedule `cookie` at `when`. On E_RANGE (another app already owns a wakeup
// within a minute) the time is shifted by +1, -1, +2, -2 minutes and retried;
// returns the time actually scheduled, or 0 if every attempt failed.
//
// WC_DEADLINE is scheduled with PebbleOS's notify_if_missed flag set, so a
// deadline that passes while the watch is off raises the firmware's boot popup;
// every other cookie is scheduled without it. See prv_notify_if_missed in the
// .c file for why the flag is this narrow -- it is a deliberate policy, not an
// oversight, and widening it produces duplicate and untrue warnings.
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
// armed" (ring_snooze_for) must check this rather than assume success.
//
// `ringing` must be the caller's live "an alarm is ringing right now" state.
// It cannot be derived from RunState: a first, un-snoozed ring has
// snooze_count == 0, which is indistinguishable in RunState from a ring that
// has ended. Because this function cancels EVERY wakeup first, a re-arm during
// a ring (e.g. a phone config save arriving mid-ring -> reload_and_rearm)
// otherwise dropped start_ring's mid-ring keep-alive and never replaced it --
// after which an eviction lost the ring with nothing scheduled to bring it
// back. With `ringing` true the keep-alive is re-armed in place of the snooze
// wakeup.
bool sc_rearm(const Alarm *alarms, int count, const Config *cfg,
              const RunState *rs, time_t now, bool ringing);

// (Re)place the rolling re-entry wakeup SC_REENTRY_GAP_S from now.
void sc_arm_reentry(time_t now);

// When the ring must start at the latest for an alarm at `alarm_time`, and when
// its smart window opens. Thin wrappers that resolve Config + the platform's
// Health availability and defer to ac_ring_deadline/ac_window_start, which are
// pure and host-tested for all three semantics -- see alarm_calc.h.
time_t sc_ring_deadline(const Config *cfg, time_t alarm_time);
time_t sc_window_start(const Config *cfg, time_t alarm_time);

// The slot index of the next occurrence that has not already rung, or -1;
// writes that occurrence's time to *alarm_when, or 0 when it returns -1
// (ac_next_alarm_unserved's own -1 path writes *out_when = 0, so this
// documents what the code does rather than leaving it as an out-parameter
// whose value on failure a future caller would have to guess at or re-derive
// from the .c file).
// Shared -- not just factored out of sc_rearm for tidiness -- because the
// pre-alarm wakeup handler (main.c) must name the exact same occurrence
// sc_rearm armed. A second private copy of this walk is precisely the defect
// backlog item 25 records: prv_dump_alarm kept its own copy of a loop that had
// been fixed in prv_pick_alarm, and stayed wrong for five days.
int sc_next_unserved(const Alarm *alarms, int count, const Config *cfg,
                     const RunState *rs, time_t now, time_t *alarm_when);

// The two Config values resolved above, exposed for the callers that pass them
// STRAIGHT to the pure math (ac_window_wakeup) rather than going through the two
// wrappers. One owner: sc_window_active is the only place that folds the setting,
// a zero window length and the platform's Health availability into "the smart
// window is active", and sc_full_dev_s the only place that resolves the effective
// escalation profile's full development time.
bool sc_window_active(const Config *cfg);
uint32_t sc_full_dev_s(const Config *cfg);
