// SPDX-License-Identifier: GPL-3.0-only
#include "health_read.h"
#include <pebble.h>

#if PBL_IF_HEALTH_ELSE(1, 0)

// Chunked read: HealthMinuteData is 12 bytes, so 60 records is 720 bytes. Static,
// because that does not fit the ~2 KB app stack.
#define HR_CHUNK 60
static HealthMinuteData s_chunk[HR_CHUNK];

// Start of the current sleep session, or 0 if the firmware does not report one.
// The firmware needs 60 minutes of sleep before a session exists, so this is
// unavailable early in the night — se_find_onset covers that case.
static time_t s_onset;

static bool prv_onset_cb(HealthActivity activity, time_t time_start, time_t time_end,
                         void *context) {
  if (activity == HealthActivitySleep) {
    s_onset = time_start;
    return false;   // the iteration is newest-first; the first hit is the current one
  }
  return true;
}

static time_t prv_session_onset(time_t now) {
  s_onset = 0;
  health_service_activities_iterate(HealthActivitySleep, now - 20 * SECONDS_PER_HOUR,
                                    now, HealthIterationDirectionPast,
                                    prv_onset_cb, NULL);
  return s_onset;
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

#endif
