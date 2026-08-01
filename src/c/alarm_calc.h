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

#endif
