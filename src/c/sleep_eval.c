// SPDX-License-Identifier: GPL-3.0-only
#include "sleep_eval.h"
#include <stddef.h>

// A contiguous run of at least this many minutes above the wake threshold is
// treated as a wake episode (arousal), not ordinary sleep movement, and is
// excluded from the ranking population wholesale. R is set just above the
// longest elevated run an ordinary night produces: measured over 200
// independent simulated skewed nights, the longest naturally occurring
// elevated run was 7 minutes (distribution: 2->12 nights, 3->100, 4->70,
// 5->15, 6->2, 7->1, 8+->0). So R=8 leaves every one of those nights
// completely untouched while still catching the shortest plausible arousal.
#define SE_WAKE_RUN_MINUTES 8
// A minute is "elevated" (a candidate wake-episode minute) once its vmc
// exceeds this multiple of the resting median plus the configured margin.
// 4x is well above ordinary resting jitter (the still-sleeper tests show
// that sitting within a small band of the median) but well below a genuine
// position change, which real VMC data puts in the hundreds against a
// resting median in the tens.
#define SE_WAKE_THRESHOLD_MULT 4

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

// Fills s_sorted[0..n) with every valid vmc in samples[from,to), capped at
// SE_MAX_SAMPLES, and returns n. Used both for the rough (pre-exclusion)
// population and, after wake episodes are identified, to rebuild the ranking
// population from scratch excluding them -- so this buffer legitimately gets
// filled more than once per call.
static int prv_fill(const SleepMinute *samples, int from, int to) {
  int n = 0;
  for (int i = from; i < to; i++) {
    if (!samples[i].is_invalid && n < SE_MAX_SAMPLES) {
      s_sorted[n++] = samples[i].vmc;
    }
  }
  return n;
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
    // The window has not started yet; nothing to judge. insufficient_data is
    // left true here -- this module deliberately does not distinguish "the
    // window hasn't started" from "not enough sleep data" in the result. The
    // real caller (Task 11) only invokes se_evaluate once time has actually
    // entered the alarm window, so this branch only matters for the
    // degenerate/malformed-call case exercised by the host test, not real
    // operation; a dedicated status for it would be a public-struct change
    // this task does not need.
    return r;
  }

  // Collect the sleeping minutes, excluding the settling-in period. Morpheuz's
  // insight: the falling-asleep stretch is not representative of the night.
  int from = (int)cfg->settle_minutes;
  if (from >= count) {
    from = 0;
  }

  // The ranking population is the HISTORY strictly before the alarm window,
  // not "from..count" (which would include the window itself). Judging the
  // window's own anomalousness against a population that already contains
  // the window is circular: real usage re-runs se_evaluate repeatedly while
  // the window is in progress, so every minute of genuine stirring the
  // window records would enlarge its own threshold as it goes. Falls back to
  // from..count when the pre-window history alone is too thin (retried
  // below), matching the population the window would otherwise be measured
  // against.
  int pop_end = (window_start > from) ? window_start : count;

  int n = prv_fill(samples, from, pop_end);
  if (n < SE_MIN_USABLE && pop_end != count) {
    // Not enough history before the window on its own -- retry against the
    // whole recorded stretch (history + window) before giving up.
    // insufficient_data must mean "not enough sleep data to build a
    // distribution from", not "not enough history happened to precede this
    // particular window start"; a short pre-window gap is not a data
    // problem, and reporting it as one would be the same silent-never-fires
    // failure relocated rather than fixed.
    pop_end = count;
    n = prv_fill(samples, from, pop_end);
  }
  if (n < SE_MIN_USABLE) {
    return r;
  }
  r.insufficient_data = false;

  // baseline is the median of the population BEFORE wake-episode exclusion
  // (below): the exclusion threshold itself is derived from this baseline,
  // so it has to be computed first, from the unfiltered set.
  prv_sort(s_sorted, n);
  r.baseline = s_sorted[n / 2];

  // Exclude WAKE EPISODES from the ranking population: contiguous runs of at
  // least SE_WAKE_RUN_MINUTES consecutive minutes whose vmc exceeds
  // SE_WAKE_THRESHOLD_MULT x baseline + min_margin. This is what actually
  // fixes the "restless early stretch" contamination (an hour of real
  // thrashing, well after the settle-in period, is not sleep and must not
  // set the trigger level) -- and it does so by RUN LENGTH, not by
  // distribution shape, so an ordinary right-skewed night (mostly still, a
  // quarter light movement, a few genuine but brief position changes, none
  // of it forming a sustained run) is left completely untouched: no run, no
  // trim, the percentile keeps its natural shape and the sensitivity levels
  // stay distinct. A distribution-shape trim (a statistical outlier fence)
  // was tried and rejected: real VMC is strongly right-skewed, so it flagged
  // the legitimate upper tail as noise and collapsed every sensitivity onto
  // the same floor, making the sensitivity setting a no-op.
  //
  // An invalid minute is BRIDGED, not treated as a run break: it is already
  // absent from the population either way (prv_fill skips it), so letting it
  // pass through a run in progress is strictly safer than breaking on it.
  // Breaking on it reinstates the exact contamination this exclusion exists
  // to remove: a real wake episode with one dropped sample every few minutes
  // (ordinary when the watch is off the wrist or health data is patchy)
  // would otherwise fragment into runs individually too short to exclude.
  uint32_t wake_threshold = (uint32_t)r.baseline * SE_WAKE_THRESHOLD_MULT + cfg->min_margin;
  int n_trim = 0;
  int run_start = -1;
  for (int i = from; i <= pop_end; i++) {
    if (i < pop_end && samples[i].is_invalid) {
      continue;   // bridge: neither extends nor breaks a run
    }
    bool elevated = (i < pop_end) && samples[i].vmc > wake_threshold;
    if (elevated) {
      if (run_start < 0) {
        run_start = i;
      }
      continue;
    }
    if (run_start >= 0) {
      int run_len = i - run_start;
      if (run_len < SE_WAKE_RUN_MINUTES) {
        // Too short to be a wake episode -- these are ordinary population
        // samples after all.
        for (int j = run_start; j < i && n_trim < SE_MAX_SAMPLES; j++) {
          if (!samples[j].is_invalid) {
            s_sorted[n_trim++] = samples[j].vmc;
          }
        }
      }
      // else: a genuine wake episode -- excluded entirely.
      run_start = -1;
    }
    if (i < pop_end && n_trim < SE_MAX_SAMPLES) {
      s_sorted[n_trim++] = samples[i].vmc;   // valid (checked above) and non-elevated
    }
  }

  // If excluding wake episodes left too little to rank confidently, fall
  // back to the unfiltered population rather than ranking against a
  // near-empty set. n is already known >= SE_MIN_USABLE, so this floor is
  // reachable only when a wake episode consumed most of the history -- rare,
  // but ranking against fewer than SE_MIN_USABLE samples would be no more
  // trustworthy than the "not enough data" case already gated on above.
  if (n_trim < SE_MIN_USABLE) {
    n_trim = prv_fill(samples, from, pop_end);
  }

  prv_sort(s_sorted, n_trim);
  int pi = (int)(((uint32_t)n_trim * cfg->percentile) / 100u);
  // No pi>=n_trim clamp needed: cfg->percentile <= 99 (enforced by
  // se_default_cfg) and n_trim >= SE_MIN_USABLE (60) here, so
  // pi = n_trim*99/100 < n_trim always.
  uint32_t level = s_sorted[pi];
  uint32_t floor_level = (uint32_t)r.baseline + cfg->min_margin;
  if (level < floor_level) {
    level = floor_level;
  }
  if (level > 0xFFFF) {
    level = 0xFFFF;
  }
  r.trigger_level = (uint16_t)level;

  uint32_t acc = 0;
  int contributing = 0;
  for (int i = window_start; i < count; i++) {
    if (samples[i].is_invalid) {
      acc = 0;
      contributing = 0;
      continue;
    }
    // The orientation reference is the mode of the ten minutes IMMEDIATELY
    // BEFORE this one -- ROLLING, recomputed per window minute.
    //
    // It used to be computed once, from the ten minutes before the window, and
    // then held fixed for the whole window. That made a wrist which settled
    // into a new position score orient_bonus (400) EVERY remaining minute of
    // the window instead of once, so the accumulator grew without bound from
    // the first sustained turn-over onwards: the window effectively ended at
    // the first posture change no matter how the sensitivity was set, which
    // made the sensitivity setting inert exactly when orientation was the
    // signal that fired (measured on the host: a flat-VMC night with one
    // posture change fired at the same window minute at P70/P79/P88/P97, and
    // with required_minutes raised to 25 it still fired, acc = 400 * 25).
    //
    // Rolling, a genuine turn-over still contributes -- for the ~6 minutes the
    // mode takes to catch up (5 of the 10 reference minutes must be the new
    // posture before the mode flips), which comfortably satisfies
    // required_minutes (1..5), so the turn-over case still fires and near the
    // same minute. But a posture that has SETTLED stops contributing, so the
    // accumulator resets and the window goes on judging the night.
    int ref_from = i - 10;
    if (ref_from < 0) {
      ref_from = 0;
    }
    uint8_t ref_orient = prv_orientation_mode(samples, ref_from,
                                              i > ref_from ? i : ref_from + 1);

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
