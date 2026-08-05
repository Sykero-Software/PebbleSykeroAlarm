// SPDX-License-Identifier: GPL-3.0-only
//
// se_merge_sessions / se_mark_awake, plus a replay of a REAL recorded night
// (tests/fixtures/night_2026_08_05.h) through the production se_evaluate.
//
//   gcc -std=c11 -Wall -I src/c -o /tmp/t
//       tests/test_sleep_sessions.c src/c/sleep_eval.c && /tmp/t
// (no -I tests: the fixture include is relative to this file's own directory)
//
// The night is the one that motivated this code: the firmware ended the sleep
// session at a night waking and started a new one afterwards, which cut the
// ranking population from the whole night down to 130 minutes, and the trip's
// own movement was too intermittent for se_evaluate's run-length wake-episode
// exclusion to notice.
#include "sleep_eval.h"
#include "fixtures/night_2026_08_05.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MIN_S 60u
#define HOUR_S 3600u

static SleepMinute g_s[SE_MAX_SAMPLES];

// se_evaluate over the night from `anchor` with the window at index `win`, as
// hr_read_night would feed it: the anchor is the (merged) sleep onset.
static SleepEvalResult eval_from(int anchor, int win, uint8_t pct, uint8_t mins,
                                 int count) {
  SleepEvalCfg cfg;
  se_default_cfg(&cfg, pct, mins);
  int wi = win - anchor;
  if (wi < 0) {
    wi = 0;
  }
  return se_evaluate(g_s + anchor, count - anchor, wi, false, &cfg);
}

// The whole production chain for this night: merge the reported sessions, mark
// the awake gap, then evaluate from the merged onset. `count` bounds the data
// the watch could actually see at the polled minute.
static SleepEvalResult chain(int count, uint8_t pct, uint8_t mins, int *onset_idx) {
  night_0805_load(g_s);
  NIGHT_0805_SPANS(spans);
  SleepSpan gaps[SE_MAX_SESSIONS];
  int n_gaps = 0;
  uint32_t onset = se_merge_sessions(spans, 2, SE_SESSION_MERGE_GAP_S, gaps,
                                     SE_MAX_SESSIONS, &n_gaps);
  se_mark_awake(g_s, NIGHT_0805_LEN, NIGHT_0805_FIRST_UTC, gaps, n_gaps);
  int anchor = (int)((onset - NIGHT_0805_FIRST_UTC) / MIN_S);
  if (onset_idx) {
    *onset_idx = anchor;
  }
  return eval_from(anchor, night_0805_idx(7, 50), pct, mins, count);
}

int main(void) {
  // ---------------------------------------------------------- se_merge_sessions
  {
    // A night waking: two sessions 13 minutes apart merge into one night, and
    // the 13 minutes are reported as an awake gap.
    const SleepSpan spans[2] = {
      { 10000u + 6u * HOUR_S, 10000u + 9u * HOUR_S },   // newest first
      { 10000u,               10000u + 6u * HOUR_S - 13u * MIN_S },
    };
    SleepSpan gaps[SE_MAX_SESSIONS];
    int n = -1;
    uint32_t onset = se_merge_sessions(spans, 2, SE_SESSION_MERGE_GAP_S, gaps,
                                       SE_MAX_SESSIONS, &n);
    assert(onset == 10000u);
    assert(n == 1);
    assert(gaps[0].start == 10000u + 6u * HOUR_S - 13u * MIN_S);
    assert(gaps[0].end == 10000u + 6u * HOUR_S);
  }
  {
    // A gap wider than the limit is a different sleep (a nap, or last night):
    // the onset stays at the newest session and nothing is reported awake.
    const SleepSpan spans[2] = {
      { 10000u + 8u * HOUR_S, 10000u + 12u * HOUR_S },
      { 10000u,               10000u + 8u * HOUR_S - (SE_SESSION_MERGE_GAP_S + 60u) },
    };
    SleepSpan gaps[SE_MAX_SESSIONS];
    int n = -1;
    uint32_t onset = se_merge_sessions(spans, 2, SE_SESSION_MERGE_GAP_S, gaps,
                                       SE_MAX_SESSIONS, &n);
    assert(onset == 10000u + 8u * HOUR_S);
    assert(n == 0);
  }
  {
    // Exactly at the limit still merges (the boundary is inclusive), one second
    // over does not. Asserted as a pair, because a >= / > slip passes either
    // test alone.
    for (int over = 0; over <= 1; over++) {
      uint32_t gap = SE_SESSION_MERGE_GAP_S + (uint32_t)over;
      const SleepSpan spans[2] = {
        { 100000u,        100000u + HOUR_S },
        { 100000u - gap - HOUR_S, 100000u - gap },
      };
      SleepSpan gaps[SE_MAX_SESSIONS];
      int n = -1;
      uint32_t onset = se_merge_sessions(spans, 2, SE_SESSION_MERGE_GAP_S, gaps,
                                         SE_MAX_SESSIONS, &n);
      if (over == 0) {
        assert(onset == 100000u - gap - HOUR_S && n == 1);
      } else {
        assert(onset == 100000u && n == 0);
      }
    }
  }
  {
    // Three sessions, two short gaps: merged across both, both reported.
    const SleepSpan spans[3] = {
      { 50000u + 7u * HOUR_S, 50000u + 8u * HOUR_S },
      { 50000u + 3u * HOUR_S, 50000u + 7u * HOUR_S - 20u * MIN_S },
      { 50000u,               50000u + 3u * HOUR_S - 10u * MIN_S },
    };
    SleepSpan gaps[SE_MAX_SESSIONS];
    int n = -1;
    uint32_t onset = se_merge_sessions(spans, 3, SE_SESSION_MERGE_GAP_S, gaps,
                                       SE_MAX_SESSIONS, &n);
    assert(onset == 50000u);
    assert(n == 2);
    assert(gaps[0].end == 50000u + 7u * HOUR_S);
    assert(gaps[1].end == 50000u + 3u * HOUR_S);
  }
  {
    // A gaps[] too small must STOP the merge, not merge and silently drop the
    // gap: dropping it would pull the awake minutes into the population, which
    // is the whole defect this module exists to prevent.
    const SleepSpan spans[3] = {
      { 50000u + 7u * HOUR_S, 50000u + 8u * HOUR_S },
      { 50000u + 3u * HOUR_S, 50000u + 7u * HOUR_S - 20u * MIN_S },
      { 50000u,               50000u + 3u * HOUR_S - 10u * MIN_S },
    };
    SleepSpan one[1];
    int n = -1;
    uint32_t onset = se_merge_sessions(spans, 3, SE_SESSION_MERGE_GAP_S, one, 1, &n);
    assert(n == 1);
    assert(onset == 50000u + 3u * HOUR_S);   // merged one gap, then stopped
    // ... and with no gaps[] at all, no merging happens.
    int n0 = -1;
    uint32_t o0 = se_merge_sessions(spans, 3, SE_SESSION_MERGE_GAP_S, NULL, 0, &n0);
    assert(o0 == 50000u + 7u * HOUR_S && n0 == 0);
  }
  {
    // Degenerate inputs.
    SleepSpan gaps[SE_MAX_SESSIONS];
    int n = -1;
    assert(se_merge_sessions(NULL, 3, 60u, gaps, SE_MAX_SESSIONS, &n) == 0 && n == 0);
    const SleepSpan one[1] = { { 7000u, 8000u } };
    assert(se_merge_sessions(one, 1, 60u, gaps, SE_MAX_SESSIONS, &n) == 7000u && n == 0);
    // A malformed (end <= start) older span is ignored, and must not end the
    // night: the good span behind it still merges.
    const SleepSpan bad[3] = {
      { 10000u + 2u * HOUR_S, 10000u + 3u * HOUR_S },
      { 10000u + HOUR_S,      10000u + HOUR_S },              // zero length
      { 10000u,               10000u + 2u * HOUR_S - 5u * MIN_S },
    };
    uint32_t ob = se_merge_sessions(bad, 3, SE_SESSION_MERGE_GAP_S, gaps,
                                    SE_MAX_SESSIONS, &n);
    assert(ob == 10000u && n == 1);
  }
  {
    // Overlapping spans (the firmware reporting a session inside another) are
    // absorbed without inventing a gap.
    const SleepSpan overlap[2] = {
      { 20000u + HOUR_S, 20000u + 4u * HOUR_S },
      { 20000u,          20000u + 2u * HOUR_S },
    };
    SleepSpan gaps[SE_MAX_SESSIONS];
    int n = -1;
    assert(se_merge_sessions(overlap, 2, 60u, gaps, SE_MAX_SESSIONS, &n) == 20000u);
    assert(n == 0);
  }
  {
    // A list that is NOT newest-first is a violated contract, and the failure it
    // would otherwise produce is the dangerous one: taking spans[0] (the oldest)
    // as the onset and absorbing the rest as "overlaps" returns the whole night
    // with NO gaps, i.e. every awake minute inside the population. It must fall
    // back to the newest session alone instead.
    const SleepSpan oldest_first[3] = {
      { 30000u,               30000u + 2u * HOUR_S },
      { 30000u + 3u * HOUR_S, 30000u + 5u * HOUR_S },
      { 30000u + 6u * HOUR_S, 30000u + 7u * HOUR_S },   // newest, listed last
    };
    SleepSpan gaps[SE_MAX_SESSIONS];
    int n = -1;
    uint32_t onset = se_merge_sessions(oldest_first, 3, SE_SESSION_MERGE_GAP_S,
                                       gaps, SE_MAX_SESSIONS, &n);
    assert(onset == 30000u + 6u * HOUR_S);
    assert(n == 0);
  }
  {
    // A malformed NEWEST span must not anchor the walk (it would fix the onset at
    // a bogus start and then measure every gap from it); the first well-formed
    // span does.
    const SleepSpan bad_head[3] = {
      { 40000u + 5u * HOUR_S, 40000u + 5u * HOUR_S },   // zero length, newest
      { 40000u + 4u * HOUR_S, 40000u + 5u * HOUR_S },
      { 40000u,               40000u + 4u * HOUR_S - 10u * MIN_S },
    };
    SleepSpan gaps[SE_MAX_SESSIONS];
    int n = -1;
    uint32_t onset = se_merge_sessions(bad_head, 3, SE_SESSION_MERGE_GAP_S, gaps,
                                       SE_MAX_SESSIONS, &n);
    assert(onset == 40000u);
    assert(n == 1);
    // Every span malformed -> nothing to anchor on at all.
    const SleepSpan all_bad[2] = { { 5u, 5u }, { 3u, 2u } };
    int nb = -1;
    assert(se_merge_sessions(all_bad, 2, 60u, gaps, SE_MAX_SESSIONS, &nb) == 0);
    assert(nb == 0);
  }
  printf("  se_merge_sessions: ok\n");

  // ------------------------------------------------------------- se_mark_awake
  {
    memset(g_s, 0, sizeof g_s);
    for (int i = 0; i < 100; i++) {
      g_s[i].vmc = 500;
      g_s[i].is_invalid = false;
    }
    // Gap covering minutes 10..19 (first_utc + 10 min .. + 20 min).
    const SleepSpan gaps[1] = { { 1000u + 10u * MIN_S, 1000u + 20u * MIN_S } };
    se_mark_awake(g_s, 100, 1000u, gaps, 1);
    for (int i = 0; i < 100; i++) {
      bool want = (i >= 10 && i < 20);
      assert(g_s[i].is_invalid == want);
    }
  }
  {
    // Overlap, not containment: a gap that starts mid-minute marks the minute it
    // starts in (partly awake is not a sleep sample), and a gap entirely before
    // first_utc marks nothing -- with no unsigned underflow.
    memset(g_s, 0, sizeof g_s);
    const SleepSpan gaps[1] = { { 1000u + 10u * MIN_S + 30u, 1000u + 12u * MIN_S + 30u } };
    se_mark_awake(g_s, 100, 1000u, gaps, 1);
    assert(g_s[9].is_invalid == false);
    assert(g_s[10].is_invalid && g_s[11].is_invalid && g_s[12].is_invalid);
    assert(g_s[13].is_invalid == false);

    memset(g_s, 0, sizeof g_s);
    const SleepSpan before[1] = { { 100u, 500u } };   // ends before first_utc=1000
    se_mark_awake(g_s, 100, 1000u, before, 1);
    for (int i = 0; i < 100; i++) {
      assert(!g_s[i].is_invalid);
    }
    // A gap straddling first_utc marks from index 0.
    memset(g_s, 0, sizeof g_s);
    const SleepSpan straddle[1] = { { 100u, 1000u + 2u * MIN_S } };
    se_mark_awake(g_s, 100, 1000u, straddle, 1);
    assert(g_s[0].is_invalid && g_s[1].is_invalid && !g_s[2].is_invalid);
  }
  printf("  se_mark_awake: ok\n");

  // ------------------------------------------- the recorded night of 2026-08-05
  const int WIN = night_0805_idx(7, 50);   // 600, the alarm time = window start
  assert(WIN == 600);
  {
    // What the watch DID last night: anchored on the post-trip session at 05:20,
    // trip minutes still in the population. Recorded in the night summary as
    // base=0 lvl=525, and it rang at 07:56.
    night_0805_load(g_s);
    SleepEvalResult r = eval_from(night_0805_idx(5, 20), WIN, 90, 2, NIGHT_0805_LEN);
    assert(!r.insufficient_data);
    assert(r.trigger_level == 525);
    assert(r.fire && r.fired_index + night_0805_idx(5, 20) == night_0805_idx(7, 56));
    printf("  recorded night, as it happened: lvl=%u fired 07:56\n", r.trigger_level);
  }
  {
    // The trip's movement is TOO INTERMITTENT for the run-length wake-episode
    // exclusion: the longest contiguous run above 4 x baseline + margin inside
    // it is 2 minutes, so se_evaluate cannot exclude it and the trip's spikes
    // (up to 7950) raise the trigger level. This is the measurement that says
    // se_mark_awake is needed and SE_WAKE_RUN_MINUTES is not enough.
    night_0805_load(g_s);
    int anchor = night_0805_idx(23, 0);
    SleepEvalResult with_trip = eval_from(anchor, WIN, 90, 2, NIGHT_0805_LEN);
    // Same anchor, but the trip excluded the way se_mark_awake excludes it.
    night_0805_load(g_s);
    for (int i = night_0805_idx(5, 7); i < night_0805_idx(5, 20); i++) {
      g_s[i].is_invalid = true;
    }
    SleepEvalResult without = eval_from(anchor, WIN, 90, 2, NIGHT_0805_LEN);
    printf("  trip in the population: lvl=%u -> excluded: lvl=%u\n",
           with_trip.trigger_level, without.trigger_level);
    assert(with_trip.trigger_level > without.trigger_level);
    assert(without.trigger_level == 414);
  }
  {
    // The chain: merge the two reported sessions, mark the gap awake, evaluate.
    // The onset comes back at the REAL bedtime, not 05:20, and the trip minutes
    // no longer set the level -- while the ring lands on the same minute as it
    // really did, because it was a genuine turn-over (two consecutive
    // orientation changes, acc = 2 x orient_bonus), not a threshold crossing.
    int onset_idx = -1;
    SleepEvalResult r = chain(NIGHT_0805_LEN, 90, 2, &onset_idx);
    assert(onset_idx == night_0805_idx(23, 12));
    assert(!r.insufficient_data);
    assert(r.trigger_level == 414);
    assert(r.fire && onset_idx + r.fired_index == night_0805_idx(7, 56));
    printf("  merged chain: onset 23:12, lvl=%u, fired 07:56\n", r.trigger_level);
  }
  {
    // The defect that actually costs the user the alarm: WITHOUT merging, a
    // night waking close to the alarm leaves too little history and the smart
    // alarm stands down for the whole window -- under SEMANTICS_RING_FROM that
    // means ringing at the far end of the window, up to window_min LATE.
    // Simulated at the minute the watch would poll, seeing only data up to then.
    night_0805_load(g_s);
    int late_anchor = night_0805_idx(7, 20);
    bool any_fire = false;
    for (int now = WIN; now <= night_0805_idx(8, 20); now++) {
      SleepEvalResult r = eval_from(late_anchor, WIN, 90, 2, now + 1);
      if (r.fire) {
        any_fire = true;
        break;
      }
      assert(r.insufficient_data);
    }
    assert(!any_fire);
    printf("  unmerged 07:20 waking: insufficient data for the whole window\n");

    // Merged, the same waking keeps the whole night in the population, so the
    // window is judged normally.
    int onset_idx = -1;
    SleepEvalResult r = chain(WIN + 6 + 1, 90, 2, &onset_idx);
    assert(!r.insufficient_data && r.fire);
  }

  printf("test_sleep_sessions: all assertions passed\n");
  return 0;
}
