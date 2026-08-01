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
