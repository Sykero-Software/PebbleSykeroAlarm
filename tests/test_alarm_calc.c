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

  printf("test_alarm_calc: all assertions passed\n");
  return 0;
}
