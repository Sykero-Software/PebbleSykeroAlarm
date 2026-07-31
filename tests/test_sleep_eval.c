// SPDX-License-Identifier: GPL-3.0-only
#include "sleep_eval.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static SleepMinute g_s[SE_MAX_SAMPLES];

// Deterministic pseudo-random so nights are reproducible without rand().
static uint32_t g_seed = 12345;
static uint32_t nextr(void) {
  g_seed = g_seed * 1103515245u + 12345u;
  return (g_seed >> 16) & 0x7FFF;
}

// Fill [0,n) with a resting night: vmc jitters around `rest`.
static void fill_rest(int n, uint16_t rest, uint16_t jitter) {
  for (int i = 0; i < n; i++) {
    g_s[i].vmc = (uint16_t)(rest + (jitter ? nextr() % jitter : 0));
    g_s[i].orientation = 0x33;
    g_s[i].is_invalid = false;
  }
}

static SleepEvalCfg cfg_for(uint8_t pct, uint8_t mins) {
  SleepEvalCfg c;
  se_default_cfg(&c, pct, mins);
  return c;
}

// Fill [0,n) with a realistic RIGHT-SKEWED night: 70% near-still, 25% "light"
// movement, 5% a genuine position change -- drawn independently per minute,
// so no engineered run ever forms. Real VMC is nothing like the symmetric
// fill_rest() jitter above; this is what exposed the Tukey-fence version
// (see the review this test responds to): a distribution-shape trim flags
// the legitimate upper tail of a skew like this as noise, which fill_rest's
// narrow symmetric jitter can never demonstrate.
static void fill_skewed(int n) {
  for (int i = 0; i < n; i++) {
    uint32_t roll = nextr() % 100;
    uint16_t vmc;
    if (roll < 70) {
      vmc = (uint16_t)(nextr() % 15);            // still: 0-14
    } else if (roll < 95) {
      vmc = (uint16_t)(20 + nextr() % 121);      // light: 20-140
    } else {
      vmc = (uint16_t)(200 + nextr() % 901);     // position change: 200-1100
    }
    g_s[i].vmc = vmc;
    g_s[i].orientation = 0x33;
    g_s[i].is_invalid = false;
  }
}

int main(void) {
  const int N = 480;              // 8 h of sleep
  const int WIN = N - 30;         // last 30 min is the alarm window

  // --- a still sleeper: nothing in the window stands out -> no fire ---
  {
    g_seed = 1;
    fill_rest(N, 30, 10);
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    printf("  still      base=%u level=%u acc=%lu fire=%d\n",
           r.baseline, r.trigger_level, (unsigned long)r.acc, (int)r.fire);
    assert(!r.insufficient_data);
    assert(!r.fire);
    // the level must sit clearly above the resting median
    assert(r.trigger_level > r.baseline);
  }

  // --- one violent twitch: high magnitude, one minute only -> must NOT fire.
  // This is the firmware's failure mode (VMC > 0) and the single most important
  // assertion in this file.
  {
    g_seed = 2;
    fill_rest(N, 30, 10);
    g_s[WIN + 10].vmc = 5000;
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    printf("  one twitch base=%u level=%u fire=%d\n",
           r.baseline, r.trigger_level, (int)r.fire);
    assert(!r.fire);
  }

  // --- sustained stirring over several minutes -> fires ---
  {
    g_seed = 3;
    fill_rest(N, 30, 10);
    for (int i = WIN + 8; i < WIN + 14; i++) {
      g_s[i].vmc = 900;
    }
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    printf("  sustained  fire=%d at=%d (window starts %d)\n",
           (int)r.fire, r.fired_index, WIN);
    assert(r.fire);
    assert(r.fired_index >= WIN + 8 && r.fired_index <= WIN + 14);
  }

  // --- restful-sleep veto: the same night must not fire while in deep sleep ---
  {
    g_seed = 3;
    fill_rest(N, 30, 10);
    for (int i = WIN + 8; i < WIN + 14; i++) {
      g_s[i].vmc = 900;
    }
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, true /* is_restful */, &c);
    assert(!r.fire);
  }

  // --- a turn-over: vmc stays low but the orientation changes -> fires via the
  // orientation bonus ---
  {
    g_seed = 4;
    fill_rest(N, 30, 6);
    for (int i = WIN + 12; i < N; i++) {
      g_s[i].orientation = 0x77;    // both nibbles moved by 4 steps
      g_s[i].vmc = 40;
    }
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    printf("  turn-over  fire=%d at=%d\n", (int)r.fire, r.fired_index);
    assert(r.fire);
  }

  // --- quantisation jitter of ONE step must not count as a turn-over ---
  {
    g_seed = 5;
    fill_rest(N, 30, 6);
    for (int i = WIN; i < N; i++) {
      g_s[i].orientation = (i % 2) ? 0x33 : 0x34;   // yaw wobbles by 1
      g_s[i].vmc = 40;
    }
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    printf("  jitter     fire=%d\n", (int)r.fire);
    assert(!r.fire);
  }

  // --- turn over ONCE and then stay still: the orientation bonus must NOT keep
  // scoring for the rest of the window.
  //
  // The whole-branch review's Important 1. The reference orientation used to be
  // computed once (the ten minutes before the window) and held fixed, so a
  // settled new posture differed from it every remaining minute and scored
  // orient_bonus every minute -- the accumulator grew without bound and the
  // window effectively ended at the first posture change regardless of the
  // sensitivity. required_minutes is raised past anything se_default_cfg would
  // clamp to (deliberately set after cfg_for) so se_evaluate cannot return
  // early on the legitimate turn-over fire and the accumulator at the END of
  // the window is observable: with the fixed reference it was 400 * 25 and
  // fire was true; with the rolling reference the settled posture stops
  // contributing, so acc resets to 0 and the window keeps judging the night. ---
  {
    g_seed = 11;
    fill_rest(N, 30, 0);              // flat VMC: orientation is the only signal
    for (int i = WIN + 3; i < N; i++) {
      g_s[i].orientation = 0x77;      // one turn-over at window minute 3, then still
    }
    SleepEvalCfg c = cfg_for(90, 2);
    c.required_minutes = 25;
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    printf("  settled    fire=%d final_acc=%lu\n", (int)r.fire, (unsigned long)r.acc);
    assert(!r.fire);
    assert(r.acc == 0);
  }

  // --- a SLOW posture drift (one quantisation step every five minutes -- a
  // wrist sagging, not a turn-over) must not be scored as a turn-over either.
  // Cumulatively the drift passes orient_step, which a fixed pre-window
  // reference could only read as "changed", so it fired on the bonus alone even
  // at a percentile whose trigger level the night never came close to (measured:
  // P97, level 573, fired at window minute 11 on acc == 2 * orient_bonus). The
  // rolling reference tracks the drift, so the night is judged on movement --
  // which is what makes the sensitivity setting mean something here. ---
  {
    g_seed = 12;
    fill_skewed(N);
    for (int i = WIN; i < N; i++) {
      int steps = (i - WIN) / 5;
      g_s[i].orientation = (uint8_t)(0x30 | ((3 + steps) & 0x0F));
    }
    SleepEvalCfg c = cfg_for(97, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    printf("  drift@P97  level=%u fire=%d\n", r.trigger_level, (int)r.fire);
    assert(r.trigger_level > 400);    // the night never reaches this on movement
    assert(!r.fire);                  // ... so nothing may fire it
  }

  // --- a restless early stretch must not blind the algorithm. The MEDIAN
  // baseline is what makes this work; a mean would be dragged up. ---
  {
    g_seed = 6;
    fill_rest(N, 30, 10);
    for (int i = 30; i < 90; i++) {
      g_s[i].vmc = 2500;            // an hour of thrashing early on
    }
    for (int i = WIN + 5; i < WIN + 11; i++) {
      g_s[i].vmc = 900;             // the same real stir as the sustained case
    }
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    printf("  restless   base=%u level=%u fire=%d\n",
           r.baseline, r.trigger_level, (int)r.fire);
    assert(r.baseline < 100);       // the median ignored the thrashing hour
    assert(r.fire);
  }

  // --- percentile monotonicity: a LOWER percentile never fires later than a
  // higher one. This is the invariant the "at other sensitivities" block in the
  // Last night summary displays, so a violation would read as a contradiction. ---
  {
    g_seed = 7;
    fill_rest(N, 40, 20);
    for (int i = WIN + 4; i < WIN + 9;  i++) g_s[i].vmc = 300;
    for (int i = WIN + 14; i < WIN + 22; i++) g_s[i].vmc = 1200;
    const uint8_t pcts[4] = { 75, 82, 90, 95 };
    int fired[4];
    for (int k = 0; k < 4; k++) {
      SleepEvalCfg c = cfg_for(pcts[k], 2);
      SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
      fired[k] = r.fire ? r.fired_index : 1 << 30;
      printf("  P%-2u        fire=%d at=%d\n", pcts[k], (int)r.fire, r.fired_index);
    }
    for (int k = 1; k < 4; k++) {
      assert(fired[k] >= fired[k - 1]);
    }
  }

  // --- required_minutes monotonicity the other way: demanding longer can only
  // delay or prevent firing ---
  {
    g_seed = 8;
    fill_rest(N, 40, 15);
    for (int i = WIN + 6; i < WIN + 10; i++) g_s[i].vmc = 800;
    int prev = -1;
    for (uint8_t m = 1; m <= 5; m++) {
      SleepEvalCfg c = cfg_for(90, m);
      SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
      int at = r.fire ? r.fired_index : 1 << 30;
      assert(at >= prev);
      prev = at;
    }
  }

  // --- deep sleep throughout / no window movement -> no fire, deadline handles it
  {
    g_seed = 9;
    fill_rest(N, 25, 4);
    SleepEvalCfg c = cfg_for(82, 1);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    assert(!r.fire);
  }

  // --- invalid data: too few usable minutes -> insufficient_data, never fires ---
  {
    g_seed = 10;
    fill_rest(N, 30, 10);
    for (int i = 0; i < N; i++) {
      g_s[i].is_invalid = true;
    }
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    assert(r.insufficient_data);
    assert(!r.fire);
  }
  {
    // a handful of valid minutes is still not enough to build a distribution
    g_seed = 11;
    fill_rest(N, 30, 10);
    for (int i = 0; i < N; i++) g_s[i].is_invalid = (i > 25);
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    assert(r.insufficient_data && !r.fire);
  }

  // --- degenerate arguments must not crash or fire ---
  {
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(NULL, 0, 0, false, &c);
    assert(!r.fire && r.insufficient_data);
    r = se_evaluate(g_s, 10, 50, false, &c);      // window_start past the end
    assert(!r.fire);
    // Deliberately pinned: "the window hasn't started" and "not enough sleep
    // data" both report insufficient_data=true. This module does not
    // distinguish them (see the comment at the window_start>=count return in
    // se_evaluate) -- the real caller only evaluates once the window has
    // actually begun, so this collapsed case only matters for a malformed
    // call like this one.
    assert(r.insufficient_data);
    r = se_evaluate(g_s, 10, -5, false, &c);      // negative window_start
    assert(!r.fire);
  }

  // --- over-long input is clamped to the LAST SE_MAX_SAMPLES minutes ---
  // A dedicated oversized buffer: passing SE_MAX_SAMPLES + 100 together with the
  // SE_MAX_SAMPLES-sized g_s would make se_evaluate's clamp (which advances the
  // pointer and keeps count at SE_MAX_SAMPLES) read 100 elements past g_s's end.
  {
    SleepEvalCfg c = cfg_for(90, 2);
    static SleepMinute over[SE_MAX_SAMPLES + 100];
    for (int i = 0; i < SE_MAX_SAMPLES + 100; i++) {
      over[i].vmc = (uint16_t)(30 + (i % 7));
      over[i].orientation = 0x33;
      over[i].is_invalid = false;
    }
    SleepEvalResult r = se_evaluate(over, SE_MAX_SAMPLES + 100, 0, false, &c);
    assert(!r.insufficient_data);
    assert(r.baseline > 0);
    // The clamp keeps the tail, so the result must equal evaluating that tail
    // directly. This is the assertion that actually pins the clamp's semantics.
    SleepEvalResult tail = se_evaluate(over + 100, SE_MAX_SAMPLES, 0, false, &c);
    assert(r.baseline == tail.baseline);
    assert(r.trigger_level == tail.trigger_level);
    assert(r.fire == tail.fire);
  }

  // --- se_find_onset ---
  {
    g_seed = 12;
    fill_rest(400, 30, 8);
    for (int i = 0; i < 60; i++) g_s[i].vmc = 3000;   // awake for the first hour
    int onset = se_find_onset(g_s, 400, 200, 10);
    printf("  onset      idx=%d (expect ~60)\n", onset);
    assert(onset >= 55 && onset <= 75);
    // never quiet -> -1
    for (int i = 0; i < 400; i++) g_s[i].vmc = 3000;
    assert(se_find_onset(g_s, 400, 200, 10) == -1);
    // quiet from the very start -> 0
    fill_rest(400, 20, 5);
    assert(se_find_onset(g_s, 400, 200, 10) == 0);
  }

  // --- (a) a realistic right-skewed night: the four sensitivities must give
  // STRICTLY DISTINCT trigger levels. fill_rest's symmetric jitter (used by
  // the "percentile monotonicity" block above) cannot show this -- there the
  // min_margin floor dominates every percentile equally, which is why that
  // block only pins non-strict monotonicity. This is the test that would
  // have caught a distribution-shape (Tukey-fence) trim collapsing every
  // sensitivity onto the same floor: real VMC is strongly right-skewed, and
  // such a trim flags the legitimate upper tail as noise. Nothing here forms
  // a run long enough to be a wake episode, so nothing should be excluded.
  {
    g_seed = 20;
    fill_skewed(N);
    const uint8_t pcts[4] = { 75, 82, 90, 95 };
    uint16_t levels[4];
    for (int k = 0; k < 4; k++) {
      SleepEvalCfg c = cfg_for(pcts[k], 2);
      SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
      levels[k] = r.trigger_level;
      printf("  skewed P%-2u level=%u\n", pcts[k], levels[k]);
    }
    for (int k = 1; k < 4; k++) {
      assert(levels[k] > levels[k - 1]);
    }
  }

  // --- (b) a large sustained stir INSIDE the alarm window must not change
  // trigger_level versus the same night without it. This is the only
  // assertion that pins the history/window population split at all: without
  // it, the window's own movement would enlarge its own threshold as the
  // night's real usage re-evaluates minute by minute. ---
  {
    g_seed = 21;
    fill_skewed(N);
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult base = se_evaluate(g_s, N, WIN, false, &c);

    g_seed = 21;
    fill_skewed(N);
    for (int i = WIN + 5; i < WIN + 25; i++) {
      g_s[i].vmc = 5000;
    }
    SleepEvalResult with_stir = se_evaluate(g_s, N, WIN, false, &c);
    printf("  window-excl base_level=%u with_stir_level=%u\n",
           base.trigger_level, with_stir.trigger_level);
    assert(base.trigger_level == with_stir.trigger_level);
  }

  // --- (c) a THIN pre-window history (only 40 usable minutes after settle)
  // with a genuine stir must still fire: the fallback to the whole recorded
  // stretch must kick in rather than reporting insufficient_data, which
  // would otherwise silently disable the smart alarm on any night where the
  // alarm window happens to start soon after the settle-in period. ---
  {
    g_seed = 22;
    const int WS = 60;        // window starts 60 min in (only 40 usable after settle)
    const int CNT = WS + 30;  // 30-minute window
    fill_rest(CNT, 30, 10);
    for (int i = WS + 6; i < WS + 12; i++) {
      g_s[i].vmc = 900;
    }
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, CNT, WS, false, &c);
    printf("  short-hist insufficient=%d fire=%d\n", (int)r.insufficient_data, (int)r.fire);
    assert(!r.insufficient_data);
    assert(r.fire);

    // The fallback path must be trustworthy across sensitivities, not just at
    // the one percentile happened to be tested above: at P95 the same night's
    // trigger level rises to exactly the stir's own magnitude (900), so it
    // correctly does NOT fire -- still with real data (insufficient_data must
    // stay false), not a collapse back to "not enough data".
    SleepEvalCfg c95 = cfg_for(95, 2);
    SleepEvalResult r95 = se_evaluate(g_s, CNT, WS, false, &c95);
    printf("  short-hist@P95 insufficient=%d level=%u fire=%d\n",
           (int)r95.insufficient_data, r95.trigger_level, (int)r95.fire);
    assert(!r95.insufficient_data);
    assert(!r95.fire);
  }

  // --- a wake episode with a dropped (is_invalid) sample every few minutes
  // must still be excluded as a whole: an invalid minute is BRIDGED in the
  // run scan (it neither extends nor breaks a run), because it is already
  // absent from the ranking population either way. Breaking on it instead
  // would fragment a genuine hour-long thrash into runs individually too
  // short to exclude, reinstating the exact contamination this deviation
  // exists to remove -- and a dropped sample every 10th minute is ordinary
  // firmware behaviour (watch off the wrist, patchy health data), not a
  // contrived input. ---
  {
    g_seed = 6;
    fill_rest(N, 30, 10);
    for (int i = 30; i < 90; i++) {
      g_s[i].vmc = 2500;
      if ((i - 30) % 10 == 9) {
        g_s[i].is_invalid = true;   // one dropped sample every 10 minutes
      }
    }
    for (int i = WIN + 5; i < WIN + 11; i++) {
      g_s[i].vmc = 900;
    }
    SleepEvalCfg c = cfg_for(90, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    printf("  invalid-bridge base=%u level=%u fire=%d\n",
           r.baseline, r.trigger_level, (int)r.fire);
    assert(r.baseline < 100);
    assert(r.fire);
  }

  // --- bridging vs. R alone: the test above does NOT isolate bridging from
  // SE_WAKE_RUN_MINUTES -- marking every 10th minute invalid across a
  // 60-minute stretch fragments it into 9-minute pieces, and 9 already
  // exceeds R=8 on its own, so break-vs-bridge makes no difference at that
  // fragment size (it only pins the historical bug, R=10 + break, together).
  // This case is built so bridging is the ONLY thing that matters: six
  // 4-minute elevated fragments, each individually well below R=8, separated
  // by all-invalid gaps -- so the scan never sees a valid non-elevated
  // sample to reset the run on its own. With bridging, the run spans the
  // whole fragmented region (well over R=8) and is excluded; reverting ONLY
  // the bridging (break on invalid, R left at 8) makes each 4-minute
  // fragment evaluate on its own, individually under R=8, so nothing is
  // excluded and the contamination returns. ---
  {
    g_seed = 6;
    fill_rest(N, 30, 10);
    int idx = 30;
    for (int frag = 0; frag < 6; frag++) {
      for (int k = 0; k < 4; k++) {
        g_s[idx].vmc = 2500;
        g_s[idx].is_invalid = false;
        idx++;
      }
      if (frag < 5) {
        for (int k = 0; k < 3; k++) {
          g_s[idx].is_invalid = true;
          idx++;
        }
      }
    }
    for (int i = WIN + 5; i < WIN + 11; i++) {
      g_s[i].vmc = 900;
    }
    SleepEvalCfg c = cfg_for(95, 2);
    SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
    printf("  invalid-bridge-fragments base=%u level=%u fire=%d\n",
           r.baseline, r.trigger_level, (int)r.fire);
    assert(r.baseline < 100);
    assert(r.fire);
  }

  // --- the four percentiles the Last night summary shows, on a night with
  // background jitter (200) well ABOVE min_margin (25) so the four trigger
  // levels genuinely separate instead of all landing on the baseline+
  // min_margin floor. (jitter=25 -- exactly min_margin -- was tried first and
  // collapsed all four onto trigger_level=82/fired_index=454: the floor
  // dominates whenever the population's own spread is <= min_margin, so a
  // fixture built that way can never fail even if percentile differentiation
  // regresses completely -- the exact "every sensitivity collapses to one
  // answer" failure mode Task 10 fixed. Caught in review; see
  // task-12-report.md.) Lower percentile => fires no later, AND at least one
  // pair must fire STRICTLY earlier, or the test would pass just as happily
  // on a fixture where the four settings never actually differ.
  {
    g_seed = 42;
    fill_rest(N, 45, 200);
    for (int i = WIN + 3;  i < WIN + 8;  i++) g_s[i].vmc = 250;
    for (int i = WIN + 12; i < WIN + 17; i++) g_s[i].vmc = 700;
    for (int i = WIN + 22; i < WIN + 28; i++) g_s[i].vmc = 2000;
    const uint8_t summary_pcts[4] = { 75, 82, 90, 95 };
    int prev = -1;
    bool any_strict = false;
    for (int k = 0; k < 4; k++) {
      SleepEvalCfg c = cfg_for(summary_pcts[k], 2);
      SleepEvalResult r = se_evaluate(g_s, N, WIN, false, &c);
      int at = r.fire ? r.fired_index : 1 << 30;
      printf("  summary P%-2u trig=%u at=%d\n", summary_pcts[k], r.trigger_level,
             r.fire ? r.fired_index : -1);
      assert(at >= prev);
      if (k > 0 && at > prev) {
        any_strict = true;
      }
      prev = at;
    }
    // Not just non-decreasing: genuinely discriminating. A test that only
    // asserts >= would pass identically whether the four sensitivities differ
    // or have collapsed onto one answer -- exactly what jitter=25 above did.
    assert(any_strict);
  }

  printf("test_sleep_eval: all assertions passed\n");
  return 0;
}
