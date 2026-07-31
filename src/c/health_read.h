// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "sleep_eval.h"
#include <time.h>

typedef struct {
  int    count;
  int    window_start;
  bool   is_restful;
  bool   available;
  time_t first_utc;
} HistoryRead;

// Fill out[0..max) with per-minute movement history from sleep onset (or 8 h back,
// whichever is later) up to now, and report where the alarm window begins within
// it.
//
// Reads the firmware's own per-minute VMC via health_service_get_minute_history.
// No accelerometer subscription: the firmware already samples at 25 Hz for
// activity tracking, so this data costs no extra sensor power, and because it is
// not held in our RAM a killed and relaunched app loses nothing.
//
// Returns available=false when health cannot be used at all — activity tracking
// switched off, or a platform without health.
HistoryRead hr_read_night(SleepMinute *out, int max, time_t window_start_utc);
