// SPDX-License-Identifier: GPL-3.0-only
#include "sleep_text.h"
#include <stdio.h>

// Truncated tenths of an hour, matching TimeStyle's sleep_format_decimal.
// Truncation (not rounding) on purpose: the two agree only if they discard the
// remainder the same way, and TimeStyle has shipped truncating since its sleep
// widget existed.
static int prv_tenths(int seconds) {
  if (seconds < 0) { seconds = 0; }
  return seconds * 10 / 3600;
}

void st_format_slept(int deep_s, int total_s, char *buf, int buf_len) {
  if (buf == NULL || buf_len < 1) {
    return;
  }
  if (total_s <= 0) {
    buf[0] = '\0';
    return;
  }
  if (deep_s < 0) { deep_s = 0; }
  // The two sums are read from health independently, so a sample landing
  // between the two calls can leave deep marginally ahead of the total. Clamp,
  // rather than print a pair that cannot be true.
  if (deep_s > total_s) { deep_s = total_s; }

  int dt = prv_tenths(deep_s);
  int tt = prv_tenths(total_s);
  snprintf(buf, (size_t)buf_len, "Slept %d.%d/%d.%d h",
           dt / 10, dt % 10, tt / 10, tt % 10);
}
