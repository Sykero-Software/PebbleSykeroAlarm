// SPDX-License-Identifier: GPL-3.0-only
#include <pebble.h>
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
  tm.tm_isdst = -1;   // let mktime resolve DST for the target date
  return mktime(&tm);
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
