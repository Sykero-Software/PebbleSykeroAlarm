// SPDX-License-Identifier: GPL-3.0-only
#include "night_text.h"
#include <stdio.h>
#include <string.h>

static void fmt_hhmm(uint16_t minute_of_day, char *out, int n) {
  if (minute_of_day == NIGHT_NO_FIRE) {
    snprintf(out, (size_t)n, "--:--");
  } else {
    snprintf(out, (size_t)n, "%02u:%02u",
             (unsigned)(minute_of_day / 60) % 100u,
             (unsigned)(minute_of_day % 60) % 100u);
  }
}

const char *nt_sens_name(uint8_t percentile) {
  switch (percentile) {
    case 95: return "Low";
    case 90: return "Medium";
    case 82: return "High";
    default: return "Custom";
  }
}

// Appends and clamps, so `off` can never walk past the buffer even if the
// content grows. A macro rather than a va_list helper because vsnprintf is not
// exported by the Core firmware (pebble_warn_unsupported_functions.h).
#define APPEND(buf, len, off, ...) do { \
    if ((off) < (len)) { \
      (off) += snprintf((buf) + (off), (size_t)((len) - (off)), __VA_ARGS__); \
      if ((off) > (len)) { (off) = (len); } \
    } \
  } while (0)

void nt_build(const NightSummary *nights, int count,
              char *head, int head_len, char *body, int body_len) {
  if (head == NULL || body == NULL || head_len <= 0 || body_len <= 0) {
    return;
  }
  head[0] = '\0';
  body[0] = '\0';
  int ho = 0, bo = 0;

  if (nights == NULL || count <= 0) {
    APPEND(body, body_len, bo,
           "No nights recorded yet.\n\nThe summary appears after the first "
           "alarm with the smart alarm on.");
    return;
  }

  const NightSummary *t = &nights[0];
  char onset[8], fired[8];
  fmt_hhmm(t->onset_min, onset, sizeof(onset));
  fmt_hhmm(t->fired_min, fired, sizeof(fired));

  if (t->smart_unavailable) {
    // No head: there is no number to glance at, and an empty head is part of
    // this function's contract.
    APPEND(body, body_len, bo,
           "Smart alarm was unavailable last night, so the alarm rang at your "
           "set time.\n\nCheck that activity tracking is on in the watch "
           "settings.\n");
  } else {
    APPEND(head, head_len, ho, "Asleep from\n%s\n\n%s\n%s", onset,
           t->fired_by_deadline ? "Deadline at" : "Woke you at", fired);
    APPEND(body, body_len, bo, "Stir needed  %u\nYou at rest  %u\n",
           (unsigned)t->trigger_level, (unsigned)t->baseline);
    APPEND(body, body_len, bo, "\nAt other sensitivities:\n");
    for (int k = 0; k < NIGHT_ALT_COUNT; k++) {
      char at[8];
      fmt_hhmm(t->alt_fired_min[k], at, sizeof(at));
      APPEND(body, body_len, bo, "  %s (%u%%)  %s%s\n",
             nt_sens_name(t->alt_percentile[k]),
             (unsigned)t->alt_percentile[k], at,
             t->alt_percentile[k] == t->percentile ? "  *" : "");
    }
  }

  if (count > 1) {
    APPEND(body, body_len, bo, "\nEarlier:\n");
    for (int i = 1; i < count; i++) {
      char at[8];
      fmt_hhmm(nights[i].fired_min, at, sizeof(at));
      APPEND(body, body_len, bo, "  %s  %s\n", at,
             nights[i].smart_unavailable ? "unavailable"
           : nights[i].fired_by_deadline ? "deadline" : "smart");
    }
  }

  // The explanation goes LAST, after the numbers, so it never delays the
  // glance -- and it is not optional: the percentile runs OPPOSITE to the
  // sensitivity, so a user reading "Low (95%)" without this would reasonably
  // conclude the opposite of the truth. ASCII only (Gothic has no en dash).
  APPEND(body, body_len, bo,
         "\nWhat this means\n"
         "The alarm watches how much you move and rings at a light moment "
         "inside your window. \"Stir needed\" comes from your own night: the "
         "more still you sleep, the lower it is.\n\n"
         "The number is the same one as the Custom setting on your phone. A "
         "HIGHER number means it takes a BIGGER stir to wake you, so Low (95%%) "
         "wakes you least eagerly.\n\n"
         "The list shows when each sensitivity would have rung. Woke you too "
         "early? Choose Low. Rang at your set time having missed a good moment? "
         "Choose High. You change it in the app's settings on your phone. "
         "--:-- means that setting would not have rung early at all.\n");
}
