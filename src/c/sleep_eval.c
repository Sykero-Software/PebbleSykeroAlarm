// SPDX-License-Identifier: GPL-3.0-only
#include "sleep_eval.h"
#include <stddef.h>

void se_default_cfg(SleepEvalCfg *out, uint8_t percentile, uint8_t required_minutes) {
  if (out == NULL) {
    return;
  }
  if (percentile < 70) percentile = 70;
  if (percentile > 99) percentile = 99;
  if (required_minutes < 1) required_minutes = 1;
  if (required_minutes > 5) required_minutes = 5;
  out->percentile = percentile;
  out->required_minutes = required_minutes;
  out->settle_minutes = 20;
  out->min_margin = 25;
  out->orient_bonus = 400;
  out->orient_step = 2;
}

// Sorting scratch. Static, not a local: 720 uint16 is 1440 bytes and the Pebble
// app stack is only about 2 KB. The event loop is single-threaded so a
// non-reentrant buffer is safe.
static uint16_t s_sorted[SE_MAX_SAMPLES];

// Shell sort. Hand-rolled rather than qsort() for the same reason the parsers
// avoid strtol: libc functions that are not in the firmware's export table link
// fine and then hard-fault on hardware.
static void prv_sort(uint16_t *a, int n) {
  for (int gap = n / 2; gap > 0; gap /= 2) {
    for (int i = gap; i < n; i++) {
      uint16_t v = a[i];
      int j = i;
      while (j >= gap && a[j - gap] > v) {
        a[j] = a[j - gap];
        j -= gap;
      }
      a[j] = v;
    }
  }
}

static uint8_t prv_nibble_dist(uint8_t a, uint8_t b) {
  // Both nibbles are a 16-step wrap-around quantisation, so the distance is
  // circular: step 15 and step 0 are adjacent, not 15 apart.
  int d = (int)a - (int)b;
  if (d < 0) d = -d;
  if (d > 8) d = 16 - d;
  return (uint8_t)d;
}

static bool prv_orientation_changed(uint8_t cur, uint8_t ref, uint8_t step) {
  uint8_t cy = cur & 0x0F, cp = (cur >> 4) & 0x0F;
  uint8_t ry = ref & 0x0F, rp = (ref >> 4) & 0x0F;
  return prv_nibble_dist(cy, ry) >= step || prv_nibble_dist(cp, rp) >= step;
}

// Most common orientation over samples[from,to), or the last valid one if there
// is no clear mode. 256 buckets is small enough to count directly.
static uint8_t prv_orientation_mode(const SleepMinute *s, int from, int to) {
  static uint8_t counts[256];
  for (int i = 0; i < 256; i++) {
    counts[i] = 0;
  }
  uint8_t best = 0;
  int best_n = -1;
  for (int i = from; i < to; i++) {
    if (s[i].is_invalid) {
      continue;
    }
    uint8_t o = s[i].orientation;
    if (counts[o] < 255) {
      counts[o]++;
    }
    if ((int)counts[o] > best_n) {
      best_n = counts[o];
      best = o;
    }
  }
  return best;
}

SleepEvalResult se_evaluate(const SleepMinute *samples, int count, int window_start,
                            bool is_restful, const SleepEvalCfg *cfg) {
  SleepEvalResult r = {0};
  r.fired_index = -1;
  r.insufficient_data = true;

  if (samples == NULL || cfg == NULL || count <= 0) {
    return r;
  }
  if (count > SE_MAX_SAMPLES) {
    // Keep the most recent samples: the tail is the night we care about.
    samples += (count - SE_MAX_SAMPLES);
    window_start -= (count - SE_MAX_SAMPLES);
    count = SE_MAX_SAMPLES;
  }
  if (window_start < 0) {
    window_start = 0;
  }
  if (window_start >= count) {
    return r;   // the window has not started yet; nothing to judge
  }

  // Collect the sleeping minutes, excluding the settling-in period. Morpheuz's
  // insight: the falling-asleep stretch is not representative of the night.
  int from = (int)cfg->settle_minutes;
  if (from >= count) {
    from = 0;
  }

  // The ranking population is the HISTORY strictly before the alarm window,
  // not "from..count" (which would include the window itself). Judging the
  // window's own anomalousness against a population that already contains the
  // window is circular, and folds whatever the window is doing into its own
  // threshold. Falls back to from..count -- the same population the window
  // would otherwise be measured against -- when there is no real history to
  // draw from (window_start <= from: a degenerate/synthetic case, since a real
  // alarm window is always well after the settle-in period).
  int pop_end = (window_start > from) ? window_start : count;

  int n = 0;
  for (int i = from; i < pop_end; i++) {
    if (!samples[i].is_invalid && n < SE_MAX_SAMPLES) {
      s_sorted[n++] = samples[i].vmc;
    }
  }
  if (n < SE_MIN_USABLE) {
    return r;
  }
  r.insufficient_data = false;

  prv_sort(s_sorted, n);
  r.baseline = s_sorted[n / 2];

  // Trim upper outliers from the ranking population before taking the
  // percentile, using Tukey's classic "far out" fence (k=3 x IQR above Q3).
  // The median baseline above is already robust to a contaminating stretch of
  // up to ~50% of the samples, but a raw percentile is not: any contiguous run
  // of elevated minutes larger than (100-percentile)% of the population --
  // e.g. an hour of restless tossing well after the settle-in window, still
  // part of ordinary sleep -- pushes the trigger level up to that stretch's
  // own magnitude, which silently disables the smart alarm for the rest of
  // the night (the most dangerous failure mode: it never fires and nothing
  // looks wrong). This only trims the pre-window HISTORY population above,
  // never the window evaluation loop below, so it cannot mask genuine current
  // activity -- only a past restless stretch is a contamination risk here.
  int q1 = s_sorted[n / 4];
  int q3 = s_sorted[(3 * n) / 4];
  int iqr = q3 - q1;
  int32_t fence = (int32_t)q3 + 3 * iqr;
  int n_trim = n;
  // n is already >= SE_MIN_USABLE here, so this floor never trims away more
  // than half the population, mirroring the median's own tolerance.
  int floor_n = n / 2;
  if (floor_n < SE_MIN_USABLE) {
    floor_n = SE_MIN_USABLE;
  }
  while (n_trim > floor_n && (int32_t)s_sorted[n_trim - 1] > fence) {
    n_trim--;
  }

  int pi = (int)(((uint32_t)n_trim * cfg->percentile) / 100u);
  if (pi >= n_trim) {
    pi = n_trim - 1;
  }
  uint32_t level = s_sorted[pi];
  uint32_t floor_level = (uint32_t)r.baseline + cfg->min_margin;
  if (level < floor_level) {
    level = floor_level;
  }
  if (level > 0xFFFF) {
    level = 0xFFFF;
  }
  r.trigger_level = (uint16_t)level;

  // The orientation reference is the mode of the ten minutes before the window.
  int ref_from = window_start - 10;
  if (ref_from < 0) {
    ref_from = 0;
  }
  uint8_t ref_orient = prv_orientation_mode(samples, ref_from, window_start > ref_from
                                                              ? window_start : ref_from + 1);

  uint32_t acc = 0;
  int contributing = 0;
  for (int i = window_start; i < count; i++) {
    if (samples[i].is_invalid) {
      acc = 0;
      contributing = 0;
      continue;
    }
    bool contributed = false;
    if (samples[i].vmc > r.trigger_level) {
      acc += (uint32_t)(samples[i].vmc - r.trigger_level);
      contributed = true;
    }
    if (prv_orientation_changed(samples[i].orientation, ref_orient, cfg->orient_step)) {
      acc += cfg->orient_bonus;
      contributed = true;
    }
    if (!contributed) {
      acc = 0;
      contributing = 0;
      continue;
    }
    contributing++;
    // Magnitude AND duration. acc >= trigger_level means a full trigger-level
    // minute of excess has built up; contributing >= required_minutes means it
    // was sustained rather than one spike.
    if (acc >= r.trigger_level && contributing >= (int)cfg->required_minutes) {
      r.acc = acc;
      r.fired_index = i;
      r.fire = !is_restful;   // deep sleep vetoes an early wake
      return r;
    }
  }
  r.acc = acc;
  return r;
}

int se_find_onset(const SleepMinute *samples, int count, uint16_t awake_above,
                  int quiet_minutes) {
  if (samples == NULL || count <= 0 || quiet_minutes <= 0) {
    return -1;
  }
  int run = 0;
  for (int i = 0; i < count; i++) {
    if (!samples[i].is_invalid && samples[i].vmc <= awake_above) {
      run++;
      if (run >= quiet_minutes) {
        return i - run + 1;
      }
    } else {
      run = 0;
    }
  }
  return -1;
}
