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

// How many occurrences ac_last_past_occurrence will walk before giving up. Both
// callers search a 25-hour span (at most two occurrences of one alarm), so this
// is slack, not a limit anyone should be near.
#define AC_LAST_PAST_MAX_STEPS 40

// The most recent occurrence of `a` at or before `now`, searching forward from
// `search_from`; 0 if the alarm has none in that span (or is disabled).
//
// This is NOT `ac_next_occurrence(a, now - <span>)`, and the difference is the
// reason the function exists. ac_next_occurrence returns the FIRST occurrence
// after its base, so for a daily alarm that spelling answers with the occurrence
// 24 h too early whenever `now` is less than an hour past the alarm -- and
// answers correctly for the rest of the day, so it reads as working. It was
// written twice in the debug dump; one copy was fixed in place and the other was
// not, and the dump of 2026-08-06 (taken 17 min after a 07:50 alarm) printed
// `a0 prev=08-05 07:50` beside `eval alarm=08-06 07:50` -- one artefact naming
// two different alarms as the one that had just rung.
//
// `now` itself counts as past: at the ring instant the occurrence in hand is the
// one ringing, not yesterday's. `search_from` after `now` yields 0.
//
// Disabled/skip_next are honoured as ac_next_occurrence honours them, so a
// caller that wants the raw grid regardless of those two (both dump sites do)
// passes a probe copy with them forced, exactly as it would for ac_next_occurrence.
time_t ac_last_past_occurrence(const Alarm *a, time_t now, time_t search_from);

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

// WHICH ALARM IS A WC_DEADLINE/WC_SNOOZE WAKEUP ACTUALLY FOR?
//
// Not "which cycle is in progress" -- that is RunState.pending_slot, and reading
// it as the answer to this question is the defect this function exists to fix
// (backlog item 20, 2026-08-05). sc_rearm arms the next UNSERVED alarm's own
// deadline at priority 1 regardless of which alarm is mid-cycle, so a second
// alarm's deadline routinely lands inside a first alarm's live cycle. Ringing
// the first alarm there restarted its ramp, destroyed its snooze, mis-stamped
// the served record, and lost the second alarm silently until the next day.
//
// The wakeup cookie carries no slot (it cannot -- a WakeupId is lost when the
// app is killed), so the answer has to be re-derived from the clock, the alarms
// and RunState. That is pure arithmetic, hence this module.
typedef enum {
  AC_WAKE_NONE = 0,       // nothing due and no live cycle: end the stale cycle, re-arm
  AC_WAKE_KEEP,           // nothing due but a cycle IS live: re-arm only, touch nothing
  AC_WAKE_RESUME,         // continue this cycle's ring: a snooze expired, or a mid-ring
                          // keep-alive arrived after an eviction. One outcome for both,
                          // because ring_started_at is the same field either way and both
                          // want from_deadline = false, so the escalation ramp resumes
                          // instead of restarting at its gentlest stage.
  AC_WAKE_RING_DEADLINE,  // this alarm's own deadline is due: a fresh ring
} AcWakeAction;

typedef struct {
  AcWakeAction action;
  int          slot;      // meaningful for AC_WAKE_RESUME and AC_WAKE_RING_DEADLINE; -1 otherwise
} AcWakeDecision;

// `lead_s` is the occurrence-to-deadline offset the callers already compute
// (now - sc_ring_deadline(cfg, now)), so an occurrence's deadline is
// `when - lead_s`. Signed: SEMANTICS_RING_FROM puts the deadline AFTER the alarm
// time. `ring_started_at` and `snooze_count` are RunState's, with
// ring_started_at's usual overload (the ring start during a ring, the snooze
// EXPIRY while a snooze is in flight). A due deadline is checked FIRST, so a
// second alarm's deadline beats a pending snooze even when the two coincide --
// the user's decision (2026-08-05): a backup alarm that stays silent is useless.
AcWakeDecision ac_dispatch_wakeup(const Alarm *alarms, int count, time_t now,
                                  int pending_slot, uint32_t ring_started_at,
                                  uint8_t snooze_count,
                                  int served_slot, time_t served_at, int32_t lead_s);

// WHAT IS ONGOING RIGHT NOW? The single classifier for RunState's one live
// cycle, so the menu row, the pending screen's caption and the launch-time
// repair cannot describe the same state differently (they did: an open window
// was reported by no screen at all, and a cycle whose owning alarm had been
// pruned by ac_prune_spent_one_time was reachable from nothing -- it still
// re-armed the rolling re-entry wakeup and, on 2026-08-05, would have rung a
// full escalating alarm for an occurrence the user never set).
//
// The order below makes the classification TOTAL -- exactly one result applies
// to any combination of fields, including the inconsistent ones.
typedef enum {
  AC_CYCLE_NONE = 0,   // nothing live
  AC_CYCLE_SNOOZE,     // a snooze is in flight
  AC_CYCLE_RINGING,    // a ring cycle is live with no window and no pending snooze
  AC_CYCLE_WINDOW,     // a smart window is open, waiting for light sleep
  AC_CYCLE_ORPHAN,     // a live cycle whose owning slot is gone
} AcCycleState;

// Plain integers in (RunState's own fields), enum out: pure, so a host test
// reaches it without alarm_store.h. `now` decides only whether a snooze expiry
// is still ahead -- ac_snooze_pending is reused rather than re-derived, so this
// and sc_rearm cannot disagree about what "a snooze is pending" means.
AcCycleState ac_cycle_state(int8_t pending_slot, uint32_t window_started_at,
                            uint32_t ring_started_at, uint8_t snooze_count,
                            time_t now);

// Does a live cycle's STORED deadline belong to the occurrence in hand?
//
// The window/re-entry wakeup handler used to trust RunState.deadline_at whenever
// a window was live, on the reasoning that a live cycle owns a real deadline.
// It does -- but not necessarily the deadline of the occurrence the handler has
// just resolved. A one-time Test alarm pruned out from under its own cycle left
// window=13:52/deadline=14:22 behind on 2026-08-05; the next re-entry resolved
// the next unserved alarm (tomorrow 07:50), kept the 14:22 deadline, read
// `now >= ring` that afternoon and rang a full escalating alarm nobody set.
//
// True means: a window is live and its stored deadline is NOT this occurrence's,
// so the cycle is a leftover and must be ended before anything reads it.
bool ac_cycle_is_stale(uint32_t window_started_at, uint32_t deadline_at,
                       time_t occurrence_deadline);

// WHAT A WC_WINDOW / WC_REENTRY WAKEUP MUST DO.
//
// This is the whole decision the window/re-entry handler used to make inline in
// main.c, and it is here for one reason: EVERY defect found in that block --
// six of them across two rounds this week, including a phantom 14:22 alarm, a
// lost day's alarm, a ring for a deleted alarm and a 180 s wake loop -- lived in
// main.c, where nothing can reach it. The host assertions written alongside those
// fixes exercised the arithmetic the handler happens to call (ac_cycle_is_stale,
// ac_window_start, ...) and therefore passed identically against the buggy
// handler. Only the DECISION is worth asserting, so the decision is what is pure.
//
// main.c is left a thin executor: build the inputs, call this, end the cycle if
// asked, abandon a stranded waiting screen if asked, then act on `action`.
typedef enum {
  AC_WIN_REARM_ONLY = 0,  // nothing to open or ring: the caller re-arms and returns
  AC_WIN_RING_NOW,        // the hard deadline is due: the caller rings from the deadline
  AC_WIN_OPEN,            // open (or continue) the smart window
} AcWindowAction;

// WHY an AC_WIN_REARM_ONLY decision was made. Before this existed, all six
// distinct rearm-only paths logged identically (main.c printed only the
// decision, not the reasoning), which cost real debugging time -- every defect
// in this handler this week was found from pebble logs. One enum, set by the
// pure function that already does the reasoning, so the log can show WHY
// without main.c re-deriving or restating any of it.
typedef enum {
  AC_WIN_REASON_NONE = 0,       // the decision is to ring or to open: no reason needed
  AC_WIN_REASON_NO_SLOT,        // no alarm to act on at all
  AC_WIN_REASON_SERVED,         // this occurrence has already rung
  AC_WIN_REASON_NO_OCCURRENCE,  // no occurrence to restart a discarded cycle from
  AC_WIN_REASON_BASIS_PAST,     // the discarded cycle's own instant is behind now
  AC_WIN_REASON_SMART_OFF,      // the smart window is switched off
  AC_WIN_REASON_WINDOW_AHEAD,   // the window has not opened yet
} AcWindowReason;

typedef struct {
  AcWindowAction action;
  int    slot;            // the alarm the decision is about; -1 when there is none
  time_t window_start;    // AC_WIN_OPEN only
  time_t deadline;        // AC_WIN_OPEN and AC_WIN_RING_NOW
  bool   end_cycle;       // the caller must end the stored cycle before acting
  bool   abandon_screen;  // the caller must cancel the poll and close a stranded
                          // waiting screen (never set together with OPEN: that
                          // path re-pushes and re-captions the screen itself)
  AcWindowReason reason;  // WHY, for AC_WIN_REARM_ONLY; AC_WIN_REASON_NONE otherwise
  time_t basis;           // the instant the decision was derived from; 0 when there
                          // is none (a log could not otherwise show this at all)
} AcWindowDecision;

// `alarms`/`count` are the current alarm set, `now` the clock. The four config
// values are exactly what ac_ring_deadline/ac_window_start take, so this module
// stays free of SDK macros: `smart_window_active` must already fold in the
// setting, a zero window length and whether the platform has Health at all
// (scheduler.c's sc_window_active is the one owner of that), and `full_dev_s` is
// the effective escalation's full development time, read only by
// SEMANTICS_AWAKE_BY.
//
// pending_slot / window_started_at / ring_started_at / snooze_count / deadline_at
// are RunState's five cycle fields, and served_slot / served_at its served
// record. `pending_slot` is bounded against `count` HERE, so the caller does not
// have to (an index the phone has deleted out from under a live cycle is no
// owner at all -- see ac_cycle_state's AC_CYCLE_ORPHAN).
//
// The caller must NOT call this while an alarm is actually ringing: that state
// belongs to main.c (s_ringing cannot be derived from RunState) and means "do
// nothing at all", not "decide something".
AcWindowDecision ac_window_wakeup(const Alarm *alarms, int count, time_t now,
                                  uint8_t semantics, bool smart_window_active,
                                  uint16_t window_min, uint32_t full_dev_s,
                                  int8_t pending_slot, uint32_t window_started_at,
                                  uint32_t ring_started_at, uint8_t snooze_count,
                                  uint32_t deadline_at,
                                  int served_slot, time_t served_at);

#endif
