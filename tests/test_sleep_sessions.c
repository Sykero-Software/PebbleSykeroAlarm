// SPDX-License-Identifier: GPL-3.0-only
//
// se_merge_sessions / se_mark_awake, plus a replay of two REAL recorded nights
// (tests/fixtures/night_2026_08_0{5,6}.h) through the production se_evaluate.
//
//   gcc -std=c11 -Wall -I src/c -o /tmp/t
//       tests/test_sleep_sessions.c src/c/sleep_eval.c && /tmp/t
// (no -I tests: the fixture includes are relative to this file's own directory)
//
// 2026-08-05 is the night that motivated this code: the firmware ended the sleep
// session at a night waking and started a new one afterwards, which cut the
// ranking population from the whole night down to 130 minutes, and the trip's
// own movement was too intermittent for se_evaluate's run-length wake-episode
// exclusion to notice. 2026-08-06 is the control: one unbroken session, nothing
// to merge and nothing to exclude, so it pins down the merging path doing
// NOTHING and measures what the anchor alone is worth.
//
// Both nights are asserted against the numbers the WATCH ITSELF computed and
// logged, never against numbers this test found agreeable -- see the fixture
// headers for the transcribed dump lines.
#include "sleep_eval.h"
#include "fixtures/night_2026_08_05.h"
#include "fixtures/night_2026_08_06.h"
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
    // A NESTED span is legitimately newest-first even though its START moves
    // forward, so it must take the absorb path, not the out-of-order fallback.
    // (A start-based ordering test got this wrong: it returned 2000 -- the nested
    // span's start -- instead of merging back to 1000.)
    const SleepSpan nested[2] = { { 1000u, 5000u }, { 2000u, 3000u } };
    SleepSpan gaps[SE_MAX_SESSIONS];
    int n = -1;
    assert(se_merge_sessions(nested, 2, 60u, gaps, SE_MAX_SESSIONS, &n) == 1000u);
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
    // What the exclusion is worth, and what it CANNOT reach. The firmware ended
    // the session at 05:14 while the movement started at 05:07, so se_mark_awake
    // gets 6 minutes: 674 -> 616. The 7 minutes it cannot touch hold 963, 674,
    // 1207 and 3785 -- and the run-length wake-episode exclusion cannot take them
    // either, because this watch's vmc returns to 0 between movements and the
    // longest contiguous elevated run inside the trip is 2 minutes. Excluding the
    // whole trip would give 471, which is the size of the residual and the reason
    // SE_WAKE_RUN_MINUTES stays on the open list.
    const int anchor = night_0805_idx(23, 50);
    night_0805_load(g_s);
    SleepEvalResult with_trip = eval_from(anchor, WIN, 90, 2, NIGHT_0805_LEN);
    night_0805_load(g_s);
    for (int i = night_0805_idx(5, 14); i < night_0805_idx(5, 20); i++) {
      g_s[i].is_invalid = true;
    }
    SleepEvalResult without = eval_from(anchor, WIN, 90, 2, NIGHT_0805_LEN);
    night_0805_load(g_s);
    for (int i = night_0805_idx(5, 7); i < night_0805_idx(5, 20); i++) {
      g_s[i].is_invalid = true;
    }
    SleepEvalResult ideal = eval_from(anchor, WIN, 90, 2, NIGHT_0805_LEN);
    printf("  merged population: lvl=%u -> real gap excluded: lvl=%u"
           " (whole trip would be %u)\n",
           with_trip.trigger_level, without.trigger_level, ideal.trigger_level);
    assert(with_trip.trigger_level == 674);
    assert(without.trigger_level == 616);
    assert(ideal.trigger_level == 471);
  }
  {
    // The chain: merge the two reported sessions, mark the gap awake, evaluate.
    // The onset comes back at the REAL bedtime (23:50) instead of 05:20, so the
    // level is taken from ~470 minutes rather than 130 -- and the ring lands on
    // the same minute as it really did, because it was a genuine turn-over (two
    // consecutive orientation changes, acc = 2 x orient_bonus), not a threshold
    // crossing. lvl=616 and 07:56 are what the watch itself logged for this night
    // (`DBG evalonset off=120 base=0 lvl=616 ... at=08-05 07:56`), so this asserts
    // agreement with hardware, not with the algorithm's own opinion.
    int onset_idx = -1;
    SleepEvalResult r = chain(NIGHT_0805_LEN, 90, 2, &onset_idx);
    assert(onset_idx == night_0805_idx(23, 50));
    assert(!r.insufficient_data);
    assert(r.trigger_level == 616);
    assert(r.fire && onset_idx + r.fired_index == night_0805_idx(7, 56));
    printf("  merged chain: onset 23:50, lvl=%u, fired 07:56 (matches the watch)\n",
           r.trigger_level);
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

  // ------------------------------------------- the recorded night of 2026-08-06
  //
  // The next night, and the control case for the one above: the firmware reported
  // ONE unbroken session, so se_merge_sessions has nothing to merge, se_mark_awake
  // nothing to exclude, and the whole night is the population by default. What it
  // still measures is how much the ANCHOR alone moves the trigger level -- 581
  // from the raw history start (21:50, with an hour of being awake in it) against
  // 354 from the merged onset (23:50) -- and that here it changes nothing: every
  // anchor, and every percentile from 75 to 95, fires at 07:51, one minute into
  // the window, because the sleeper was already stirring when it opened.
  //
  // Every number asserted below is transcribed from the watch's own dump of this
  // night (`Dump last night` -> `pebble logs`, run 2026-08-06 08:07):
  //
  //   DBG cfg smart=1 win=30min sem=2 sens=1 pct=90 mins=2 prof=1
  //   DBG hist first=08-05 21:50 n=618 widx=600
  //   DBG onset merged=08-05 23:50 newest=08-05 23:50 sessions=1 gaps=0
  //   DBG sess0 08-05 23:50 -> 08-06 08:02
  //   DBG eval pct=90/2min base=0 lvl=581 fire=1 fidx=601 at=08-06 07:51 acc=2006 insuf=0
  //   DBG evalonset off=120 base=0 lvl=354 fire=1 fidx=481 at=08-06 07:51 acc=2233
  //   DBG evalalt pct=95 lvl=1192 fire=1 fidx=601 at=08-06 07:51 acc=1395
  //   DBG evalalt pct=90 lvl=581  fire=1 fidx=601 at=08-06 07:51 acc=2006
  //   DBG evalalt pct=82 lvl=25   fire=1 fidx=601 at=08-06 07:51 acc=2562
  //   DBG evalalt pct=75 lvl=25   fire=1 fidx=601 at=08-06 07:51 acc=2562
  //
  // What is deliberately NOT asserted: the LIVE run that morning recorded acc=800
  // at the fire point (2 x orient_bonus, no vmc contribution at all) and no P95
  // fire (`n0 alt 95=--:--`), because the minute in progress returns a PARTIAL
  // vmc -- the live decision saw a fraction of the minute the replay reads back
  // whole. Both are correct; a retrospective replay is not the live decision, and
  // trying to make this test reproduce acc=800 or the live P95 stand-down would be
  // encoding the sampling artefact, not the algorithm.
  const int WIN_0806 = night_0806_idx(7, 50);
  const int FIRE_0806 = night_0806_idx(7, 51);
  assert(WIN_0806 == 600 && FIRE_0806 == 601);
  {
    // One session: nothing to merge, no awake gap, onset = the session's start.
    NIGHT_0806_SPANS(spans);
    SleepSpan gaps[SE_MAX_SESSIONS];
    int n = -1;
    uint32_t onset = se_merge_sessions(spans, 1, SE_SESSION_MERGE_GAP_S, gaps,
                                       SE_MAX_SESSIONS, &n);
    assert(n == 0);
    assert(onset == NIGHT_0806_FIRST_UTC
                        + (uint32_t)night_0806_idx(23, 50) * MIN_S);
  }
  {
    // Anchored at the start of the recorded history (21:50), i.e. the whole
    // 618-minute stretch including the waking hour before bed.
    night_0806_load(g_s);
    SleepEvalResult r = eval_from(0, WIN_0806, 90, 2, NIGHT_0806_LEN);
    assert(!r.insufficient_data);
    assert(r.baseline == 0);
    assert(r.trigger_level == 581);
    assert(r.fire && r.fired_index == FIRE_0806);
    assert(r.acc == 2006);
    printf("  night 08-06, from 21:50: lvl=%u fired 07:51 acc=%lu\n",
           r.trigger_level, (unsigned long)r.acc);
  }
  {
    // The production chain, anchored at the merged onset: the awake hour before
    // bed drops out of the population, which nearly halves the level (581 -> 354)
    // without moving the fire point by a minute.
    night_0806_load(g_s);
    NIGHT_0806_SPANS(spans);
    SleepSpan gaps[SE_MAX_SESSIONS];
    int n_gaps = -1;
    uint32_t onset = se_merge_sessions(spans, 1, SE_SESSION_MERGE_GAP_S, gaps,
                                       SE_MAX_SESSIONS, &n_gaps);
    se_mark_awake(g_s, NIGHT_0806_LEN, NIGHT_0806_FIRST_UTC, gaps, n_gaps);
    int anchor = (int)((onset - NIGHT_0806_FIRST_UTC) / MIN_S);
    assert(anchor == 120 && anchor == night_0806_idx(23, 50));
    SleepEvalResult r = eval_from(anchor, WIN_0806, 90, 2, NIGHT_0806_LEN);
    assert(!r.insufficient_data);
    assert(r.baseline == 0);
    assert(r.trigger_level == 354);
    assert(r.fire && r.fired_index == 481 && anchor + r.fired_index == FIRE_0806);
    assert(r.acc == 2233);
    printf("  night 08-06, from onset 23:50 (off=%d): lvl=%u fired 07:51"
           " (matches the watch)\n", anchor, r.trigger_level);
  }
  {
    // Sensitivity sweep, anchored at 21:50 as the dump's `evalalt` lines are: the
    // level spans 1192 -> 25 and the fire point does not move at all, so this
    // night says nothing about where the sensitivity setting should sit -- worth
    // recording precisely because it is the opposite of a discriminating night.
    static const struct { uint8_t pct; uint16_t lvl; uint32_t acc; } k_alt[4] = {
      { 95, 1192, 1395 }, { 90, 581, 2006 }, { 82, 25, 2562 }, { 75, 25, 2562 },
    };
    for (int k = 0; k < 4; k++) {
      night_0806_load(g_s);
      SleepEvalResult r = eval_from(0, WIN_0806, k_alt[k].pct, 2, NIGHT_0806_LEN);
      assert(r.trigger_level == k_alt[k].lvl);
      assert(r.fire && r.fired_index == FIRE_0806);
      assert(r.acc == k_alt[k].acc);
    }
    printf("  night 08-06, P95..P75: lvl 1192..25, all firing at 07:51\n");
  }

  printf("test_sleep_sessions: all assertions passed\n");
  return 0;
}
