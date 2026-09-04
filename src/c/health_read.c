// SPDX-License-Identifier: GPL-3.0-only
#include "health_read.h"
#include <pebble.h>

#if PBL_IF_HEALTH_ELSE(1, 0)

// Chunked read: HealthMinuteData is 12 bytes, so 60 records is 720 bytes. Static,
// because that does not fit the ~2 KB app stack.
#define HR_CHUNK 60
static HealthMinuteData s_chunk[HR_CHUNK];

// Every sleep session the firmware reports for the last 20 h, newest first, and
// the awake stretches between the ones that belong to the same night.
//
// It is NOT enough to take the newest session's start: the firmware ENDS the
// session at every night waking, so a trip to the toilet at 05:07 makes the
// "current session" start at 05:20 and the ranking population the ~2 h since,
// instead of the whole night (measured on the recorded night of 2026-08-05,
// which tests/fixtures/night_2026_08_05.h replays). The closer to the alarm
// that waking is, the thinner the population, and below SE_MIN_USABLE the smart
// alarm stands down for the entire window.
static SleepSpan s_spans[SE_MAX_SESSIONS];
static int s_nspans;
static SleepSpan s_gaps[SE_MAX_SESSIONS];
static int s_ngaps;

static bool prv_span_cb(HealthActivity activity, time_t time_start, time_t time_end,
                        void *context) {
  if (activity == HealthActivitySleep && s_nspans < SE_MAX_SESSIONS) {
    s_spans[s_nspans].start = (uint32_t)time_start;
    s_spans[s_nspans].end = (uint32_t)time_end;
    s_nspans++;
  }
  return s_nspans < SE_MAX_SESSIONS;   // stop iterating once the array is full
}

// Onset of the night, sessions merged across short wakings, or 0 if the
// firmware reports no session at all. It needs 60 minutes of sleep before a
// session exists, so this is unavailable early in the night — se_find_onset
// covers that case.
static time_t prv_session_onset(time_t now) {
  s_nspans = 0;
  s_ngaps = 0;
  health_service_activities_iterate(HealthActivitySleep, now - 20 * SECONDS_PER_HOUR,
                                    now, HealthIterationDirectionPast,
                                    prv_span_cb, NULL);
  uint32_t onset = se_merge_sessions(s_spans, s_nspans, SE_SESSION_MERGE_GAP_S,
                                     s_gaps, SE_MAX_SESSIONS, &s_ngaps);
  if (s_nspans > 1) {
    APP_LOG(APP_LOG_LEVEL_INFO, "SLEEP %d sessions, merged onset=%lu, %d awake gap(s)",
            s_nspans, (unsigned long)onset, s_ngaps);
  }
  return (time_t)onset;
}

#ifndef SCREENSHOT_FIXTURES
// TimeStyle's own accessibility check (src/c/util.c is_health_metric_accessible),
// deliberately identical: the same today-window, the same mask bit. A metric
// the watchface would draw must be one this screen draws too.
static int prv_sum_today(HealthMetric metric) {
  time_t start = time_start_of_today();
  time_t end = time(NULL);
  HealthServiceAccessibilityMask mask =
      health_service_metric_accessible(metric, start, end);
  if (!(mask & HealthServiceAccessibilityMaskAvailable)) {
    return 0;
  }
  return (int)health_service_sum_today(metric);
}
#endif   // the fixture build below never calls it, and an unused static warns

void hr_sleep_totals(int *deep_s, int *total_s) {
#ifdef SCREENSHOT_FIXTURES
  // Appstore screenshots only, and ONLY under the SCREENSHOT_FIXTURES env flag
  // (see wscript): a headless emulator has recorded no sleep at all, so every
  // screen carrying this line would be shot with the line hidden. 1.75 h
  // restful of 6.5 h -- deliberately the same demo night TimeStyle's own sleep
  // widget fixture uses, so the two products' store pages agree. `pebble
  // publish` runs its own build with no such env var, so this cannot reach a
  // released bundle.
  *deep_s = 6300;
  *total_s = 23400;
#else
  *total_s = prv_sum_today(HealthMetricSleepSeconds);
  *deep_s = prv_sum_today(HealthMetricSleepRestfulSeconds);
#endif
}

HistoryRead hr_read_night(SleepMinute *out, int max, time_t window_start_utc) {
  HistoryRead hr = { .count = 0, .window_start = -1, .is_restful = false,
                     .available = false, .first_utc = 0 };
  if (out == NULL || max <= 0) {
    return hr;
  }
  time_t now = time(NULL);

  HealthServiceAccessibilityMask acc =
      health_service_metric_accessible(HealthMetricSleepSeconds,
                                       now - SECONDS_PER_HOUR, now);
  if (acc == HealthServiceAccessibilityMaskNoPermission
      || acc == HealthServiceAccessibilityMaskNotSupported) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "health not accessible (mask=%d)", (int)acc);
    return hr;
  }

  time_t onset = prv_session_onset(now);
  time_t start = onset != 0 ? onset : now - 8 * SECONDS_PER_HOUR;
  if (now - start > (time_t)max * SECONDS_PER_MINUTE) {
    start = now - (time_t)max * SECONDS_PER_MINUTE;
  }

  int n = 0;
  time_t cursor = start;
  while (n < max && cursor < now) {
    time_t chunk_start = cursor;
    time_t chunk_end = cursor + (time_t)HR_CHUNK * SECONDS_PER_MINUTE;
    if (chunk_end > now) {
      chunk_end = now;
    }
    uint32_t got = health_service_get_minute_history(s_chunk, HR_CHUNK,
                                                    &chunk_start, &chunk_end);
    if (got == 0) {
      break;
    }
    if (n == 0) {
      hr.first_utc = chunk_start;
    }
    for (uint32_t i = 0; i < got && n < max; i++) {
      out[n].vmc = s_chunk[i].vmc;
      out[n].orientation = s_chunk[i].orientation;
      out[n].is_invalid = s_chunk[i].is_invalid;
      n++;
    }
    // chunk_end is updated by the call to the end of what was actually returned.
    if (chunk_end <= cursor) {
      break;   // no forward progress; stop rather than loop
    }
    cursor = chunk_end;
  }

  hr.count = n;
  if (n == 0) {
    return hr;
  }
  hr.available = true;

  // Drop the minutes the firmware itself says were spent awake (the gaps merged
  // across above), BEFORE anything reads them. se_evaluate's own wake-episode
  // exclusion cannot catch these: it infers arousals from run length, and on
  // this watch the per-minute vmc falls back to 0 between movements, so a trip to
  // the toilet is a handful of 1-2 minute runs, far below SE_WAKE_RUN_MINUTES.
  // Left in, they sit in the night's top decile and raise the trigger level
  // (measured on the recorded night: 674 -> 616).
  //
  // Only the minutes the firmware calls awake, which is LESS than the arousal:
  // on that night the session ended at 05:14 though the movement began at 05:07,
  // so four sizeable spikes stayed in the population. Excluding the whole trip
  // would have given 471. The residual is on the backlog, not silently ignored.
  se_mark_awake(out, hr.count, (uint32_t)hr.first_utc, s_gaps, s_ngaps);

  if (window_start_utc > hr.first_utc) {
    int idx = (int)((window_start_utc - hr.first_utc) / SECONDS_PER_MINUTE);
    hr.window_start = idx < n ? idx : n - 1;
  } else {
    hr.window_start = 0;
  }

  HealthActivityMask cur = health_service_peek_current_activities();
  hr.is_restful = (cur & HealthActivityRestfulSleep) != 0;

  // If the firmware had no session, fall back to detecting onset ourselves and
  // drop everything before it, so the baseline is not polluted by being awake.
  if (onset == 0) {
    int own = se_find_onset(out, n, 200, 10);
    if (own > 0 && own < n) {
      for (int i = 0; i + own < n; i++) {
        out[i] = out[i + own];
      }
      hr.count = n - own;
      hr.first_utc += (time_t)own * SECONDS_PER_MINUTE;
      hr.window_start = hr.window_start > own ? hr.window_start - own : 0;
    }
  }
  return hr;
}

#else  // no health on this platform (aplite)

HistoryRead hr_read_night(SleepMinute *out, int max, time_t window_start_utc) {
  HistoryRead hr = { .count = 0, .window_start = -1, .is_restful = false,
                     .available = false, .first_utc = 0 };
  return hr;
}

void hr_sleep_totals(int *deep_s, int *total_s) {
  *deep_s = 0;
  *total_s = 0;
}

#endif
