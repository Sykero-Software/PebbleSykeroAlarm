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

  printf("test_sleep_eval: all assertions passed\n");
  return 0;
}
