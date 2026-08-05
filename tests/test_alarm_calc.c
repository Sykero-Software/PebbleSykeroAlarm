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

  printf("test_alarm_calc: all assertions passed\n");
  return 0;
}
