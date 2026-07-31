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

  printf("test_alarm_calc: all assertions passed\n");
  return 0;
}
