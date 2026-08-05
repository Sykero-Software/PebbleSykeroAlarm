// SPDX-License-Identifier: GPL-3.0-only
#include <stddef.h>
#include "alarm_calc.h"

// Convert tm_wday (0=Sunday) to our bit index (0=Monday).
static int prv_bit_for_wday(int tm_wday) {
  return (tm_wday + 6) % 7;
}

// The occurrence of minute_of_day on the local calendar day `day_offset` days
// after the local day containing `now`. Built from calendar fields so DST is
// handled by mktime.
static time_t prv_occurrence_on(time_t now, int day_offset, uint16_t minute_of_day) {
  struct tm tm = *localtime(&now);
  tm.tm_mday += day_offset;
  tm.tm_hour = minute_of_day / 60;
  tm.tm_min = minute_of_day % 60;
  tm.tm_sec = 0;
  tm.tm_isdst = -1;   // ask mktime to resolve DST for the target date
  time_t t = mktime(&tm);

  // Verify the round trip, because tm_isdst = -1 is NOT reliably honoured.
  //
  // Observed on the diorite emulator 2026-08-01: every occurrence came out exactly
  // one hour late -- an 08:30 alarm read "in 9 h" at 00:16, and a one-time 00:10
  // alarm that should have been 23 h away read "in 54 min" (i.e. 01:10), so it also
  // failed to fire. The same code over glibc computes correctly, so the algorithm is
  // right and it is the platform's localtime/mktime pair that disagrees: fields
  // produced by a summer-time localtime were re-interpreted by mktime as standard
  // time. EEST vs EET is exactly the one hour seen.
  //
  // So do not trust it: ask what wall-clock time `t` actually is, and if it is not
  // the one requested, shift by the difference. The correction is applied ONLY when
  // it demonstrably lands on the requested time, which makes this a provable no-op
  // on a platform that already round-trips correctly, and keeps it from oscillating
  // on a local time that genuinely does not exist (the spring-forward gap, where
  // mktime's normalization to the following hour is the correct answer and must be
  // left alone).
  struct tm back = *localtime(&t);
  const int want = (int)minute_of_day;
  const int got = back.tm_hour * 60 + back.tm_min;
  if (got != want) {
    const int diff = got - want;              // minutes the platform overshot by
    if (diff > -720 && diff < 720) {          // sub-half-day only
      const time_t adj = t - (time_t)diff * 60;
      struct tm chk = *localtime(&adj);
      if (chk.tm_hour * 60 + chk.tm_min == want) {
        t = adj;
      }
    }
  }
  return t;
}

// n-th (1-based) occurrence strictly after `now`.
static time_t prv_nth_occurrence(const Alarm *a, time_t now, int n) {
  int found = 0;
  // 15 days covers any weekly pattern twice over, which is enough for n <= 2.
  for (int off = 0; off <= 15; off++) {
    time_t cand = prv_occurrence_on(now, off, a->minute_of_day);
    if (cand <= now) {
      continue;
    }
    if (a->weekday_mask != 0) {
      struct tm tm = *localtime(&cand);
      if ((a->weekday_mask & (1 << prv_bit_for_wday(tm.tm_wday))) == 0) {
        continue;
      }
    }
    if (++found == n) {
      return cand;
    }
  }
  return 0;
}

time_t ac_next_occurrence(const Alarm *a, time_t now) {
  if (a == NULL || !a->enabled) {
    return 0;
  }
  return prv_nth_occurrence(a, now, a->skip_next ? 2 : 1);
}

int ac_next_alarm(const Alarm *alarms, int count, time_t now, time_t *out_when) {
  int best = -1;
  time_t best_when = 0;
  for (int i = 0; i < count; i++) {
    time_t w = ac_next_occurrence(&alarms[i], now);
    if (w == 0) {
      continue;
    }
    if (best < 0 || w < best_when) {
      best = i;
      best_when = w;
    }
  }
  if (best >= 0 && out_when != NULL) {
    *out_when = best_when;
  }
  return best;
}

time_t ac_ring_deadline(uint8_t semantics, bool smart_window_active,
                        uint16_t window_min, uint32_t full_dev_s,
                        time_t alarm_time) {
  if (semantics == SEMANTICS_AWAKE_BY) {
    // Independent of the window: the ramp has to START early enough to be at
    // full strength by the set time, window or no window.
    return alarm_time - (time_t)full_dev_s;
  }
  if (semantics == SEMANTICS_RING_FROM && smart_window_active) {
    // The set time is the EARLIEST, so the hard deadline is the far end of the
    // window -- and it is a real deadline: if no good moment is found in there,
    // the alarm still rings, just late rather than early.
    return alarm_time + (time_t)window_min * 60;
  }
  return alarm_time;
}

time_t ac_window_start(uint8_t semantics, bool smart_window_active,
                       uint16_t window_min, uint32_t full_dev_s,
                       time_t alarm_time) {
  time_t ring = ac_ring_deadline(semantics, smart_window_active, window_min,
                                 full_dev_s, alarm_time);
  if (!smart_window_active) {
    return ring;
  }
  if (semantics == SEMANTICS_RING_FROM) {
    return alarm_time;
  }
  return ring - (time_t)window_min * 60;
}

bool ac_is_served(time_t when, int slot, int served_slot, time_t served_at,
                  int32_t lead_s) {
  return served_slot >= 0 && slot == served_slot && when != 0
         && (when - (time_t)lead_s) <= served_at + AC_SERVED_TOLERANCE_S;
}

int ac_next_alarm_unserved(const Alarm *alarms, int count, time_t now,
                           int served_slot, time_t served_at, int32_t lead_s,
                           time_t *out_when) {
  time_t base = now;
  // One occurrence at most can be marked served, so this converges on the second
  // pass; the bound is a backstop against a future caller passing something that
  // does not advance rather than an expected number of iterations.
  for (int guard = 0; guard <= MAX_ALARMS; guard++) {
    time_t when = 0;
    int slot = ac_next_alarm(alarms, count, base, &when);
    if (slot < 0) {
      break;
    }
    if (!ac_is_served(when, slot, served_slot, served_at, lead_s)) {
      if (out_when != NULL) {
        *out_when = when;
      }
      return slot;
    }
    // This occurrence has already rung: look strictly past it. `when` is a real
    // occurrence instant, so the next pass cannot return it again.
    base = when;
  }
  if (out_when != NULL) {
    *out_when = 0;
  }
  return -1;
}

int ac_row_actions(const Alarm *a, AcAction *out, int max) {
  if (a == NULL || out == NULL || max < 1) {
    return 0;
  }
  int n = 0;
  if (!a->enabled) {
    out[n++] = AC_ACTION_TURN_ON;
    return n;
  }
  // A one-time alarm has exactly one occurrence, so "skip the next one" would
  // silently move it a day (prv_nth_occurrence(n=2) on a mask-0 alarm returns the
  // same time tomorrow). Turning it off is the honest action.
  if (a->weekday_mask != 0 && max >= 2) {
    out[n++] = a->skip_next ? AC_ACTION_RING_NEXT : AC_ACTION_SKIP_NEXT;
  }
  if (n < max) {
    out[n++] = AC_ACTION_TURN_OFF;
  }
  return n;
}

int ac_prune_spent_one_time(Alarm *alarms, int count, bool *missed,
                           int8_t *pending_slot, int8_t *served_slot) {
  if (alarms == NULL || count <= 0) {
    return count > 0 ? count : 0;
  }
  int map[MAX_ALARMS];
  int w = 0;
  for (int r = 0; r < count && r < MAX_ALARMS; r++) {
    if (alarms[r].weekday_mask == 0 && !alarms[r].enabled) {
      map[r] = -1;
      continue;
    }
    map[r] = w;
    alarms[w] = alarms[r];
    if (missed != NULL) {
      missed[w] = missed[r];
    }
    w++;
  }
  if (w == count) {
    return count;   // nothing spent: leave every index exactly as it was
  }
  for (int i = w; i < MAX_ALARMS; i++) {
    alarms[i] = (Alarm){0};
    if (missed != NULL) {
      missed[i] = false;
    }
  }
  if (pending_slot != NULL && *pending_slot >= 0) {
    *pending_slot = (*pending_slot < count) ? (int8_t)map[*pending_slot] : -1;
  }
  if (served_slot != NULL && *served_slot >= 0) {
    *served_slot = (*served_slot < count) ? (int8_t)map[*served_slot] : -1;
  }
  return w;
}

// Read exactly `digits` ASCII digits into *out. Returns the position after them,
// or NULL if any character is not a digit.
static const char *prv_read_uint(const char *p, int digits, uint16_t *out) {
  uint16_t v = 0;
  for (int i = 0; i < digits; i++) {
    if (p[i] < '0' || p[i] > '9') {
      return NULL;
    }
    v = (uint16_t)(v * 10 + (p[i] - '0'));
  }
  *out = v;
  return p + digits;
}

int ac_parse_set(const char *s, Alarm *out, int max) {
  int n = 0;
  if (s == NULL || out == NULL) {
    return 0;
  }
  const char *p = s;
  while (*p != '\0' && n < max) {
    Alarm a = {0};
    a.enabled = true;
    if (*p == '-') {
      a.enabled = false;
      p++;
    }
    uint16_t hh = 0, mm = 0;
    const char *q = prv_read_uint(p, 2, &hh);
    if (q == NULL || *q != ':') {
      goto skip_slot;
    }
    q = prv_read_uint(q + 1, 2, &mm);
    if (q == NULL || *q != '|' || hh > 23 || mm > 59) {
      goto skip_slot;
    }
    q++;
    for (int i = 0; i < 7; i++) {
      if (q[i] != '0' && q[i] != '1') {
        goto skip_slot;
      }
      if (q[i] == '1') {
        a.weekday_mask |= (uint8_t)(1 << i);
      }
    }
    q += 7;
    a.minute_of_day = (uint16_t)(hh * 60 + mm);
    out[n++] = a;
    p = q;
    if (*p == ';') {
      p++;
    }
    continue;

  skip_slot:
    while (*p != '\0' && *p != ';') {
      p++;
    }
    if (*p == ';') {
      p++;
    }
  }
  return n;
}

// Hand-rolled, not strcmp: the safe-to-call libc surface on the Core Devices
// firmware is narrow (memcpy/memset/strlen/snprintf are fine, the str*to*
// converters hard-fault), and a four-line comparison needs no such judgement
// call. Both strings are NUL-terminated by their callers.
static bool prv_str_eq(const char *a, const char *b) {
  if (a == NULL || b == NULL) {
    return false;
  }
  while (*a != '\0' && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

bool ac_apply_set_if_changed(const char *incoming, const char *last_applied,
                             Alarm *out, int *count, int max) {
  if (incoming == NULL || out == NULL || count == NULL) {
    return false;
  }
  if (last_applied != NULL && last_applied[0] != '\0'
      && prv_str_eq(incoming, last_applied)) {
    return false;   // a true no-op: out[] and *count are left exactly as they were
  }
  *count = ac_parse_set(incoming, out, max);
  return true;
}

bool ac_snooze_pending(uint8_t snooze_count, uint32_t ring_started_at, time_t now) {
  return snooze_count > 0 && ring_started_at != 0 && (time_t)ring_started_at > now;
}

AcWakeDecision ac_dispatch_wakeup(const Alarm *alarms, int count, time_t now,
                                  int pending_slot, uint32_t ring_started_at,
                                  uint8_t snooze_count,
                                  int served_slot, time_t served_at, int32_t lead_s) {
  AcWakeDecision d = { .action = AC_WAKE_NONE, .slot = -1 };
  const time_t tol = (time_t)AC_SERVED_TOLERANCE_S;

  // A cycle only counts as live if the slot it names still exists: the phone can
  // delete alarms while a wakeup is in flight, and a stale index must never come
  // back as a slot to ring.
  bool cycle_live = (alarms != NULL && pending_slot >= 0 && pending_slot < count);

  // 1. A DUE DEADLINE, first -- this is what makes a second alarm's deadline beat
  //    a pending snooze, including when they coincide.
  //
  //    The search base is shifted by lead_s, not just by -tol: we want
  //    occurrences whose DEADLINE (when - lead_s) lands in [now - tol, now +
  //    tol], and ac_next_alarm_unserved only searches forward of the base it is
  //    given, so the occurrence window is the deadline window shifted by
  //    lead_s. Without the shift, SEMANTICS_RING_FROM (lead_s negative, the
  //    deadline sits AFTER the raw occurrence by up to a 60-minute window) would
  //    search forward from a point already well past today's occurrence and
  //    land on tomorrow's -- a due alarm that has never rung reported as
  //    "nothing due". The shift keeps the lower bound intact for every sign of
  //    lead_s: from when > now - tol + lead_s it follows that when - lead_s >
  //    now - tol, so a deadline that passed long ago still cannot be picked up.
  //    lead_s == 0 (RING_LATEST) reduces this to the original `now - tol`.
  if (alarms != NULL && count > 0) {
    time_t when = 0;
    int slot = ac_next_alarm_unserved(alarms, count, now - tol + (time_t)lead_s,
                                      served_slot, served_at, lead_s, &when);
    if (slot >= 0 && when != 0 && (when - (time_t)lead_s) <= now + tol) {
      d.action = AC_WAKE_RING_DEADLINE;
      d.slot = slot;
      return d;
    }
  }

  // 2. THIS CYCLE'S OWN RING IS DUE, OR ALREADY RUNNING. One test covers both,
  //    because ring_started_at is the same field either way: a snooze whose
  //    expiry has arrived (it holds the expiry), and a mid-ring keep-alive
  //    arriving after an eviction (it holds the ring start, already in the past).
  //    Both must RESUME rather than start fresh, so the escalation ramp
  //    continues instead of restarting at its gentlest stage. A snooze still in
  //    the future fails this test and falls through to KEEP below -- which is
  //    the whole point: nothing here may end a snooze that has not expired.
  //    The tolerance absorbs an E_RANGE-shifted wakeup firing up to two minutes
  //    early.
  if (cycle_live && ring_started_at != 0 && (time_t)ring_started_at <= now + tol) {
    d.action = AC_WAKE_RESUME;
    d.slot = pending_slot;
    return d;
  }

  // 3. A live cycle with nothing due: leave it completely alone. This is what
  //    protects a pending snooze from being ended by a stray wakeup -- returning
  //    NONE here is exactly the bug (NONE ends the cycle).
  if (cycle_live) {
    d.action = AC_WAKE_KEEP;
    return d;
  }

  return d;   // AC_WAKE_NONE
}

AcCycleState ac_cycle_state(int8_t pending_slot, uint32_t window_started_at,
                            uint32_t ring_started_at, uint8_t snooze_count,
                            time_t now) {
  AcCycleState st = AC_CYCLE_NONE;
  if (ac_snooze_pending(snooze_count, ring_started_at, now)) {
    st = AC_CYCLE_SNOOZE;
  } else if (ring_started_at != 0) {
    // An expired snooze lands here too, and should: the keep-alive wakeup will
    // resume the RING, so a ring is what the user has to be able to cancel.
    st = AC_CYCLE_RINGING;
  } else if (window_started_at != 0) {
    st = AC_CYCLE_WINDOW;
  }
  if (st != AC_CYCLE_NONE && pending_slot < 0) {
    // No owning alarm: only ac_prune_spent_one_time can produce this (every
    // path that begins a cycle records a real slot), and it must be reported
    // as its own state so a caller can offer to clear it rather than trying to
    // describe an alarm that no longer exists.
    return AC_CYCLE_ORPHAN;
  }
  return st;
}

bool ac_cycle_is_stale(uint32_t window_started_at, uint32_t deadline_at,
                       time_t occurrence_deadline) {
  if (window_started_at == 0) {
    return false;   // no live window: there is no stored deadline to trust
  }
  return deadline_at == 0 || (time_t)deadline_at != occurrence_deadline;
}

// The decision itself, without the `abandon_screen` derivation (see the wrapper
// below): every path here can `return d` as soon as it knows the answer, which is
// how the branch structure of the handler this was extracted from is preserved
// one-for-one.
static AcWindowDecision prv_window_wakeup(
    const Alarm *alarms, int count, time_t now,
    uint8_t semantics, bool smart_window_active,
    uint16_t window_min, uint32_t full_dev_s,
    int8_t pending_slot, uint32_t window_started_at,
    uint32_t ring_started_at, uint8_t snooze_count, uint32_t deadline_at,
    int served_slot, time_t served_at) {
  AcWindowDecision d = { .action = AC_WIN_REARM_ONLY, .slot = -1,
                         .window_start = 0, .deadline = 0,
                         .end_cycle = false, .abandon_screen = false };
  if (alarms == NULL) {
    count = 0;
  }
  // How far before its occurrence a ring starts under this config -- the one
  // input ac_is_served needs. Derived from ac_ring_deadline rather than restated,
  // so this cannot disagree with sc_rearm about what "already rang" means; the
  // difference does not depend on WHICH occurrence it is asked about, so `now`
  // serves as the reference (same construction as sc_rearm's own lead_s).
  const int32_t lead_s = (int32_t)(now - ac_ring_deadline(semantics,
                                                          smart_window_active,
                                                          window_min, full_dev_s,
                                                          now));

  // pending_slot as the CYCLE CLASSIFIER must see it: an index that no longer
  // names an alarm is no owner at all, which is what makes ac_cycle_state report
  // AC_CYCLE_ORPHAN. The phone can delete slots while a cycle is live, and
  // deleting the WHOLE set while a window was open left a cycle nothing could
  // clear -- sc_rearm re-arms the rolling re-entry on window_started_at != 0
  // alone, so the watch woke the app every SC_REENTRY_GAP_S for ever.
  const int8_t owner = (pending_slot >= 0 && pending_slot < count)
                       ? pending_slot : (int8_t)-1;

  // WHICH ALARM IS THIS WINDOW WAKEUP FOR? pending_slot answers that ONLY while
  // its window is actually live -- i.e. only in the state this wakeup exists to
  // service. Reading it unconditionally let a cycle with NO live window hijack
  // another alarm's window opening: a force-quit mid-ring leaves ring_started_at
  // != 0, window_started_at == 0 and pending_slot on the OLD alarm, and the
  // launch re-arm then drops that cycle's keep-alive, so it stays live
  // indefinitely. The next day, alarm 1's own WC_WINDOW arrived and the handler
  // worked on alarm 0: the window landed in the future, it re-armed only -- and
  // sc_rearm will not re-place a WC_WINDOW whose start has passed, so alarm 1's
  // smart window never opened at all and it could only ring at its hard deadline.
  //
  // AC_CYCLE_WINDOW is exactly the condition: a live window, no ring and no
  // pending snooze in progress, and an owner that still names a real alarm.
  // Anything else falls through to ac_next_alarm below, which resolves whatever
  // is next NOW.
  //
  // A pending snooze stays protected: it reaches this as AC_CYCLE_SNOOZE, so the
  // next alarm is resolved instead and the window the caller then tries to open
  // is declined by open_smart_window's own snooze check (the one owner of that
  // rule).
  int slot = -1;
  // Did `slot` come from the live cycle itself, i.e. does it OWN that cycle?
  // Only then may the cycle's stored fields be interpreted through this alarm's
  // occurrence grid (see the staleness probe below).
  bool cycle_slot = false;
  if (ac_cycle_state(owner, window_started_at, ring_started_at, snooze_count,
                     now) == AC_CYCLE_WINDOW) {
    slot = owner;
    cycle_slot = true;
  }
  time_t when = 0;
  if (slot < 0) {
    slot = ac_next_alarm(alarms, count, now - 60, &when);
  } else {
    when = ac_next_occurrence(&alarms[slot], now - 60);
  }
  d.slot = slot;
  if (slot < 0) {
    // There is no alarm to resolve at all -- every slot is gone or disabled. A
    // live WINDOW cannot survive that: nothing will ever ring it, no screen can
    // describe it, and the rolling re-entry is re-armed on window_started_at != 0
    // alone, so leaving it standing wakes the app every SC_REENTRY_GAP_S for ever
    // (the state the phone leaves behind by deleting ALL alarms while a window is
    // open). Gated on window_started_at specifically, NOT on "any live cycle":
    // ring_started_at may hold a pending snooze, which is a promise already made
    // to the user and is ended by nothing here (start_ring zeroes
    // window_started_at, so a snooze never has a live window and can never reach
    // this).
    d.end_cycle = (window_started_at != 0);
    return d;
  }
  // NEVER re-open a window, or ring, for an occurrence that has ALREADY RUNG.
  // sc_rearm no longer arms one (it picks with ac_next_alarm_unserved), so this
  // is the second line of defence for the same reason the deadline guard below is
  // one -- a future path that forgets must not be able to resurrect the double
  // ring, which cost the user a 07:20 wake AND a 07:50 one on the same morning.
  // The stored cycle is deliberately left alone here: this wakeup was simply
  // consumed, and nothing about a served occurrence says the cycle is invalid.
  if (ac_is_served(when, slot, served_slot, served_at, lead_s)) {
    return d;
  }
  // The stored deadline is trusted ONLY when it is this occurrence's. Being
  // "live" is not enough: a cycle can outlive the alarm that owned it (a pruned
  // one-time alarm), and this would then apply that alarm's deadline to whatever
  // occurrence it just resolved -- which is exactly how a 13:52 test alarm armed
  // a 14:22 ring for a 07:50 alarm on 2026-08-05. A mismatch means the cycle is
  // a leftover, so it is ended before anything reads it, and the window is then
  // re-derived from the cycle's OWN occurrence (see `basis` below).
  //
  // THE CYCLE'S OWN occurrence, not the next one after now. Under
  // SEMANTICS_RING_FROM the window runs [T, T+w], so a re-entry INSIDE a live
  // window resolves `when` to tomorrow's occurrence (today's T is already in the
  // past) -- validating the stored deadline against that discarded the live
  // window and lost the day's alarm entirely. The stored window start is what
  // identifies the occurrence the cycle is about. Probed with enabled/skip_next
  // FORCED, so this asks about the alarm's grid, not about its current on/off
  // state (a cycle for an alarm the user disabled mid-window must keep its
  // previous behaviour, not be discarded by this guard).
  //
  // ONLY for the alarm that OWNS the cycle (cycle_slot). Probing a cycle's stored
  // window start against a DIFFERENT alarm's grid answers a question nobody
  // asked, and the answer is dangerous: it would let a foreign alarm's occurrence
  // become the basis below and, if the orphan's window start is old enough, ring
  // an occurrence that is already hours past. An unowned live cycle leaves
  // cycle_deadline 0, which ac_cycle_is_stale reports stale -- which is right,
  // and is what ends it.
  time_t cycle_deadline = 0;
  // ...and, from the SAME search base, the same occurrence as the alarm ACTUALLY
  // IS NOW -- enabled and skip_next honoured. That is what a restart may be
  // derived from (`basis` below); the forced probe above may not be. The force
  // exists only to answer "does the stored deadline fit this alarm's grid", and
  // using its answer to open a NEW window would ring an alarm the user has
  // switched off or deleted: a deleted alarm leaves a DISABLED one-time row in
  // its slot, whose minute_of_day is its own, so the forced probe happily returns
  // that row's next occurrence -- a ring nobody set, at a time nobody chose. 0
  // here (disabled, or a spent one-time) means there is nothing to re-open, which
  // is the correct outcome for exactly that case.
  time_t restart_when = 0;
  if (cycle_slot && window_started_at != 0) {
    const time_t base = (time_t)window_started_at - 1;
    Alarm probe = alarms[slot];
    probe.enabled = true;
    probe.skip_next = false;
    const time_t cycle_when = ac_next_occurrence(&probe, base);
    if (cycle_when != 0) {
      cycle_deadline = ac_ring_deadline(semantics, smart_window_active,
                                        window_min, full_dev_s, cycle_when);
    }
    restart_when = ac_next_occurrence(&alarms[slot], base);
  }
  const bool discarded = ac_cycle_is_stale(window_started_at, deadline_at,
                                           cycle_deadline);
  d.end_cycle = discarded;
  // A discarded cycle's stored fields are about to be zeroed by the caller, so
  // nothing below may read them: `live` folds the discard in rather than leaving
  // that to the reader (the handler this replaces relied on the ORDER of an
  // in-place runstate_end_cycle for the same effect).
  const bool live = !discarded && window_started_at != 0 && deadline_at != 0;

  // THE OCCURRENCE A FRESH WINDOW/RING IS DERIVED FROM.
  //
  // After a discard that is the cycle's OWN occurrence (restart_when), never
  // `when`. Two defects, one cause -- `when` is the wrong occurrence here:
  //
  //  * `when` is 0 for a disabled slot, and a ring instant derived from 0 is an
  //    epoch-era value, i.e. always <= now: deleting an alarm mid-window (which
  //    leaves a disabled one-time row in its slot) discarded the cycle and then
  //    started a full escalating alarm, immediately, for the alarm the user had
  //    just deleted.
  //  * under SEMANTICS_RING_FROM the live window is [T, T+w], so inside it `when`
  //    is TOMORROW's occurrence: after a mid-window config change the cycle was
  //    discarded and then re-armed for tomorrow only, so the alarm never rang
  //    today (sc_rearm's own ac_next_alarm_unserved skips today's past occurrence
  //    too). The spec says a mid-window config change restarts the cycle from the
  //    new config -- so the SAME occurrence must be re-opened, which is what
  //    restart_when is.
  const time_t basis = discarded ? restart_when : when;
  if (!live && basis == 0) {
    // There is no occurrence to derive anything from: the slot has no future
    // occurrence (it is disabled -- deleting an alarm on the phone leaves a
    // disabled one-time row behind -- or it is a spent one-time), or the
    // discarded cycle had no owning alarm at all. Nothing to do -- and above all
    // NOT a ring computed from 0, which would be an epoch-era instant and
    // therefore fire a full escalating alarm at once.
    return d;
  }
  const time_t ring = live
      ? (time_t)deadline_at
      : ac_ring_deadline(semantics, smart_window_active, window_min, full_dev_s,
                         basis);
  if (now >= ring) {
    // ON THE DISCARDED PATH, AN INSTANT ALREADY BEHIND `now` IS NOT A DEADLINE TO
    // RING -- IT IS A CYCLE THAT IS OVER.
    //
    // The restart basis is derived from `window_started_at - 1`, so unlike the
    // basis a non-discarded path uses (always > now - 60) it can be arbitrarily
    // far behind `now`. Default semantics, a 07:50 daily alarm whose window opened
    // at 07:20: at 07:40 the user edits that alarm on the phone to 07:30, meaning
    // TOMORROW. The next re-entry finds the cycle stale (this occurrence's
    // deadline is 07:30 now, not 07:50), ends it, re-derives from today 07:30 --
    // and `now >= ring` then started a full escalating alarm on the spot, for an
    // alarm time the user had just moved into the past. The same edit made one
    // minute BEFORE the window opened arms it for tomorrow, and an open window
    // must not change that.
    //
    // GATED ON THE BASIS, NOT ON THE DEADLINE, so SEMANTICS_AWAKE_BY keeps
    // working: there the deadline is the alarm time MINUS the escalation ramp, so
    // a deadline behind `now` with the occurrence still ahead means the ramp
    // genuinely must already be running, and ringing at once is correct.
    //
    // The RING_FROM re-open is untouched (it is the reason this is not simply
    // "never ring from a past basis"): that cycle's basis -- its window start -- is
    // behind `now` by design, but its deadline is basis + the window, still ahead,
    // so it never reaches this branch at all and falls through to AC_WIN_OPEN
    // below, which is how the day's alarm survives a mid-window config change.
    //
    // The refused occurrence is DROPPED, not re-armed. sc_rearm's priority-1 pick
    // (ac_next_alarm_unserved, search base `now`) only ever returns a FUTURE
    // occurrence, so what actually gets armed the moment the config changes is the
    // NEXT occurrence -- typically tomorrow's, not this one. Losing it is still the
    // right trade: this branch is reachable only from a phone-side config save or
    // alarm-set edit made WHILE AWAKE, because the other two paths that can
    // discard a cycle -- the nightly DST recheck and ac_prune_spent_one_time --
    // either leave the basis in the future or leave no occurrence at all (the
    // `!live && basis == 0` return above). The user editing an alarm at the phone
    // is what makes dropping the stale occurrence acceptable rather than a missed
    // wake-up. Ringing instead is what the old code did, and it rang a full
    // escalating alarm for an alarm time the user had just moved into the past --
    // exactly the case above (07:30 edited from 07:50, ten minutes after the fact).
    //
    // A deadline missed by more than the served tolerance does not ring on the
    // WC_DEADLINE path either -- ac_dispatch_wakeup requires it inside +/-
    // AC_SERVED_TOLERANCE_S -- so this is that same rule, applied to a path that
    // could otherwise reach back hours.
    //
    // Worth stating plainly: ac_ring_deadline never returns a ring EARLIER than
    // its basis under RING_LATEST or RING_FROM (ring == basis for RING_LATEST;
    // ring == basis + the window for RING_FROM), so `basis > now` and `now >=
    // ring` cannot hold together in those two modes -- AC_WIN_RING_NOW is
    // therefore UNREACHABLE on this discarded path under RING_LATEST/RING_FROM.
    // It is reachable only under SEMANTICS_AWAKE_BY, where ring = basis -
    // full_dev_s can sit behind `now` while the occurrence itself (basis) is still
    // ahead -- case 9b in the test file is exactly that.
    //
    // ONLY on the discarded path, and `discarded` is the test rather than `!live`:
    // a wakeup arriving with NO live cycle derives its basis from `now - 60`, so
    // its basis is at most a minute behind and a ring is then exactly right -- a
    // WC_WINDOW that E_RANGE shifted onto the alarm time, or a re-entry seconds
    // after it. Blocking that (which `!live` did in the first draft of this fix)
    // is a missed wake-up, and case 8's own control assertion caught it.
    if (!discarded || basis > now) {
      d.action = AC_WIN_RING_NOW;
      d.deadline = ring;
    }
    return d;
  }
  if (!smart_window_active) {
    return d;
  }
  const time_t win = live
      ? (time_t)window_started_at
      : ac_window_start(semantics, smart_window_active, window_min, full_dev_s,
                        basis);
  if (win > now) {
    // A stray re-entry (or a cycle just discarded above) with the next
    // occurrence's window still hours away. Opening it here would put a
    // multi-hour "window" on the poll timer, whose 1-minute evaluation could fire
    // on ordinary daytime movement. There is nothing to open: re-arm and let the
    // real WC_WINDOW wakeup do it.
    return d;
  }
  d.action = AC_WIN_OPEN;
  d.window_start = win;
  d.deadline = ring;
  return d;
}

AcWindowDecision ac_window_wakeup(const Alarm *alarms, int count, time_t now,
                                  uint8_t semantics, bool smart_window_active,
                                  uint16_t window_min, uint32_t full_dev_s,
                                  int8_t pending_slot, uint32_t window_started_at,
                                  uint32_t ring_started_at, uint8_t snooze_count,
                                  uint32_t deadline_at,
                                  int served_slot, time_t served_at) {
  AcWindowDecision d = prv_window_wakeup(alarms, count, now, semantics,
                                         smart_window_active, window_min,
                                         full_dev_s, pending_slot,
                                         window_started_at, ring_started_at,
                                         snooze_count, deadline_at, served_slot,
                                         served_at);
  // EVERY PATH THAT ENDS A CYCLE WITHOUT OPENING A WINDOW OR RINGING must also
  // abandon a stranded waiting screen. With window_started_at cleared, the poll
  // returns early for ever (its guard reads that field first), and the waiting
  // screen's BACK is deliberately a no-op (it must not be possible to cancel a
  // smart window by brushing a button in your sleep), so nothing would bring the
  // user back to the watchface -- and the screen keeps DESCRIBING the dead cycle
  // through localtime(0) as "Alarm 02:00 / Waiting for light sleep".
  //
  // Derived here, once, from the two fields that decide it, rather than being set
  // at each of the four returns that need it: the handler this replaces set it by
  // hand at five call sites and the one it missed is how the screen got stranded
  // in the first place. RING_NOW needs no cleanup (start_ring takes the screen
  // off the stack itself) and OPEN must NOT have any (the same window is being
  // re-opened under the new config, and open_smart_window re-pushes and
  // re-captions that very screen).
  d.abandon_screen = d.end_cycle && d.action == AC_WIN_REARM_ONLY;
  return d;
}
