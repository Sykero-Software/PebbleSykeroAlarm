// SPDX-License-Identifier: GPL-3.0-only
#ifndef ALARM_CALC_H
#define ALARM_CALC_H
#include <stdbool.h>
#include <stdint.h>

// The one platform seam in this otherwise pure module. The Pebble SDK ships no
// <time.h>: it declares struct tm, localtime and mktime in pebble.h itself
// (sdk-core/pebble/<board>/include/pebble.h), and the arm toolchain's own
// <time.h> leaves struct tm incomplete — so a watch build that includes only
// <time.h> fails with "variable 'tm' has initializer but incomplete type" and
// implicit declarations of localtime/mktime. On the host, <time.h> is exactly
// right. Keeping the conditional HERE means alarm_calc.c and every host test
// stay free of platform #ifdefs.
#ifdef __ARM_EABI__
#include <pebble.h>
#else
#include <time.h>
#endif

#define MAX_ALARMS 8

// What the time the user set actually MEANS. These live here, with the alarm time
// math that consumes them, rather than in alarm_store.h with the rest of Config:
// ac_ring_deadline/ac_window_start below are the definition of each mode, and a
// pure module cannot include alarm_store.h (which includes this one).
//
// RING_LATEST is the historical default and was labelled "Ringing starts then",
// which a real user reasonably read as "not before then" -- it means the
// opposite, "not after then", and the smart window only ever moves the ring
// EARLIER. RING_FROM is that other reading, added 2026-08-01 because the label
// was genuinely ambiguous and both behaviours are legitimate.
#define SEMANTICS_RING_LATEST  0   // window [T - w, T]; ring at T at the latest
#define SEMANTICS_AWAKE_BY     1   // full escalation developed by T
#define SEMANTICS_RING_FROM    2   // window [T, T + w]; never ring before T
// The old spelling, kept so nothing silently changes meaning mid-refactor.
#define SEMANTICS_RING_STARTS  SEMANTICS_RING_LATEST

// One alarm slot. weekday_mask bit0=Monday … bit6=Sunday; 0 means a one-time
// alarm (fires at the next occurrence of minute_of_day, then is disabled by the
// caller). skip_next makes the *next* occurrence be skipped exactly once.
typedef struct {
  uint16_t minute_of_day;   // 0..1439
  uint8_t  weekday_mask;
  bool     enabled;
  bool     skip_next;
} Alarm;

// Next time this alarm fires, strictly after `now`, in local wall-clock terms.
// Returns 0 if it never fires (disabled).
//
// Computed via localtime/mktime rather than by adding multiples of 86400, so a
// 07:00 alarm stays at wall-clock 07:00 across a DST transition: the real time
// that elapses is an hour shorter (spring) or longer (autumn) than the wall-clock
// difference, which is the correct behaviour for an alarm clock.
time_t ac_next_occurrence(const Alarm *a, time_t now);

// Index of the alarm that fires soonest, or -1 if none will. On success writes
// that alarm's time to *out_when (may be NULL).
int ac_next_alarm(const Alarm *alarms, int count, time_t now, time_t *out_when);

// Like ac_next_alarm, but never returns an occurrence that HAS ALREADY RUNG.
//
// A ring can end BEFORE its own alarm time -- that is the whole point of the
// smart window -- and then "the next occurrence strictly after now" is still
// today's, the very one that just rang. Re-arming it made a 07:50 alarm whose
// window opened at 07:20 ring twice, at 07:20 and again at 07:50, with no
// snooze involved (reported from the wrist 2026-08-01). Every path that ends or
// interrupts a ring re-arms -- Stop, the missed-alarm cap, a phone config save,
// the nightly DST check, an ordinary app launch -- so the fact "this occurrence
// has rung" has to be persisted state consulted here, not a one-off adjustment
// at the call site that ends the ring.
//
// `served_slot` is the slot whose ring already happened and `served_at` that
// ring's instant (RunState.served_slot/served_at); served_slot < 0 means nothing
// has rung and this behaves exactly like ac_next_alarm. `lead_s` is how far
// before the occurrence a ring starts (0 for "ringing starts at the set time",
// the escalation's full development time for "awake by"), so the comparison is
// made in ring instants and holds under both semantics.
//
// The slot must match as well as the instant: an alarm at 07:30 must still ring
// after the 07:50 one rang early at 07:20, and only same-slot occurrences are a
// day or more apart, which is what makes the <= comparison (and the tolerance
// below) safe.
//
// A ring may start slightly BEFORE the instant it was scheduled for: sc_schedule
// shifts a wakeup by +/-1 and +/-2 minutes when another app already owns one
// within a minute (E_RANGE), so a deadline ring for 07:50 can begin at 07:48 and
// record 07:48 as served. Without the tolerance below, "the next occurrence
// after 07:49" is again today's 07:50 and the same alarm rings twice -- the same
// defect as the early smart wake, by a rarer route.
#define AC_SERVED_TOLERANCE_S 180

// True when the occurrence `when` of slot `slot` is the one already recorded as
// rung. Exposed separately so the wakeup handler can refuse to (re)open a window
// for it without restating the tolerance -- one owner for the comparison.
bool ac_is_served(time_t when, int slot, int served_slot, time_t served_at,
                  int32_t lead_s);

int ac_next_alarm_unserved(const Alarm *alarms, int count, time_t now,
                           int served_slot, time_t served_at, int32_t lead_s,
                           time_t *out_when);

// When the ring must start AT THE LATEST for an alarm set to `alarm_time`, and
// when its smart window opens. Pure, so all three modes are host-tested: this is
// the mapping that decides when an alarm actually goes off, and it has now been
// misread once by a user and once (as SEMANTICS_AWAKE_BY vs an imminent alarm) by
// the code itself.
//
// `smart_window_active` must already fold in everything that can disable the
// window -- the setting, a zero length, and whether the platform has Health at
// all -- so this stays free of SDK macros. `full_dev_s` is
// esc_full_development_s for the effective profile; it is only read by
// SEMANTICS_AWAKE_BY.
//
// With the window inactive every mode collapses to `alarm_time`, which is what
// makes "smart alarm off" mean "rings exactly when you said" under all three.
time_t ac_ring_deadline(uint8_t semantics, bool smart_window_active,
                        uint16_t window_min, uint32_t full_dev_s,
                        time_t alarm_time);
time_t ac_window_start(uint8_t semantics, bool smart_window_active,
                       uint16_t window_min, uint32_t full_dev_s,
                       time_t alarm_time);

// Drop every SPENT one-time alarm -- weekday_mask == 0 and not enabled -- from
// alarms[0..count), compacting the survivors down. Returns the new count.
//
// A one-time alarm that is switched off has no future occurrence, so it is a dead
// row. That would be cosmetic if anything could delete it, and nothing can: the
// watch has no delete by design (times are set from the phone), and the phone
// cannot delete one IT NEVER KNEW ABOUT -- the watch's own "Test alarm" writes a
// slot straight into the array without touching the recorded AlarmSet string, so
// the phone's next config is byte-identical to the last applied one and
// ac_apply_set_if_changed correctly treats it as a no-op. The result, seen on the
// real watch 2026-08-01: two spent test alarms (00:00 and 00:11) that the user
// could not remove from either side.
//
// `missed` is compacted alongside (it is indexed by slot), and *pending_slot /
// *served_slot are remapped -- to -1 if their own slot was the one dropped. Any
// of the three may be NULL. Getting this wrong would alias one alarm's state onto
// another, which is the same index-drift class as the positional message keys.
int ac_prune_spent_one_time(Alarm *alarms, int count, bool *missed,
                            int8_t *pending_slot, int8_t *served_slot);

// What the alarm list's per-row submenu offers, in display order.
//
// This is the app's answer to a UX defect, not a convenience: the list shipped
// with SELECT toggling on/off and long SELECT setting skip-next, neither of them
// mentioned anywhere on screen, and the first user pressed SELECT expecting the
// skip and switched the alarm off instead. The rows are state-dependent because a
// menu that offers an action the alarm is already in cannot be told from a broken
// button.
typedef enum {
  AC_ACTION_SKIP_NEXT = 0,   // set skip_next   -- "Skip Mon 07:50"
  AC_ACTION_RING_NEXT,       // clear skip_next -- "Ring Mon 07:50"
  AC_ACTION_TURN_OFF,        // enabled = false -- "Turn off"
  AC_ACTION_TURN_ON,         // enabled = true  -- "Turn on"
} AcAction;

#define AC_MAX_ACTIONS 2

// Writes the actions for `a` into out[0..max) and returns how many. Returns 0 on a
// NULL argument or max < 1 rather than assuming there is room.
int ac_row_actions(const Alarm *a, AcAction *out, int max);

// Parse the packed AlarmSet wire format into out[]:
//     "07:00|1111100;-08:30|0000011"
// slots separated by ';', each "[-]HH:MM|DDDDDDD" where the seven digits are
// Mon..Sun and a leading '-' means the slot is disabled. Malformed slots are
// skipped. Returns the number of alarms written (<= max).
//
// Hand-rolled digit parsing on purpose: atoi/strtol are not exported by the
// Core Devices firmware and hard-fault on hardware.
int ac_parse_set(const char *s, Alarm *out, int max);

// Apply `incoming` ONLY if it differs, byte for byte, from `last_applied` (the
// AlarmSet string that produced the alarms currently in out[]). Returns true when
// out[]/count were replaced, false when the resend was a no-op.
//
// This is the invariant it exists to hold: A CONFIG RESEND THAT CHANGES NOTHING
// MUST NOT CHANGE ANYTHING. `enabled` and `skip_next` are watch-mutable BY DESIGN
// -- SELECT toggles an alarm on/off, long SELECT skips its next occurrence, and
// ring_stop_now disables a fired one-time alarm -- and none of that is ever sent
// back to the phone. But ac_parse_set rebuilds every slot from `{0}` with
// `enabled` taken from the phone's saved '-' bit and `skip_next` forced to false,
// so re-parsing an unchanged string silently reverts all three. That was harmless
// while the phone's dict only arrived on an explicit Save; it is not harmless now
// that the watch asks for its config on EVERY launch, because dst_check (on by
// default) launches the app around 03:00 every night: disable tomorrow's alarm
// from the wrist at 22:00, and the 03:00 launch would re-enable it and ring at
// 07:00 anyway.
//
// A GENUINE phone-side change still takes precedence in full, including a
// phone-side disable -- that is why this compares the string rather than merging
// per-slot: merging by minute_of_day+weekday_mask would make a phone-side disable
// undeliverable, since it is indistinguishable from a watch-side one.
//
// `last_applied` may be NULL or "" (nothing recorded yet), which always applies.
bool ac_apply_set_if_changed(const char *incoming, const char *last_applied,
                             Alarm *out, int *count, int max);

// Is a snooze in flight at `now`? While one is, RunState.ring_started_at holds
// its EXPIRY rather than a ring start (ring_snooze_now moves it forward so the
// escalation ramp resumes in the right place), so all three parts matter: a
// non-zero count, a non-zero stamp, and an expiry still ahead of us. Written
// out by hand at every call site before this existed, and about to be written
// out at three more -- the pending screen's launch check and both menus.
//
// Plain integers in, bool out: the caller passes RunState fields, so this stays
// a pure function that a host test can reach without alarm_store.h (which
// includes this header, so it could not be included back).
bool ac_snooze_pending(uint8_t snooze_count, uint32_t ring_started_at, time_t now);

#endif
