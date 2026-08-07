// SPDX-License-Identifier: GPL-3.0-only
#define _POSIX_C_SOURCE 200809L
#include "alarm_calc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Build a local-time timestamp. mon is 1-12, year is full (2026).
static time_t at(int year, int mon, int day, int hour, int min) {
  struct tm tm = {0};
  tm.tm_year = year - 1900;
  tm.tm_mon = mon - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = min;
  tm.tm_isdst = -1;
  return mktime(&tm);
}

// One ac_window_wakeup scenario's inputs, so a case below reads as the STATE it
// describes rather than as fourteen positional arguments -- and so that adding an
// input later does not touch every case. Anything left out defaults to 0/NULL,
// which for the five RunState cycle fields means "no cycle"; `pending_slot` and
// `served_slot` are written out in every case, because 0 is a real slot there and
// -1 is the "none" both need.
typedef struct {
  const Alarm *alarms;
  int      count;
  time_t   now;
  uint8_t  semantics;
  bool     smart_window_active;
  uint16_t window_min;
  uint32_t full_dev_s;
  int8_t   pending_slot;
  time_t   window_started_at;
  time_t   ring_started_at;
  uint8_t  snooze_count;
  time_t   deadline_at;
  int      served_slot;
  time_t   served_at;
} WinCase;

static AcWindowDecision win_decide(const WinCase *c) {
  return ac_window_wakeup(c->alarms, c->count, c->now, c->semantics,
                          c->smart_window_active, c->window_min, c->full_dev_s,
                          c->pending_slot, (uint32_t)c->window_started_at,
                          (uint32_t)c->ring_started_at, c->snooze_count,
                          (uint32_t)c->deadline_at, c->served_slot, c->served_at);
}

static void show(const char *label, time_t t) {
  struct tm *tm = localtime(&t);
  printf("  %-28s %04d-%02d-%02d %02d:%02d\n", label,
         tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
}

int main(void) {
  setenv("TZ", "Europe/Helsinki", 1);
  tzset();

  // --- weekday repeat: Mon-Fri 07:00 ---
  Alarm wd = { .minute_of_day = 7 * 60, .weekday_mask = 0x1F, .enabled = true };

  // Wed 2026-07-01 06:00 -> same day 07:00
  assert(ac_next_occurrence(&wd, at(2026, 7, 1, 6, 0)) == at(2026, 7, 1, 7, 0));
  // Wed 07:00 exactly -> NEXT day (strictly after now)
  assert(ac_next_occurrence(&wd, at(2026, 7, 1, 7, 0)) == at(2026, 7, 2, 7, 0));
  // Fri 2026-07-03 08:00 -> skips the weekend to Mon 2026-07-06
  assert(ac_next_occurrence(&wd, at(2026, 7, 3, 8, 0)) == at(2026, 7, 6, 7, 0));

  // --- weekend alarm crossing a month boundary ---
  Alarm we = { .minute_of_day = 9 * 60 + 30, .weekday_mask = 0x60, .enabled = true };
  // Fri 2026-07-31 10:00 -> Sat 2026-08-01 09:30
  assert(ac_next_occurrence(&we, at(2026, 7, 31, 10, 0)) == at(2026, 8, 1, 9, 30));

  // --- year boundary ---
  Alarm daily = { .minute_of_day = 6 * 60, .weekday_mask = 0x7F, .enabled = true };
  assert(ac_next_occurrence(&daily, at(2026, 12, 31, 7, 0)) == at(2027, 1, 1, 6, 0));

  // --- The wall-clock invariant: whatever an occurrence resolves to, its LOCAL time
  // must BE the alarm's time. This is what the diorite emulator violated on
  // 2026-08-01, by exactly one hour, in every row -- an 08:30 alarm read "in 9 h" at
  // 00:16 and a one-time 00:10 alarm read "in 54 min" instead of "in 23 h", so it
  // never fired. The cause was tm_isdst = -1 not being honoured by the platform's
  // mktime, which prv_occurrence_on now verifies and corrects. Asserted here across a
  // whole year (both DST boundaries and every weekday pattern) rather than for one
  // date, because the failure was uniform and a single-date check happened to pass.
  {
    const uint16_t minutes[5] = { 0, 10, 7 * 60, 8 * 60 + 30, 23 * 60 + 59 };
    const uint8_t masks[4] = { 0x00, 0x1F, 0x60, 0x7F };   // once, Mon-Fri, weekend, daily
    int checked = 0;
    for (int mi = 0; mi < 5; mi++) {
      for (int wi = 0; wi < 4; wi++) {
        Alarm a = { .minute_of_day = minutes[mi], .weekday_mask = masks[wi],
                    .enabled = true, .skip_next = false };
        for (int day = 0; day < 365; day += 7) {
          time_t from = at(2026, 8, 1, 12, 0) + (time_t)day * 86400;
          time_t next = ac_next_occurrence(&a, from);
          assert(next > from);
          struct tm *tm = localtime(&next);
          assert(tm->tm_hour * 60 + tm->tm_min == (int)a.minute_of_day);
          if (a.weekday_mask != 0) {
            assert((a.weekday_mask & (1 << ((tm->tm_wday + 6) % 7))) != 0);
          }
          checked++;
        }
      }
    }
    printf("  wall-clock invariant held for %d occurrences\n", checked);
    assert(checked == 5 * 4 * 53);
  }

  // --- DST: Europe/Helsinki springs forward 2027-03-28 (between 02:00 and 03:00).
  // A 07:00 alarm must stay at wall-clock 07:00, i.e. 22h after Sat 08:00,
  // not 24h. Adding 86400 naively would land at 08:00.
  {
    Alarm seven = { .minute_of_day = 7 * 60, .weekday_mask = 0x7F, .enabled = true };
    time_t from = at(2027, 3, 27, 8, 0);            // Sat, after that day's alarm
    time_t next = ac_next_occurrence(&seven, from); // must be Sun 07:00 local
    struct tm *tm = localtime(&next);
    assert(tm->tm_hour == 7 && tm->tm_min == 0 && tm->tm_mday == 28);
    assert(next - from == 22 * 3600);               // 22h, because an hour vanished
    show("DST spring-forward next", next);
  }
  // Autumn: 2027-10-31 (between 03:00 and 04:00), so the gap is 24h.
  {
    Alarm seven = { .minute_of_day = 7 * 60, .weekday_mask = 0x7F, .enabled = true };
    time_t from = at(2027, 10, 30, 8, 0);
    time_t next = ac_next_occurrence(&seven, from);
    struct tm *tm = localtime(&next);
    assert(tm->tm_hour == 7 && tm->tm_min == 0 && tm->tm_mday == 31);
    assert(next - from == 24 * 3600);
    show("DST fall-back next", next);
  }

  // --- one-time alarm (mask 0): fires once, then never ---
  Alarm once = { .minute_of_day = 5 * 60, .weekday_mask = 0x00, .enabled = true };
  assert(ac_next_occurrence(&once, at(2026, 7, 1, 4, 0)) == at(2026, 7, 1, 5, 0));
  // already past today -> tomorrow (a one-time alarm is "the next time this clock time comes")
  assert(ac_next_occurrence(&once, at(2026, 7, 1, 6, 0)) == at(2026, 7, 2, 5, 0));

  // --- disabled ---
  Alarm off = { .minute_of_day = 7 * 60, .weekday_mask = 0x7F, .enabled = false };
  assert(ac_next_occurrence(&off, at(2026, 7, 1, 6, 0)) == 0);

  // --- skip_next: returns the occurrence AFTER the next one ---
  Alarm skip = { .minute_of_day = 7 * 60, .weekday_mask = 0x1F,
                 .enabled = true, .skip_next = true };
  assert(ac_next_occurrence(&skip, at(2026, 7, 1, 6, 0)) == at(2026, 7, 2, 7, 0));
  // skip on a Friday jumps over Monday to Tuesday
  Alarm skip_fri = { .minute_of_day = 7 * 60, .weekday_mask = 0x1F,
                     .enabled = true, .skip_next = true };
  assert(ac_next_occurrence(&skip_fri, at(2026, 7, 3, 8, 0)) == at(2026, 7, 7, 7, 0));

  // --- ac_next_alarm picks the soonest enabled alarm ---
  Alarm set[3] = {
    { .minute_of_day = 9 * 60, .weekday_mask = 0x7F, .enabled = true },
    { .minute_of_day = 7 * 60, .weekday_mask = 0x7F, .enabled = true },
    { .minute_of_day = 6 * 60, .weekday_mask = 0x7F, .enabled = false },
  };
  time_t when = 0;
  assert(ac_next_alarm(set, 3, at(2026, 7, 1, 5, 0), &when) == 1);
  assert(when == at(2026, 7, 1, 7, 0));
  // all disabled -> -1
  for (int i = 0; i < 3; i++) set[i].enabled = false;
  assert(ac_next_alarm(set, 3, at(2026, 7, 1, 5, 0), &when) == -1);

  // --- ac_parse_set ---
  Alarm parsed[MAX_ALARMS];
  int n = ac_parse_set("07:00|1111100;08:30|0000011", parsed, MAX_ALARMS);
  assert(n == 2);
  assert(parsed[0].minute_of_day == 7 * 60 && parsed[0].weekday_mask == 0x1F);
  assert(parsed[0].enabled == true);
  assert(parsed[1].minute_of_day == 8 * 60 + 30 && parsed[1].weekday_mask == 0x60);
  // a leading '-' marks a disabled slot; the time is still remembered
  n = ac_parse_set("-07:00|1111100", parsed, MAX_ALARMS);
  assert(n == 1 && parsed[0].enabled == false && parsed[0].minute_of_day == 420);
  // empty string, and garbage, yield zero alarms rather than crashing
  assert(ac_parse_set("", parsed, MAX_ALARMS) == 0);
  assert(ac_parse_set(";;", parsed, MAX_ALARMS) == 0);
  assert(ac_parse_set("nonsense", parsed, MAX_ALARMS) == 0);
  // more than max slots are truncated, not overflowed
  n = ac_parse_set("01:00|1111111;02:00|1111111;03:00|1111111", parsed, 2);
  assert(n == 2);
  // one-time slot: an all-zero weekday field
  n = ac_parse_set("05:15|0000000", parsed, MAX_ALARMS);
  assert(n == 1 && parsed[0].weekday_mask == 0x00 && parsed[0].minute_of_day == 315);
  // out-of-range fields are rejected, not wrapped
  assert(ac_parse_set("25:00|1111111", parsed, MAX_ALARMS) == 0);
  assert(ac_parse_set("07:75|1111111", parsed, MAX_ALARMS) == 0);

  // --- A CONFIG RESEND THAT CHANGES NOTHING MUST NOT CHANGE ANYTHING.
  //
  // The whole-branch re-review's Critical. `enabled` and `skip_next` are
  // watch-mutable by design (SELECT toggles an alarm on/off, long SELECT skips its
  // next occurrence, ring_stop_now disables a fired one-time alarm) and are never
  // sent back to the phone, but ac_parse_set rebuilds every slot from `{0}` --
  // `enabled` from the phone's '-' bit, `skip_next` always false. That was
  // harmless while the phone's dict only arrived on an explicit Save; once the
  // watch asks for its config on every launch it is not, because dst_check (on by
  // default) launches the app around 03:00 EVERY NIGHT: disable tomorrow's alarm
  // from the wrist at 22:00, and an unconditional re-parse re-enables it and the
  // alarm rings at 07:00 anyway.
  {
    Alarm a[MAX_ALARMS];
    int   cnt = 0;
    const char *set = "07:00|1111100;08:30|0000011";

    // nothing recorded yet ("" and NULL both mean "always apply")
    assert(ac_apply_set_if_changed(set, "", a, &cnt, MAX_ALARMS));
    assert(cnt == 2 && a[0].enabled && a[1].enabled);
    assert(ac_apply_set_if_changed(set, NULL, a, &cnt, MAX_ALARMS));
    assert(cnt == 2);

    // the user now acts ON THE WATCH: slot 0 switched off, slot 1's next
    // occurrence skipped
    a[0].enabled = false;
    a[1].skip_next = true;

    // ... and the phone resends the very same string (every launch, ~03:00 nightly)
    assert(!ac_apply_set_if_changed(set, set, a, &cnt, MAX_ALARMS));
    assert(cnt == 2);
    assert(a[0].enabled == false);      // still off -- the alarm must NOT ring
    assert(a[1].skip_next == true);     // still skipped
    assert(a[0].minute_of_day == 7 * 60);
    assert(a[1].minute_of_day == 8 * 60 + 30);
    // byte-identical, not merely equivalent: a separately-built copy of the same
    // string is still a no-op (the comparison is by value, not by pointer)
    char same[64];
    snprintf(same, sizeof(same), "%s", set);
    assert(!ac_apply_set_if_changed(same, set, a, &cnt, MAX_ALARMS));
    assert(a[0].enabled == false && a[1].skip_next == true);

    // A GENUINE phone-side change still takes full precedence -- including a
    // phone-side DISABLE, which is exactly why this compares the string instead of
    // merging per slot (a merge could never tell a phone-side disable from the
    // watch-side one above, so it would be undeliverable).
    const char *set_disabled = "-07:00|1111100;08:30|0000011";
    assert(ac_apply_set_if_changed(set_disabled, set, a, &cnt, MAX_ALARMS));
    assert(cnt == 2);
    assert(a[0].enabled == false);      // the phone's disable
    assert(a[1].enabled == true);
    assert(a[1].skip_next == false);    // reset by a real change, as designed

    // ... and a phone-side ENABLE overrides a watch-side disable, same way
    a[0].enabled = false;
    assert(ac_apply_set_if_changed(set, set_disabled, a, &cnt, MAX_ALARMS));
    assert(a[0].enabled == true);

    // a changed slot COUNT is a change like any other
    assert(ac_apply_set_if_changed("07:00|1111100", set, a, &cnt, MAX_ALARMS));
    assert(cnt == 1);

    // defensive: a NULL incoming string is never treated as a change
    cnt = 1;
    assert(!ac_apply_set_if_changed(NULL, set, a, &cnt, MAX_ALARMS));
    assert(cnt == 1);
  }

  // --- ac_next_alarm_unserved: an occurrence that has already RUNG must never
  // be armed a second time.
  //
  // The defect this exists for, reported from the wrist 2026-08-01: a 07:50
  // alarm whose smart window opened at 07:20 rang early, was dismissed at
  // 07:20 -- and then rang AGAIN at 07:50, with no snooze involved. Ending the
  // ring re-arms from time(NULL), and "the next occurrence strictly after
  // 07:20" is that same 07:50. The deadline path never showed it (a ring that
  // starts AT the alarm time ends after it, so the next occurrence is already
  // tomorrow's), and the emulator cannot fire an early smart wake at all.
  {
    Alarm daily = { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x7F,
                    .enabled = true };
    time_t when = 0;
    const time_t today = at(2026, 8, 1, 7, 50);
    const time_t tomorrow = at(2026, 8, 2, 7, 50);
    const time_t early = at(2026, 8, 1, 7, 21);   // just after the early ring

    // Nothing served yet (served_slot < 0) behaves exactly like ac_next_alarm.
    assert(ac_next_alarm_unserved(&daily, 1, early, -1, 0, 0, &when) == 0);
    assert(when == today);

    // Served: slot 0's 07:50 ring already happened, so the next thing to arm is
    // TOMORROW's occurrence -- this is the whole bug.
    assert(ac_next_alarm_unserved(&daily, 1, early, 0, today, 0, &when) == 0);
    assert(when == tomorrow);

    // ... and once that occurrence is in the past the stored served_at is inert,
    // so no clearing pass is needed anywhere.
    assert(ac_next_alarm_unserved(&daily, 1, at(2026, 8, 1, 8, 0), 0, today, 0,
                                  &when) == 0);
    assert(when == tomorrow);

    // A DIFFERENT slot at an earlier time must NOT be swallowed by the skip:
    // matching on the slot as well as the instant is what keeps a 07:30 alarm
    // ringing after the 07:50 one rang early.
    {
      Alarm two[2] = {
        { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x7F, .enabled = true },
        { .minute_of_day = 7 * 60 + 30, .weekday_mask = 0x7F, .enabled = true },
      };
      // now 07:21, slot 0's 07:50 served -> slot 1's 07:30 today still wins.
      assert(ac_next_alarm_unserved(two, 2, early, 0, today, 0, &when) == 1);
      assert(when == at(2026, 8, 1, 7, 30));
    }

    // "Awake by" semantics: served_at is the RING instant, which sits lead_s
    // before the occurrence. The same occurrence must still be recognised as
    // served, so the comparison is made in ring-instant terms rather than
    // occurrence terms.
    const int32_t lead_s = 6 * 60;                       // esc_full_development_s
    const time_t served_ring = today - lead_s;           // 07:44
    assert(ac_next_alarm_unserved(&daily, 1, at(2026, 8, 1, 7, 30), 0,
                                  served_ring, lead_s, &when) == 0);
    assert(when == tomorrow);

    // A ring that STARTED EARLY because sc_schedule shifted its wakeup (E_RANGE
    // moves a wakeup by up to 2 minutes) records that earlier instant as served.
    // Today's 07:50 must still count as rung, or the deadline path double-rings
    // too -- rarer than the smart-window case, same defect.
    assert(ac_next_alarm_unserved(&daily, 1, at(2026, 8, 1, 7, 49), 0,
                                  at(2026, 8, 1, 7, 48), 0, &when) == 0);
    assert(when == tomorrow);

    // The tolerance must not reach a whole occurrence: yesterday's ring has no
    // hold over the next one, a day later.
    assert(ac_next_alarm_unserved(&daily, 1, at(2026, 8, 1, 8, 0), 0,
                                  at(2026, 7, 31, 7, 50), 0, &when) == 0);
    assert(when == tomorrow);

    // A one-time alarm that has been disabled by ringing still reports "none".
    {
      Alarm once = { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0,
                     .enabled = false };
      assert(ac_next_alarm_unserved(&once, 1, early, 0, today, 0, &when) == -1);
      assert(when == 0);
    }
  }

  // --- ac_ring_deadline / ac_window_start: what the set time MEANS.
  //
  // Three modes, because "Ringing starts then" was read by a real user as "not
  // before then" when it means "not after then" (2026-08-01). The mapping decides
  // when an alarm actually goes off, so every mode is pinned here.
  {
    const time_t T = at(2026, 8, 1, 7, 50);
    const uint16_t w = 30;
    const uint32_t dev = 360;   // esc_full_development_s, Normal profile

    // RING_LATEST: T is the LATEST. Window [T-30, T].
    assert(ac_ring_deadline(SEMANTICS_RING_LATEST, true, w, dev, T) == T);
    assert(ac_window_start(SEMANTICS_RING_LATEST, true, w, dev, T)
           == at(2026, 8, 1, 7, 20));

    // RING_FROM: T is the EARLIEST. Window [T, T+30], and the deadline is the far
    // end -- the alarm still rings if no good moment is found, just late.
    assert(ac_ring_deadline(SEMANTICS_RING_FROM, true, w, dev, T)
           == at(2026, 8, 1, 8, 20));
    assert(ac_window_start(SEMANTICS_RING_FROM, true, w, dev, T) == T);

    // AWAKE_BY: the ramp must be fully developed by T, so it starts before it --
    // and that is true whether or not the smart window is active.
    assert(ac_ring_deadline(SEMANTICS_AWAKE_BY, true, w, dev, T) == T - (time_t)dev);
    assert(ac_ring_deadline(SEMANTICS_AWAKE_BY, false, w, dev, T) == T - (time_t)dev);
    assert(ac_window_start(SEMANTICS_AWAKE_BY, true, w, dev, T)
           == T - (time_t)dev - (time_t)w * 60);

    // With the window inactive (setting off, zero length, or no Health on the
    // platform) every non-awake-by mode collapses to exactly the set time: "smart
    // alarm off" must mean "rings when you said", under all of them.
    assert(ac_ring_deadline(SEMANTICS_RING_LATEST, false, w, dev, T) == T);
    assert(ac_window_start(SEMANTICS_RING_LATEST, false, w, dev, T) == T);
    assert(ac_ring_deadline(SEMANTICS_RING_FROM, false, w, dev, T) == T);
    assert(ac_window_start(SEMANTICS_RING_FROM, false, w, dev, T) == T);

    // The window never opens after the deadline it belongs to -- sc_rearm only
    // arms WC_WINDOW when win < ring, so a mode that broke this would silently
    // lose its window rather than fail loudly.
    for (int m = 0; m <= 2; m++) {
      assert(ac_window_start((uint8_t)m, true, w, dev, T)
             <= ac_ring_deadline((uint8_t)m, true, w, dev, T));
    }

    // The lead time sc_rearm derives (now - ring_deadline(now)) must round-trip
    // through ac_is_served for every mode, including the NEGATIVE lead RING_FROM
    // produces -- that is what keeps the double-ring fix working there.
    {
      Alarm daily = { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x7F,
                      .enabled = true };
      time_t when = 0;
      const time_t ref = at(2026, 8, 1, 7, 0);
      for (int m = 0; m <= 2; m++) {
        int32_t lead_s = (int32_t)(ref - ac_ring_deadline((uint8_t)m, true, w, dev, ref));
        time_t served = ac_ring_deadline((uint8_t)m, true, w, dev, T);
        assert(ac_next_alarm_unserved(&daily, 1, ref, 0, served, lead_s, &when) == 0);
        assert(when == at(2026, 8, 2, 7, 50));   // today's has rung: tomorrow's
      }
    }
  }

  // --- ac_prune_spent_one_time: a switched-off one-time alarm is a dead row that
  // NOTHING could delete (the watch has no delete, and the phone never knew about
  // a watch-created Test alarm). Two of them were stuck on the real watch.
  {
    Alarm a4[MAX_ALARMS] = {
      { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x7F, .enabled = true },
      { .minute_of_day = 0,           .weekday_mask = 0,    .enabled = false },  // spent
      { .minute_of_day = 11,          .weekday_mask = 0,    .enabled = false },  // spent
      { .minute_of_day = 8 * 60 + 30, .weekday_mask = 0x60, .enabled = true },
    };
    bool missed[MAX_ALARMS] = { false, true, false, true };
    int8_t pending = 3, served = 3;
    assert(ac_prune_spent_one_time(a4, 4, missed, &pending, &served) == 2);
    assert(a4[0].minute_of_day == 7 * 60 + 50);
    assert(a4[1].minute_of_day == 8 * 60 + 30);
    // missed[] is indexed by slot, so it must move with them -- slot 3's marker
    // belongs to the 08:30 alarm, now at slot 1.
    assert(missed[0] == false && missed[1] == true);
    assert(missed[2] == false && missed[3] == false);
    // ... and so must the two RunState slot references, or one alarm's state
    // silently becomes another's.
    assert(pending == 1 && served == 1);

    // A reference to a slot that was itself dropped becomes "none": keeping the
    // number would point it at whichever alarm slid into that index.
    Alarm a2[MAX_ALARMS] = {
      { .minute_of_day = 30, .weekday_mask = 0, .enabled = false },   // spent
      { .minute_of_day = 7 * 60, .weekday_mask = 0x1F, .enabled = true },
    };
    int8_t p2 = 0, s2 = 0;
    assert(ac_prune_spent_one_time(a2, 2, NULL, &p2, &s2) == 1);
    assert(a2[0].minute_of_day == 7 * 60);
    assert(p2 == -1 && s2 == -1);

    // A one-time alarm that is still ENABLED has a future and must survive -- this
    // is a pending Test alarm, and pruning it would delete the alarm the user just
    // asked for. Nothing may move when there is nothing spent.
    Alarm keep[MAX_ALARMS] = {
      { .minute_of_day = 7 * 60, .weekday_mask = 0x1F, .enabled = true },
      { .minute_of_day = 61, .weekday_mask = 0, .enabled = true },
    };
    int8_t p3 = 1, s3 = 1;
    assert(ac_prune_spent_one_time(keep, 2, NULL, &p3, &s3) == 2);
    assert(keep[1].minute_of_day == 61);
    assert(p3 == 1 && s3 == 1);

    // A DISABLED REPEATING alarm is not spent -- it is switched off, and switching
    // it back on must still be possible.
    Alarm off[MAX_ALARMS] = {
      { .minute_of_day = 7 * 60, .weekday_mask = 0x1F, .enabled = false },
    };
    assert(ac_prune_spent_one_time(off, 1, NULL, NULL, NULL) == 1);

    // Degenerate inputs must not crash or invent alarms.
    assert(ac_prune_spent_one_time(NULL, 3, NULL, NULL, NULL) == 3);
    assert(ac_prune_spent_one_time(a2, 0, NULL, NULL, NULL) == 0);
  }

  // --- ac_row_actions: what the alarm-row submenu offers. Pure, because a menu
  // that offers the wrong action is invisible until someone presses it.
  {
    AcAction act[AC_MAX_ACTIONS];
    Alarm rep = { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x7F,
                  .enabled = true, .skip_next = false };

    // On, not skipping: skip the next one, or turn it off.
    assert(ac_row_actions(&rep, act, AC_MAX_ACTIONS) == 2);
    assert(act[0] == AC_ACTION_SKIP_NEXT && act[1] == AC_ACTION_TURN_OFF);

    // On, already skipping: the first row UNDOES the skip. Offering "skip" again
    // would be a no-op the user could not tell from a broken button.
    rep.skip_next = true;
    assert(ac_row_actions(&rep, act, AC_MAX_ACTIONS) == 2);
    assert(act[0] == AC_ACTION_RING_NEXT && act[1] == AC_ACTION_TURN_OFF);

    // Off: one row, and NOT a skip -- a disabled alarm has nothing to skip.
    rep.enabled = false;
    assert(ac_row_actions(&rep, act, AC_MAX_ACTIONS) == 1);
    assert(act[0] == AC_ACTION_TURN_ON);

    // A ONE-TIME alarm gets no skip row at all. skip_next on a mask-0 alarm makes
    // prv_nth_occurrence return the SECOND occurrence of that time of day, i.e. it
    // silently moves the alarm to tomorrow -- which is not what "skip the next
    // one" means for an alarm that only has one. Turning it off is the honest
    // action, and it is what the user wanted anyway.
    Alarm once = { .minute_of_day = 6 * 60, .weekday_mask = 0, .enabled = true,
                   .skip_next = false };
    assert(ac_row_actions(&once, act, AC_MAX_ACTIONS) == 1);
    assert(act[0] == AC_ACTION_TURN_OFF);

    // Defensive: no room, or no alarm, writes nothing and claims nothing.
    assert(ac_row_actions(&rep, act, 0) == 0);
    assert(ac_row_actions(NULL, act, AC_MAX_ACTIONS) == 0);
    assert(ac_row_actions(&rep, NULL, AC_MAX_ACTIONS) == 0);
  }

  // --- ac_snooze_pending: ring_started_at holds the snooze EXPIRY in flight ---
  {
    time_t base = at(2026, 8, 5, 7, 10);
    // Expiry five minutes out: pending.
    assert(ac_snooze_pending(1, (uint32_t)(base + 300), base));
    // Expiry already passed -- the wakeup fired and the ring is running again.
    assert(!ac_snooze_pending(1, (uint32_t)(base - 1), base));
    // Exactly now is not "in the future": the expiry has arrived.
    assert(!ac_snooze_pending(1, (uint32_t)base, base));
    // Never snoozed: the same field then means the RING START, not an expiry,
    // and a future ring start would otherwise read as a pending snooze.
    assert(!ac_snooze_pending(0, (uint32_t)(base + 300), base));
    // No stamp at all.
    assert(!ac_snooze_pending(2, 0, base));
  }

  // --- ac_dispatch_wakeup: which alarm is a deadline/snooze wakeup FOR? ---
  {
    // A at 07:00 and B at 07:05, both Mon-Fri. lead_s = 0 (RING_LATEST, the
    // default: the deadline IS the alarm time).
    Alarm ab[2] = {
      { .minute_of_day = 7 * 60,      .weekday_mask = 0x1F, .enabled = true },
      { .minute_of_day = 7 * 60 + 5,  .weekday_mask = 0x1F, .enabled = true },
    };
    time_t t0700 = at(2026, 8, 5, 7, 0);    // Wednesday
    time_t t0701 = at(2026, 8, 5, 7, 1);
    time_t t0705 = at(2026, 8, 5, 7, 5);
    time_t t0711 = at(2026, 8, 5, 7, 11);

    // THE REGRESSION THIS FUNCTION EXISTS FOR. A rang at 07:00 and was snoozed
    // at 07:01 until 07:11, so pending_slot is 0 and served is (0, 07:00). B's
    // own deadline fires at 07:05. The answer must be B -- reading pending_slot
    // instead rang A again, restarted its ramp, destroyed the snooze, and lost
    // B silently until tomorrow.
    AcWakeDecision d = ac_dispatch_wakeup(ab, 2, t0705,
                                          0 /*pending_slot*/, (uint32_t)t0711,
                                          1 /*snooze_count*/,
                                          0 /*served_slot*/, t0700, 0 /*lead_s*/);
    assert(d.action == AC_WAKE_RING_DEADLINE);
    assert(d.slot == 1);

    // The same cycle one minute in, with nothing else due: the snooze must be
    // left strictly alone. KEEP, not NONE -- NONE ends the cycle, which is what
    // erased the snooze.
    d = ac_dispatch_wakeup(ab, 1 /*only A exists*/, t0701,
                           0, (uint32_t)t0711, 1, 0, t0700, 0);
    assert(d.action == AC_WAKE_KEEP);

    // The snooze expiry itself: resume A's ring, continuing the cycle.
    d = ac_dispatch_wakeup(ab, 1, t0711, 0, (uint32_t)t0711, 1, 0, t0700, 0);
    assert(d.action == AC_WAKE_RESUME);
    assert(d.slot == 0);

    // A snooze expiry and another alarm's deadline coinciding: the deadline
    // wins (the user's decision, 2026-08-05 -- a backup alarm must ring).
    d = ac_dispatch_wakeup(ab, 2, t0705, 0, (uint32_t)t0705, 1, 0, t0700, 0);
    assert(d.action == AC_WAKE_RING_DEADLINE);
    assert(d.slot == 1);

    // A mid-ring keep-alive after an eviction: A's ring started at 07:00 and was
    // never dismissed (snooze_count 0, ring_started_at in the past). Resume it,
    // so the escalation ramp continues instead of restarting.
    d = ac_dispatch_wakeup(ab, 1, t0700 + 240, 0, (uint32_t)t0700, 0, 0, t0700, 0);
    assert(d.action == AC_WAKE_RESUME);
    assert(d.slot == 0);

    // No alarms at all: nothing to ring, nothing live.
    d = ac_dispatch_wakeup(ab, 0, t0705, -1, 0, 0, -1, 0, 0);
    assert(d.action == AC_WAKE_NONE);

    // A stale pending_slot -- the phone deleted alarms while this wakeup was in
    // flight -- must never come back as a slot to ring. Nothing else is due here
    // (A's 07:00 is served and its next occurrence is tomorrow), so the honest
    // answer is NONE with no slot, and the caller's cleanup branch runs.
    d = ac_dispatch_wakeup(ab, 1, t0711, 5 /*out of range*/, (uint32_t)t0711, 1,
                           0, t0700, 0);
    assert(d.action == AC_WAKE_NONE);
    assert(d.slot == -1);

    // An unserved alarm whose deadline passed a minute ago IS due -- the wakeup
    // may have been late, and an alarm that has not rung must still ring.
    d = ac_dispatch_wakeup(ab, 1, t0701, -1, 0, 0, -1, 0, 0);
    assert(d.action == AC_WAKE_RING_DEADLINE);
    assert(d.slot == 0);

    // A disabled alarm's occurrence is not due, and no cycle is live.
    Alarm off1 = { .minute_of_day = 7 * 60, .weekday_mask = 0x1F, .enabled = false };
    d = ac_dispatch_wakeup(&off1, 1, t0700, -1, 0, 0, -1, 0, 0);
    assert(d.action == AC_WAKE_NONE);

    // The tolerance works in both directions: a deadline two minutes AHEAD of
    // now (an E_RANGE-shifted wakeup firing early) still counts as due.
    d = ac_dispatch_wakeup(ab, 2, t0705 - 120, -1, 0, 0, 0, t0700, 0);
    assert(d.action == AC_WAKE_RING_DEADLINE);
    assert(d.slot == 1);

    // REGRESSION: SEMANTICS_RING_FROM's deadline sits AFTER the raw occurrence
    // (lead_s is negative there), so a fixed `now - tol` search base searches
    // forward from a point already well past the alarm's own occurrence and
    // lands on tomorrow's instead -- a due alarm that has never rung gets
    // reported as "nothing due". A at 07:00 with a 10-minute window: deadline
    // is 07:10, so lead_s = -600 (deadline = when - lead_s = when + 600). At
    // the deadline instant, with nothing served and no live cycle, this MUST
    // ring.
    d = ac_dispatch_wakeup(ab, 1, t0700 + 600, -1, 0, 0, -1, 0, -600);
    assert(d.action == AC_WAKE_RING_DEADLINE);
    assert(d.slot == 0);

    // SEMANTICS_AWAKE_BY: lead_s positive (the escalation's full development
    // time), deadline BEFORE the alarm time. 15 minutes of development: at the
    // deadline instant (07:00 - 900s), this must ring too.
    d = ac_dispatch_wakeup(ab, 1, t0700 - 900, -1, 0, 0, -1, 0, 900);
    assert(d.action == AC_WAKE_RING_DEADLINE);
    assert(d.slot == 0);

    // Same RING_FROM window (lead_s negative again), but well before the
    // deadline: not due yet, and no live cycle -- NONE, proving the shifted
    // search base did not turn into "always find something".
    d = ac_dispatch_wakeup(ab, 1, t0700, -1, 0, 0, -1, 0, -600);
    assert(d.action == AC_WAKE_NONE);

    // alarms == NULL: nothing to consult, nothing live.
    d = ac_dispatch_wakeup(NULL, 0, t0700, -1, 0, 0, -1, 0, 0);
    assert(d.action == AC_WAKE_NONE);
    assert(d.slot == -1);

    // pending_slot == count exactly is the boundary case, not a far-out index:
    // still out of range, still not live.
    d = ac_dispatch_wakeup(ab, 1, t0701, 1 /*== count*/, (uint32_t)t0711, 1,
                           0, t0700, 0);
    assert(d.action == AC_WAKE_NONE);
    assert(d.slot == -1);

    // RESUME/KEEP boundary on the snooze expiry: exactly at now + tol resumes;
    // one second later it has not expired yet and the cycle is left alone.
    d = ac_dispatch_wakeup(ab, 1, t0700, 0,
                           (uint32_t)(t0700 + AC_SERVED_TOLERANCE_S), 1, 0, t0700, 0);
    assert(d.action == AC_WAKE_RESUME);
    assert(d.slot == 0);

    d = ac_dispatch_wakeup(ab, 1, t0700, 0,
                           (uint32_t)(t0700 + AC_SERVED_TOLERANCE_S + 1), 1, 0, t0700, 0);
    assert(d.action == AC_WAKE_KEEP);
  }

  // --- ac_cycle_state: the single answer to "what is ongoing" ---
  {
    const time_t now = at(2026, 8, 5, 14, 0);
    const uint32_t future = (uint32_t)at(2026, 8, 5, 14, 30);
    const uint32_t past = (uint32_t)at(2026, 8, 5, 13, 0);

    // Nothing live.
    assert(ac_cycle_state(-1, 0, 0, 0, now) == AC_CYCLE_NONE);
    // A slot with no cycle fields is still nothing live.
    assert(ac_cycle_state(0, 0, 0, 0, now) == AC_CYCLE_NONE);

    // A snooze in flight (count > 0 and the expiry is still ahead) wins.
    assert(ac_cycle_state(0, 0, future, 1, now) == AC_CYCLE_SNOOZE);
    // ... even when a window is recorded too.
    assert(ac_cycle_state(0, past, future, 1, now) == AC_CYCLE_SNOOZE);

    // A ring cycle with no snooze: ring_started_at set, count 0. This is the
    // force-quit-mid-ring state the menu has to be able to show.
    assert(ac_cycle_state(0, 0, past, 0, now) == AC_CYCLE_RINGING);
    // An EXPIRED snooze is no longer pending, so it reads as a live ring, not
    // as a snooze -- the ring is what the keep-alive wakeup will resume.
    assert(ac_cycle_state(0, 0, past, 2, now) == AC_CYCLE_RINGING);
    // A ring beats a window (start_ring zeroes window_started_at, so a cycle
    // carrying both is already inconsistent; the ring is the live thing).
    assert(ac_cycle_state(0, past, past, 0, now) == AC_CYCLE_RINGING);

    // A smart window open, nothing ringing.
    assert(ac_cycle_state(0, past, 0, 0, now) == AC_CYCLE_WINDOW);

    // pending_slot < 0 with ANY live cycle is an orphan -- the owning alarm is
    // gone (ac_prune_spent_one_time mapped it away), whatever the cycle was.
    assert(ac_cycle_state(-1, past, 0, 0, now) == AC_CYCLE_ORPHAN);
    assert(ac_cycle_state(-1, 0, past, 0, now) == AC_CYCLE_ORPHAN);
    assert(ac_cycle_state(-1, 0, future, 1, now) == AC_CYCLE_ORPHAN);

    // Boundary: a snooze expiring exactly at `now` has expired (ac_snooze_pending
    // requires the expiry to be strictly ahead), so this is a live ring.
    assert(ac_cycle_state(0, 0, (uint32_t)now, 1, now) == AC_CYCLE_RINGING);
  }

  // --- ac_cycle_is_stale: a stored deadline that belongs to another occurrence ---
  {
    const time_t dl_today = at(2026, 8, 5, 8, 20);
    const time_t dl_tomorrow = at(2026, 8, 6, 8, 20);

    // No window live: nothing to be stale about (the caller has no cycle to end).
    assert(!ac_cycle_is_stale(0, 0, dl_tomorrow));
    assert(!ac_cycle_is_stale(0, (uint32_t)dl_today, dl_tomorrow));

    // A live window whose stored deadline IS this occurrence's: the normal
    // mid-window re-entry. sc_rearm computed both from the same occurrence, so
    // they are equal by construction.
    assert(!ac_cycle_is_stale((uint32_t)at(2026, 8, 5, 7, 50),
                             (uint32_t)dl_today, dl_today));

    // The 2026-08-05 defect: a 13:52 Test alarm left window=13:52 deadline=14:22,
    // and a re-entry re-adopted it for TOMORROW's 07:50 occurrence while keeping
    // the 14:22 deadline -- which then rang that afternoon for nothing.
    assert(ac_cycle_is_stale((uint32_t)at(2026, 8, 5, 13, 52),
                            (uint32_t)at(2026, 8, 5, 14, 22), dl_tomorrow));

    // A live window with NO stored deadline cannot be trusted either.
    assert(ac_cycle_is_stale((uint32_t)at(2026, 8, 5, 7, 50), 0, dl_today));
  }

  // --- ac_cycle_is_stale under SEMANTICS_RING_FROM: the comparison MUST be
  // made against the cycle's OWN occurrence, not "the next occurrence after
  // now" -- fix round 1's regression.
  //
  // RING_FROM's window sits [T, T+w], so a re-entry INSIDE a live window has
  // `now` already past T; "the next occurrence after now" then resolves to
  // TOMORROW, and comparing the stored deadline against that discarded a
  // perfectly live window and lost the day's alarm entirely. The stored
  // window start identifies the occurrence the cycle is about, so the
  // production code now probes the alarm's occurrence grid from
  // `window_started_at - 1`, not from `now`.
  {
    Alarm daily = { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x7F,
                    .enabled = true };
    const uint16_t window_min = 30;
    const uint32_t full_dev_s = 0;
    const time_t win_start_today = at(2026, 8, 5, 7, 50);
    const time_t dl_today = at(2026, 8, 5, 8, 20);
    const time_t dl_tomorrow = at(2026, 8, 6, 8, 20);

    // 1. Validating against the CYCLE'S occurrence (base = window start - 1):
    // NOT stale for a live window.
    {
      time_t cycle_when = ac_next_occurrence(&daily, win_start_today - 1);
      time_t cycle_deadline = ac_ring_deadline(SEMANTICS_RING_FROM, true,
                                               window_min, full_dev_s, cycle_when);
      assert(cycle_deadline == dl_today);
      assert(!ac_cycle_is_stale((uint32_t)win_start_today, (uint32_t)dl_today,
                                cycle_deadline));
    }

    // 2. The same validation still reports STALE for the real orphan this
    // task exists for: a 13:52 window/14:22 deadline left behind under this
    // same daily 07:50 alarm.
    {
      const time_t win_start_orphan = at(2026, 8, 5, 13, 52);
      const time_t dl_orphan = at(2026, 8, 5, 14, 22);
      time_t cycle_when = ac_next_occurrence(&daily, win_start_orphan - 1);
      assert(cycle_when == at(2026, 8, 6, 7, 50));   // tomorrow: today's already rang
      time_t cycle_deadline = ac_ring_deadline(SEMANTICS_RING_FROM, true,
                                               window_min, full_dev_s, cycle_when);
      assert(cycle_deadline == dl_tomorrow);
      assert(ac_cycle_is_stale((uint32_t)win_start_orphan, (uint32_t)dl_orphan,
                               cycle_deadline));
    }

    // 3. THE REGRESSION THIS ROUND FIXED: validating against "the next
    // occurrence after now" (base = now - 60, now = 07:53, mid-window)
    // instead of against the cycle's own occurrence reports STALE for the
    // SAME LIVE window as case 1 -- because by 07:53 today's 07:50 has
    // already passed, so "the next occurrence after now" is tomorrow's. This
    // is exactly the wrong comparison the round-1 fix replaced in main.c; do
    // NOT "simplify" the production code's search base back to `now` --
    // this assertion is what would catch that regression.
    {
      const time_t now_mid_window = at(2026, 8, 5, 7, 53);
      time_t wrong_when = ac_next_occurrence(&daily, now_mid_window - 60);
      assert(wrong_when == at(2026, 8, 6, 7, 50));   // tomorrow -- the bug
      time_t wrong_deadline = ac_ring_deadline(SEMANTICS_RING_FROM, true,
                                               window_min, full_dev_s, wrong_when);
      assert(ac_cycle_is_stale((uint32_t)win_start_today, (uint32_t)dl_today,
                               wrong_deadline));
    }

    // 4. A live cycle with NO OWNING ALARM is never probed at all: main.c runs
    // the occurrence probe only for the alarm that owns the cycle (a foreign
    // alarm's grid answers a question nobody asked, and adopting its occurrence
    // could ring an instant already hours past). So production passes
    // occurrence_deadline 0 for an unowned cycle -- which must read as stale, so
    // that the orphan of case 2 is still ended even when the alarm that left it
    // behind has been pruned away.
    {
      const time_t win_start_orphan = at(2026, 8, 5, 13, 52);
      const time_t dl_orphan = at(2026, 8, 5, 14, 22);
      assert(ac_cycle_is_stale((uint32_t)win_start_orphan, (uint32_t)dl_orphan, 0));
    }

    // 5. WHAT A DISCARDED CYCLE IS RESTARTED FROM. The cycle's own occurrence is
    // not only what the staleness comparison is made against -- it is also the
    // basis main.c re-derives the fresh window and deadline from once the stale
    // cycle has been ended. A mid-window config change (the smart window shortened
    // 30 -> 20 at 07:55) must re-open the SAME window under the new config:
    {
      const time_t now_mid_window = at(2026, 8, 5, 7, 58);
      const uint16_t new_window_min = 20;
      time_t cycle_when = ac_next_occurrence(&daily, win_start_today - 1);
      assert(cycle_when == at(2026, 8, 5, 7, 50));          // today's -- the cycle's own
      // Stale under the new config (08:10 != the stored 08:20), so the cycle is
      // discarded...
      time_t new_deadline = ac_ring_deadline(SEMANTICS_RING_FROM, true,
                                            new_window_min, full_dev_s, cycle_when);
      assert(new_deadline == at(2026, 8, 5, 8, 10));
      assert(ac_cycle_is_stale((uint32_t)win_start_today, (uint32_t)dl_today,
                               new_deadline));
      // ...and re-opened for the same occurrence, window start unchanged and
      // already reached, so it opens immediately rather than being lost.
      time_t new_win = ac_window_start(SEMANTICS_RING_FROM, true,
                                       new_window_min, full_dev_s, cycle_when);
      assert(new_win == win_start_today);
      assert(new_win <= now_mid_window && new_deadline > now_mid_window);

      // Deriving the restart from "the next occurrence after now" instead loses
      // the day's alarm entirely: its window does not open until TOMORROW, so the
      // handler can only re-arm -- which is the defect this basis fixes.
      time_t wrong_when = ac_next_occurrence(&daily, now_mid_window - 60);
      assert(wrong_when == at(2026, 8, 6, 7, 50));
      assert(ac_window_start(SEMANTICS_RING_FROM, true, new_window_min,
                             full_dev_s, wrong_when) > now_mid_window);

      // And a disabled slot has no occurrence at all, so `when` is 0 there: a
      // ring instant derived from it is an epoch-era value, i.e. ALWAYS already
      // past -- which is why main.c must never compute one from 0.
      Alarm disabled = daily;
      disabled.enabled = false;
      assert(ac_next_occurrence(&disabled, now_mid_window - 60) == 0);
      assert(ac_ring_deadline(SEMANTICS_RING_FROM, true, new_window_min,
                              full_dev_s, 0) < now_mid_window);

      // THE RESTART BASIS IS THE ALARM AS IT ACTUALLY IS, not the forced probe.
      // The staleness probe forces enabled/skip_next so it can ask about the
      // alarm's GRID (deliberate: a cycle for an alarm switched off mid-window
      // keeps its previous behaviour). Its occurrence must never become the basis
      // for OPENING a window, because deleting an alarm on the phone leaves a
      // DISABLED one-time row in that slot -- with its own minute_of_day, so the
      // forced probe cheerfully returns an occurrence and a window would be opened
      // for an alarm that no longer exists. Read from the same base without the
      // force, a deleted/disabled slot yields 0, i.e. nothing to re-open:
      Alarm deleted_row = { .minute_of_day = 7 * 60 + 52, .weekday_mask = 0,
                            .enabled = false };
      Alarm forced = deleted_row;
      forced.enabled = true;
      assert(ac_next_occurrence(&forced, win_start_today - 1) != 0);   // the trap
      assert(ac_next_occurrence(&deleted_row, win_start_today - 1) == 0);
    }
  }

  // --- ac_window_wakeup: THE DECISION a WC_WINDOW / WC_REENTRY wakeup makes ---
  //
  // This block exists because the six defects found in that handler over two
  // rounds this week were all in main.c, where no test can reach them -- and the
  // assertions written alongside those fixes (the ac_cycle_is_stale and
  // ac_window_start cases above) passed identically against the buggy handler,
  // because they only exercised the arithmetic it happened to call. Each case
  // below is one of those defects, asserted on the DECISION, and each fails if the
  // corresponding bug comes back.
  //
  // Times are Europe/Helsinki (set at the top of main), Wed 2026-08-05 unless
  // stated. `full_dev_s` is 0 except in the SEMANTICS_AWAKE_BY cases, which pass a
  // fixed 1800 s (30 min) ramp -- 1200 s where a profile change is being modelled.
  {
    const Alarm daily = { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x7F,
                          .enabled = true };
    const time_t t0750 = at(2026, 8, 5, 7, 50);
    const time_t t0820 = at(2026, 8, 5, 8, 20);

    // 1. RING_FROM, live window [07:50, 08:20], re-entry at 07:58, config
    //    unchanged: continue the window from the STORED values, cycle untouched.
    //
    //    Locks out: discarding a live RING_FROM window. `when` inside such a
    //    window is TOMORROW's occurrence (today's 07:50 has passed), so
    //    validating the stored deadline against it reported stale, ended the
    //    cycle, and re-armed for tomorrow only -- the whole day's alarm lost.
    {
      WinCase c = { .alarms = &daily, .count = 1, .now = at(2026, 8, 5, 7, 58),
                    .semantics = SEMANTICS_RING_FROM, .smart_window_active = true,
                    .window_min = 30, .pending_slot = 0,
                    .window_started_at = t0750, .deadline_at = t0820,
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.action == AC_WIN_OPEN);
      assert(d.slot == 0);
      assert(d.window_start == t0750);      // the STORED window start
      assert(d.deadline == t0820);          // the STORED deadline
      assert(!d.end_cycle);
      assert(!d.abandon_screen);            // never on the open path
    }

    // 2. The same window, but the smart window length changed 30 -> 20 while it
    //    was open, so the stored deadline (08:20) is no longer this occurrence's
    //    (08:10): end the cycle and RE-OPEN THE SAME window under the new config.
    //
    //    Locks out: dropping the day's alarm on a mid-window config change. The
    //    first fix ended the stale cycle and then derived the restart from `when`
    //    -- tomorrow -- so the alarm never rang today. The restart basis must be
    //    the cycle's OWN occurrence.
    {
      WinCase c = { .alarms = &daily, .count = 1, .now = at(2026, 8, 5, 7, 58),
                    .semantics = SEMANTICS_RING_FROM, .smart_window_active = true,
                    .window_min = 20, .pending_slot = 0,
                    .window_started_at = t0750, .deadline_at = t0820,
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.end_cycle);                  // the stored deadline was 08:20
      assert(d.action == AC_WIN_OPEN);
      assert(d.slot == 0);
      assert(d.window_start == t0750);
      assert(d.deadline == at(2026, 8, 5, 8, 10));
      assert(!d.abandon_screen);            // the same screen is re-captioned
    }

    // 3. THE ORPHAN THAT STARTED ALL OF THIS. A one-time 13:52 test alarm was
    //    pruned out from under its own cycle, leaving window=13:52/deadline=14:22
    //    with pending_slot -1 and only a daily 07:50 alarm in the set. At 13:58 the
    //    re-entry resolved tomorrow's 07:50, kept the 14:22 deadline, read
    //    `now >= ring` that afternoon and rang a full escalating alarm nobody set.
    //
    //    Locks out: exactly that phantom ring.
    {
      WinCase c = { .alarms = &daily, .count = 1, .now = at(2026, 8, 5, 13, 58),
                    .semantics = SEMANTICS_RING_LATEST, .smart_window_active = true,
                    .window_min = 30, .pending_slot = -1,
                    .window_started_at = at(2026, 8, 5, 13, 52),
                    .deadline_at = at(2026, 8, 5, 14, 22),
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.action != AC_WIN_RING_NOW);  // THE defect
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(d.end_cycle);                  // the leftover must not survive
      assert(d.abandon_screen);             // ...nor the screen describing it
      assert(d.deadline == 0);              // nothing derived from 14:22
      assert(d.reason == AC_WIN_REASON_NO_OCCURRENCE);  // the orphan owns nothing
                                                         // to restart from
    }

    // 4. AN ALARM DELETED ON THE PHONE, mid-window. Deleting a slot shifts the
    //    survivors down and can leave a DISABLED row in the slot the cycle names
    //    -- here a spent one-time 22:00 row under a live 07:50/08:20 window. The
    //    forced staleness probe answers about that row's grid (22:30), which does
    //    not match the stored deadline, so the cycle is a leftover; and the
    //    restart basis, read UNFORCED, is 0 -- there is nothing to re-open.
    //
    //    Locks out: ringing for an alarm the user deleted. Deriving the restart
    //    from `when` (0 for a disabled slot) gave an epoch-era ring instant, i.e.
    //    always <= now, so this path started a full escalating alarm immediately.
    {
      const Alarm deleted_row = { .minute_of_day = 22 * 60, .weekday_mask = 0,
                                  .enabled = false };
      WinCase c = { .alarms = &deleted_row, .count = 1,
                    .now = at(2026, 8, 5, 7, 58),
                    .semantics = SEMANTICS_RING_FROM, .smart_window_active = true,
                    .window_min = 30, .pending_slot = 0,
                    .window_started_at = t0750, .deadline_at = t0820,
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(d.action != AC_WIN_RING_NOW);
      assert(d.end_cycle);
      assert(d.abandon_screen);
      assert(d.deadline == 0);
    }

    // 4b. THE DELIBERATELY DIFFERENT CASE, asserted so it cannot be "simplified"
    //     into 4: the SAME alarm merely SWITCHED OFF mid-window. The staleness
    //     probe forces enabled/skip_next precisely so this asks about the alarm's
    //     GRID, not its on/off state -- the occurrence still resolves to 07:50 and
    //     the stored deadline still fits, so the cycle stays live and the window
    //     continues. That an alarm switched off mid-window still rings is a
    //     recorded deferral, not an accident.
    {
      Alarm switched_off = daily;
      switched_off.enabled = false;
      WinCase c = { .alarms = &switched_off, .count = 1,
                    .now = at(2026, 8, 5, 7, 58),
                    .semantics = SEMANTICS_RING_FROM, .smart_window_active = true,
                    .window_min = 30, .pending_slot = 0,
                    .window_started_at = t0750, .deadline_at = t0820,
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.action == AC_WIN_OPEN);
      assert(!d.end_cycle);
      assert(d.window_start == t0750 && d.deadline == t0820);
    }

    // 5. THE ALARM MOVED INTO THE PAST WHILE ITS WINDOW WAS OPEN. Default
    //    semantics (RING_LATEST), a 07:50 daily alarm whose window opened at
    //    07:20; at 07:40 the user edits that alarm on the phone to 07:30, meaning
    //    TOMORROW. The config save re-arms, and the next re-entry finds the stored
    //    08:20-era cycle stale (this occurrence's deadline is now 07:30), ends it,
    //    and re-derives from the cycle's own occurrence -- today 07:30, which is
    //    already ten minutes behind us.
    //
    //    Locks out: ringing a full escalating alarm on the spot for an alarm time
    //    the user had just moved into the past. The restart basis comes from
    //    `window_started_at - 1`, so unlike the old basis (always > now - 60) it
    //    can be arbitrarily far behind `now` -- and `now >= ring` then read as "the
    //    deadline has passed, ring". ON THE DISCARDED PATH AN INSTANT ALREADY
    //    BEHIND `now` IS NOT A DEADLINE TO RING; IT IS A CYCLE THAT IS OVER.
    //
    //    The same edit made one minute BEFORE the window opened arms the alarm for
    //    tomorrow (asserted below), and an open window must not change that.
    {
      const Alarm moved = { .minute_of_day = 7 * 60 + 30, .weekday_mask = 0x7F,
                            .enabled = true };
      WinCase c = { .alarms = &moved, .count = 1, .now = at(2026, 8, 5, 7, 40),
                    .semantics = SEMANTICS_RING_LATEST, .smart_window_active = true,
                    .window_min = 30, .pending_slot = 0,
                    .window_started_at = at(2026, 8, 5, 7, 20),
                    .deadline_at = t0750, .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.end_cycle);                  // 07:30 != the stored 07:50 deadline
      assert(d.action != AC_WIN_RING_NOW);  // THE defect
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(d.deadline == 0);
      assert(d.abandon_screen);             // the cycle is over; free the screen
      assert(d.reason == AC_WIN_REASON_BASIS_PAST);  // the discarded basis (07:30)
                                                      // is already behind `now`

      // The contrast: no open window at all, same edit, same clock. Nothing today
      // -- which is what an open window must not be allowed to change.
      c.pending_slot = -1;
      c.window_started_at = 0;
      c.deadline_at = 0;
      d = win_decide(&c);
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(!d.end_cycle && !d.abandon_screen);
    }

    // 5b. THE BOUNDARY OF THAT RULE, in the mode where it is least obvious.
    //     RING_FROM's window is [T, T + w], so a discarded cycle's basis (T) is
    //     behind `now` by design and its window is still re-opened -- but only
    //     while the NEW deadline is ahead. Here the window length was cut 30 -> 10
    //     mid-window, so the new deadline was 08:00 and it is already 08:05: that
    //     cycle is over, not due. Re-opening it would be worse than pointless --
    //     the poll's first tick sees a deadline in the past and rings anyway, which
    //     is the very ring case 5 exists to prevent, reached by another door.
    {
      WinCase c = { .alarms = &daily, .count = 1, .now = at(2026, 8, 5, 8, 5),
                    .semantics = SEMANTICS_RING_FROM, .smart_window_active = true,
                    .window_min = 10, .pending_slot = 0,
                    .window_started_at = t0750, .deadline_at = t0820,
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.end_cycle);
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(d.action != AC_WIN_OPEN);
      assert(d.abandon_screen);

      // ...while ten minutes earlier the same change DOES re-open it (case 2's
      // rule), which is what makes this a boundary and not a reversal.
      c.now = at(2026, 8, 5, 7, 55);
      d = win_decide(&c);
      assert(d.end_cycle);
      assert(d.action == AC_WIN_OPEN);
      assert(d.window_start == t0750 && d.deadline == at(2026, 8, 5, 8, 0));
      assert(!d.abandon_screen);
    }

    // 6. pending_slot OUT OF RANGE with a live window -- the phone deleted the
    //    slots (or all of them) while the window was open. An index that no longer
    //    names an alarm is no owner, so the cycle must be ended.
    //
    //    Locks out: the 180 s wake loop. sc_rearm re-arms the rolling re-entry on
    //    window_started_at != 0 ALONE, so a live cycle nothing can clear woke the
    //    app every SC_REENTRY_GAP_S for ever, with the menu advertising an alarm
    //    that no longer existed.
    {
      // 6a. Every alarm deleted: count 0, so there is nothing to resolve at all.
      WinCase c = { .alarms = &daily, .count = 0, .now = at(2026, 8, 5, 7, 58),
                    .semantics = SEMANTICS_RING_FROM, .smart_window_active = true,
                    .window_min = 30, .pending_slot = 0,
                    .window_started_at = t0750, .deadline_at = t0820,
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(d.slot == -1);
      assert(d.end_cycle);
      assert(d.abandon_screen);
      assert(d.reason == AC_WIN_REASON_NO_SLOT);  // no alarm left to resolve at all

      // 6b. Some alarms left, but the cycle names a slot past the end of the set.
      c.count = 1;
      c.pending_slot = 3;
      d = win_decide(&c);
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(d.end_cycle);
      assert(d.abandon_screen);
    }

    // 7. A KILLED-RING CYCLE MUST NOT HIJACK THE NEXT ALARM'S WINDOW. A force-quit
    //    mid-ring leaves ring_started_at set, window_started_at 0 and pending_slot
    //    on the OLD alarm (0 here), and the launch re-arm drops that cycle's
    //    keep-alive, so it stays live indefinitely. When alarm 1's own WC_WINDOW
    //    then arrives, the decision must be about ALARM 1.
    //
    //    Locks out: reading pending_slot unconditionally. It made the handler work
    //    on alarm 0, whose window was still hours away, so it re-armed only -- and
    //    sc_rearm will not re-place a WC_WINDOW whose start has passed, so alarm
    //    1's smart window never opened at all that day.
    {
      const Alarm two[2] = {
        { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x7F, .enabled = true },
        { .minute_of_day = 6 * 60,      .weekday_mask = 0x7F, .enabled = true },
      };
      WinCase c = { .alarms = two, .count = 2, .now = at(2026, 8, 5, 5, 35),
                    .semantics = SEMANTICS_RING_LATEST, .smart_window_active = true,
                    .window_min = 30, .pending_slot = 0,
                    .window_started_at = 0,
                    .ring_started_at = at(2026, 8, 4, 7, 50),   // yesterday's ring
                    .deadline_at = at(2026, 8, 4, 7, 50),
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.slot == 1);                  // NOT 0, the killed cycle's slot
      assert(d.action == AC_WIN_OPEN);
      assert(d.window_start == at(2026, 8, 5, 5, 30));
      assert(d.deadline == at(2026, 8, 5, 6, 0));
      // The killed ring's own cycle is left alone here: only a live WINDOW is
      // ended by this handler, and a ring cycle may still be resumed.
      assert(!d.end_cycle);
    }

    // 8. AN OCCURRENCE THAT HAS ALREADY RUNG IS NEVER RE-OPENED, AND NEVER RUNG
    //    AGAIN. A smart window can ring at 07:20 for a 07:50 alarm, and then "the
    //    next occurrence after now" is still today's 07:50: start_ring records the
    //    served occurrence by its DEADLINE instant, and this reads that record.
    //    Here a stray window/re-entry wakeup arrives at 07:50:30, seconds past the
    //    deadline of an occurrence that already rang.
    //
    //    Locks out: the double ring that cost the user a 07:20 wake AND a 07:50 one
    //    on the same morning.
    {
      WinCase c = { .alarms = &daily, .count = 1, .now = t0750 + 30,
                    .semantics = SEMANTICS_RING_LATEST, .smart_window_active = true,
                    .window_min = 30, .pending_slot = -1,
                    .served_slot = 0, .served_at = t0750 };
      AcWindowDecision d = win_decide(&c);
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(d.action != AC_WIN_RING_NOW);
      assert(!d.end_cycle);                 // nothing invalid about the state
      assert(!d.abandon_screen);

      // Without the served guard this same wakeup DOES ring: the occurrence it
      // resolves is today's 07:50 and its deadline is already behind `now`.
      //
      // That control assertion is load-bearing twice over. It is also what caught
      // the first draft of case 5's fix: written as "only ring when the basis is
      // ahead of now OR a cycle is live" it blocked THIS ring too -- a wakeup with
      // no live cycle, arriving seconds after the alarm time, i.e. a missed
      // wake-up. The rule only ever applies to a DISCARDED cycle, whose basis can
      // reach back hours; a fresh basis is at most a minute behind by construction.
      time_t w = 0;
      assert(ac_next_alarm(&daily, 1, c.now - 60, &w) == 0);
      assert(w == t0750 && w <= c.now);
      // ...and with the record cleared, that is exactly what happens.
      c.served_slot = -1;
      d = win_decide(&c);
      assert(d.action == AC_WIN_RING_NOW);
      assert(d.deadline == t0750);
    }

    // 9. SEMANTICS_AWAKE_BY RINGS AS SOON AS THE RAMP IS DUE -- an immediate ring
    //    is legitimate in this mode, because the deadline is the alarm time MINUS
    //    the escalation's full development (1800 s here), so a deadline already
    //    behind `now` means the ramp genuinely must already be running.
    //
    //    Locks out: over-applying the rule of case 5 below. Note `now` is 07:50
    //    exactly, so the occurrence this resolves is NOT ahead of `now` -- a gate
    //    that refused to ring whenever the occurrence had been reached would break
    //    this legitimate ring.
    {
      const uint32_t ramp = 1800;
      WinCase c = { .alarms = &daily, .count = 1, .now = t0750,
                    .semantics = SEMANTICS_AWAKE_BY, .smart_window_active = true,
                    .window_min = 30, .full_dev_s = ramp, .pending_slot = 0,
                    .window_started_at = at(2026, 8, 5, 6, 50),
                    .deadline_at = at(2026, 8, 5, 7, 20),
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.action == AC_WIN_RING_NOW);
      assert(d.slot == 0);
      assert(d.deadline == at(2026, 8, 5, 7, 20));
      assert(!d.end_cycle);
      assert(!d.abandon_screen);             // start_ring closes the screen itself

      // 9b. The same mode on the DISCARDED path: the escalation profile changed
      //     mid-window (1800 -> 1200 s), so the stored 07:20 deadline is no longer
      //     this occurrence's 07:30 one. The cycle is ended -- and the ring must
      //     still happen at once at 07:35, because the ramp for a 07:50 alarm is
      //     already 5 minutes overdue. The restart basis (07:50) is still AHEAD of
      //     now, which is what distinguishes this from case 5.
      c.full_dev_s = 1200;
      c.now = at(2026, 8, 5, 7, 35);
      d = win_decide(&c);
      assert(d.end_cycle);
      assert(d.action == AC_WIN_RING_NOW);
      assert(d.deadline == at(2026, 8, 5, 7, 30));
      assert(!d.abandon_screen);
    }

    // 10. ORDINARY MID-WINDOW RE-ENTRIES, nothing changed, under both of the
    //     non-AWAKE_BY modes: continue from the stored values and touch nothing.
    //     The plain case, asserted because two separate fixes broke it (round 1
    //     discarded the live RING_FROM window; the round-2 restart basis had to be
    //     kept from re-deriving one).
    {
      // RING_LATEST: the window sits [T - w, T], so it opens at 07:20 and the
      // deadline IS the alarm time.
      WinCase latest = { .alarms = &daily, .count = 1, .now = at(2026, 8, 5, 7, 35),
                         .semantics = SEMANTICS_RING_LATEST,
                         .smart_window_active = true, .window_min = 30,
                         .pending_slot = 0,
                         .window_started_at = at(2026, 8, 5, 7, 20),
                         .deadline_at = t0750, .served_slot = -1 };
      AcWindowDecision d = win_decide(&latest);
      assert(d.action == AC_WIN_OPEN && d.slot == 0);
      assert(d.window_start == at(2026, 8, 5, 7, 20) && d.deadline == t0750);
      assert(!d.end_cycle && !d.abandon_screen);

      // RING_FROM: the window sits [T, T + w]; re-entry a minute before the far
      // end, the point at which `when` has long since become tomorrow's.
      WinCase from = { .alarms = &daily, .count = 1, .now = at(2026, 8, 5, 8, 19),
                       .semantics = SEMANTICS_RING_FROM,
                       .smart_window_active = true, .window_min = 30,
                       .pending_slot = 0, .window_started_at = t0750,
                       .deadline_at = t0820, .served_slot = -1 };
      d = win_decide(&from);
      assert(d.action == AC_WIN_OPEN && d.slot == 0);
      assert(d.window_start == t0750 && d.deadline == t0820);
      assert(!d.end_cycle && !d.abandon_screen);
    }

    // 11. NOTHING HERE MAY END A PENDING SNOOZE -- it is a promise already made to
    //     the user, and while one is in flight ring_started_at holds its EXPIRY.
    //     A snooze reaches this decision as AC_CYCLE_SNOOZE, never as a window, so
    //     pending_slot is not read as this wakeup's owner and the cycle is left
    //     exactly as it is. (The window this then resolves for the OTHER alarm is
    //     declined by open_smart_window, which is the one owner of that rule --
    //     opening it would zero ring_started_at/snooze_count, the only record of
    //     the snooze, and ring the wrong alarm at the snooze's expiry.)
    {
      const Alarm two[2] = {
        { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x7F, .enabled = true },
        { .minute_of_day = 6 * 60,      .weekday_mask = 0x7F, .enabled = true },
      };
      WinCase c = { .alarms = two, .count = 2, .now = at(2026, 8, 5, 5, 35),
                    .semantics = SEMANTICS_RING_LATEST, .smart_window_active = true,
                    .window_min = 30, .pending_slot = 0, .window_started_at = 0,
                    .ring_started_at = at(2026, 8, 5, 5, 45),   // snooze expiry
                    .snooze_count = 1, .deadline_at = at(2026, 8, 5, 5, 30),
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(!d.end_cycle);                 // the snooze survives, whatever else
      assert(!d.abandon_screen);
      assert(d.slot == 1);                  // the decision is about the OTHER alarm

      // ...and the same holds when every alarm has been deleted meanwhile: the
      // "no alarm left" path ends a live WINDOW only, never a snooze (which has no
      // window -- start_ring zeroes it -- so it can never reach that branch).
      c.count = 0;
      d = win_decide(&c);
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(d.slot == -1);
      assert(!d.end_cycle);
      assert(!d.abandon_screen);
    }

    // The smart window switched off entirely (the setting, or a zero length):
    // nothing to open, and the alarm is left to its hard deadline via the re-arm.
    {
      WinCase c = { .alarms = &daily, .count = 1, .now = at(2026, 8, 5, 7, 35),
                    .semantics = SEMANTICS_RING_LATEST, .smart_window_active = false,
                    .window_min = 0, .pending_slot = -1, .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(!d.end_cycle && !d.abandon_screen);
    }

    // A window whose start is still ahead: there is nothing to open yet, and
    // opening it would put a multi-hour "window" on the 1-minute poll, which could
    // fire on ordinary daytime movement.
    {
      WinCase c = { .alarms = &daily, .count = 1, .now = at(2026, 8, 5, 3, 0),
                    .semantics = SEMANTICS_RING_LATEST, .smart_window_active = true,
                    .window_min = 30, .pending_slot = -1, .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(!d.end_cycle && !d.abandon_screen);
      assert(d.reason == AC_WIN_REASON_WINDOW_AHEAD);  // the window is hours away
    }

    // THE TRIPWIRE FOR THE SMART-WINDOW-OFF GATE. Mutation testing proved that
    // replacing `if (!smart_window_active) { return d; }` with `if (false)` left
    // the whole suite green: no case above combines smart_window_active == false
    // with a LIVE cycle, which is the only state where that branch matters (with
    // no live cycle, `!live && basis == 0` or the served/no-slot guards already
    // return first). Here a window IS open (07:20-07:50) and the setting is off;
    // the ONLY thing standing between `now` and the escalating "not yet due, so
    // continue the [window, deadline] I was already given" AC_WIN_OPEN outcome is
    // this gate.
    {
      WinCase c = { .alarms = &daily, .count = 1, .now = at(2026, 8, 5, 7, 35),
                    .semantics = SEMANTICS_RING_LATEST, .smart_window_active = false,
                    .window_min = 0, .pending_slot = 0,
                    .window_started_at = at(2026, 8, 5, 7, 20), .deadline_at = t0750,
                    .served_slot = -1 };
      AcWindowDecision d = win_decide(&c);
      assert(d.action == AC_WIN_REARM_ONLY);
      assert(!d.end_cycle);
      assert(d.reason == AC_WIN_REASON_SMART_OFF);
    }
  }

  // ---------------------------------------------------------------------
  // ac_last_past_occurrence -- "which occurrence just rang?"
  //
  // This exists because the naive spelling is wrong in a way that only shows up
  // some of the time. `ac_next_occurrence(a, now - 25h)` returns the FIRST
  // occurrence after that base, so for a daily alarm it answers with the one
  // 24 h too early whenever `now` is less than an hour past the alarm -- and it
  // answers correctly the rest of the day, which is why it survived. The dump of
  // 2026-08-06, taken 17 minutes after a 07:50 alarm, printed
  // `a0 prev=08-05 07:50` next to `eval alarm=08-06 07:50`: the same artefact
  // disagreeing with itself, on the one field a reader uses to check that the
  // app and the user mean the same alarm.
  {
    Alarm daily = { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x7f,
                    .enabled = true, .skip_next = false };
    time_t from = at(2026, 8, 6, 8, 7) - 25 * 3600;

    // The regression: 17 minutes after the alarm, the answer is TODAY's.
    assert(ac_last_past_occurrence(&daily, at(2026, 8, 6, 8, 7), from)
           == at(2026, 8, 6, 7, 50));

    // Late the same evening -- the case the naive version happened to get right,
    // kept so a "simplification" back to it fails here too.
    assert(ac_last_past_occurrence(&daily, at(2026, 8, 5, 19, 17),
                                   at(2026, 8, 5, 19, 17) - 25 * 3600)
           == at(2026, 8, 5, 7, 50));

    // At the ring instant itself the occurrence in hand is the one ringing, not
    // yesterday's: the dump runs while an alarm is live often enough to matter.
    assert(ac_last_past_occurrence(&daily, at(2026, 8, 6, 7, 50),
                                   at(2026, 8, 6, 7, 50) - 25 * 3600)
           == at(2026, 8, 6, 7, 50));

    // One second before it, today's has not happened -- yesterday's is the last.
    assert(ac_last_past_occurrence(&daily, at(2026, 8, 6, 7, 50) - 1,
                                   at(2026, 8, 6, 7, 50) - 1 - 25 * 3600)
           == at(2026, 8, 5, 7, 50));

    // Nothing in the searched span: a Monday-only alarm read on a Thursday.
    Alarm mondays = { .minute_of_day = 7 * 60 + 50, .weekday_mask = 0x01,
                      .enabled = true, .skip_next = false };
    assert(ac_last_past_occurrence(&mondays, at(2026, 8, 6, 8, 7), from) == 0);

    // A disabled alarm has no occurrences at all -- both dump sites probe with a
    // forced-enabled copy precisely because they want the raw grid instead.
    Alarm off = daily;
    off.enabled = false;
    assert(ac_last_past_occurrence(&off, at(2026, 8, 6, 8, 7), from) == 0);

    // A search window that starts after `now` is empty, not an infinite walk.
    assert(ac_last_past_occurrence(&daily, at(2026, 8, 6, 8, 7),
                                   at(2026, 8, 7, 0, 0)) == 0);

    // A month-long span still answers with the LAST one, not the first it meets:
    // the walk is bounded (AC_LAST_PAST_MAX_STEPS), and the bound has to be
    // generous enough that a realistic span cannot truncate to a stale answer.
    assert(ac_last_past_occurrence(&daily, at(2026, 8, 6, 8, 7),
                                   at(2026, 7, 8, 0, 0))
           == at(2026, 8, 6, 7, 50));
  }

  // --- ac_prealarm_start ----------------------------------------------------
  // The pre-alarm waiting screen: it opens `pre_min` before the ALARM TIME, and
  // only when that is genuinely earlier than the smart window's own screen.
  printf("--- ac_prealarm_start ---\n");
  {
    const time_t T = 1000000;

    // Off: the feature is disabled, whatever the window does.
    assert(ac_prealarm_start(T, 0, T - 30 * 60) == 0);

    // The ordinary case: a 60-minute lead against a 30-minute window opens the
    // screen half an hour before the window would have.
    assert(ac_prealarm_start(T, 60, T - 30 * 60) == T - 60 * 60);

    // Lead SHORTER than the window: the smart window's screen is already up by
    // then, so a second wakeup would add nothing.
    assert(ac_prealarm_start(T, 15, T - 30 * 60) == 0);

    // Lead EXACTLY the window start: not strictly earlier, so still nothing.
    assert(ac_prealarm_start(T, 30, T - 30 * 60) == 0);

    // No smart window at all (smart off, or a platform with no Health API):
    // ac_window_start collapses onto the ring deadline, so any positive lead
    // qualifies and this screen is the ONLY one the user gets before the ring.
    assert(ac_prealarm_start(T, 60, T) == T - 60 * 60);

    // SEMANTICS_RING_FROM: the window runs [T, T+w], so window_start IS the
    // alarm time and every positive lead is earlier than it. This is the mode
    // the user actually runs, and it is why the lead is anchored on the alarm
    // time rather than on the ring deadline (which sits AFTER T here).
    assert(ac_prealarm_start(T, 90, T) == T - 90 * 60);

    // The maximum the config offers, and a one-minute lead, both plain.
    assert(ac_prealarm_start(T, 90, T - 60 * 60) == T - 90 * 60);
    assert(ac_prealarm_start(T, 1, T) == T - 60);
  }
  printf("  ac_prealarm_start: ok\n");

  printf("test_alarm_calc: all assertions passed\n");
  return 0;
}
