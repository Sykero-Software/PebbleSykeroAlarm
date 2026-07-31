// SPDX-License-Identifier: GPL-3.0-only
#include "scheduler.h"
#include <pebble.h>

time_t sc_schedule(time_t when, WakeCookie cookie) {
  if (when <= time(NULL)) {
    APP_LOG(APP_LOG_LEVEL_INFO, "wakeup %d declined: requested time already past",
            (int)cookie);
    return 0;
  }
  // +1, -1, +2, -2 minutes: PebbleOS rejects a wakeup within one minute of any
  // already-scheduled one (system-wide, so another app can block our slot).
  static const int k_shift_min[] = { 0, 1, -1, 2, -2 };
  for (unsigned i = 0; i < ARRAY_LENGTH(k_shift_min); i++) {
    time_t t = when + (time_t)k_shift_min[i] * SECONDS_PER_MINUTE;
    if (t <= time(NULL)) {
      continue;
    }
    WakeupId id = wakeup_schedule(t, (int32_t)cookie, false);
    if (id >= 0) {
      if (k_shift_min[i] != 0) {
        APP_LOG(APP_LOG_LEVEL_INFO, "wakeup %d shifted %+d min (E_RANGE)",
                (int)cookie, k_shift_min[i]);
      }
      return t;
    }
    if (id != E_RANGE) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "wakeup %d failed: %d", (int)cookie, (int)id);
      if (id == E_OUT_OF_RESOURCES) {
        return 0;   // no slots left; shifting cannot help
      }
    }
  }
  APP_LOG(APP_LOG_LEVEL_ERROR, "wakeup %d could not be placed", (int)cookie);
  return 0;
}

void sc_cancel_all(void) {
  wakeup_cancel_all();
}

time_t sc_ring_deadline(const Config *cfg, time_t alarm_time) {
  if (cfg->time_semantics != SEMANTICS_AWAKE_BY) {
    return alarm_time;
  }
  EscParams e;
  as_effective_esc(cfg, &e);
  return alarm_time - (time_t)esc_full_development_s(&e);
}

time_t sc_window_start(const Config *cfg, time_t alarm_time) {
  time_t ring = sc_ring_deadline(cfg, alarm_time);
#if PBL_IF_HEALTH_ELSE(1, 0)
  if (cfg->smart_enabled && cfg->smart_window_min > 0) {
    return ring - (time_t)cfg->smart_window_min * SECONDS_PER_MINUTE;
  }
#endif
  return ring;
}

void sc_arm_reentry(time_t now) {
  sc_schedule(now + SC_REENTRY_GAP_S, WC_REENTRY);
}

// 03:00 local tomorrow (or today if it is still ahead).
static time_t prv_next_dst_check(time_t now) {
  struct tm tm = *localtime(&now);
  tm.tm_hour = 3;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  tm.tm_isdst = -1;
  time_t t = mktime(&tm);
  if (t <= now) {
    tm = *localtime(&now);
    tm.tm_mday += 1;
    tm.tm_hour = 3;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    t = mktime(&tm);
  }
  return t;
}

bool sc_rearm(const Alarm *alarms, int count, const Config *cfg,
              const RunState *rs, time_t now) {
  // Cancel-then-reschedule is the only reliable approach: WakeupIds are lost when
  // the app is killed, so there is nothing to selectively cancel after a relaunch.
  sc_cancel_all();
  bool ok = true;

  time_t alarm_when = 0;
  int slot = ac_next_alarm(alarms, count, now, &alarm_when);

  // PRIORITY 1 — the hard deadline. Scheduled before anything else so that if
  // slots or E_RANGE retries run out, the thing that still exists is the alarm.
  if (slot >= 0) {
    time_t ring = sc_ring_deadline(cfg, alarm_when);
    if (ring <= now) {
      // SEMANTICS_AWAKE_BY subtracts the full escalation development time from
      // the alarm time, so an alarm closer than that (e.g. the built-in
      // 2-minute Test alarm) computes a deadline already in the past -- and
      // until this fix, that silently dropped the wakeup here (this branch
      // did not log, and the ring was simply never scheduled: "awake by"
      // + an imminent alarm = the alarm never rings). Falling back to the
      // alarm time itself keeps the alarm's basic promise (it rings) even
      // when the awake-by lead time does not fit before it. alarm_when is
      // always > now here (ac_next_alarm only ever returns future
      // occurrences), so this fallback is always schedulable.
      //
      // Accepted residual race, not eliminated: ac_next_alarm/prv_nth_occurrence
      // only guarantees alarm_when > the `now` THIS function was called with. An
      // occurrence as little as a second out could still tick into the past by
      // the time sc_schedule below re-reads time(NULL) itself. We do not chase
      // that race here -- sc_schedule's own already-past guard now LOGS the
      // decline instead of failing silently (see its own comment), which is
      // what makes this an acceptable, visible edge case rather than a repeat
      // of the bug this fallback exists to fix.
      APP_LOG(APP_LOG_LEVEL_INFO,
              "ring deadline for slot %d already past (ring=%lu <= now=%lu) -- "
              "falling back to ringing at the alarm time",
              slot, (unsigned long)ring, (unsigned long)now);
      ring = alarm_when;
    }
    sc_schedule(ring, WC_DEADLINE);
  }

  // PRIORITY 2 — snooze expiry, if one is pending. Ahead of the window because a
  // pending snooze is a promise already made to the user. This is the one
  // wakeup this function treats as critical to the caller: ring_snooze_now
  // (Task 7) relies on sc_rearm to actually schedule it before exiting to the
  // watchface, so failure here is reported back via the return value.
  //
  // While a snooze is pending, ring_started_at holds the snooze EXPIRY, not the
  // ring start: ring_snooze_now (Task 7) moves it forward so ring_elapsed_s()
  // resumes at the right point in the ramp. So it is the wakeup time as-is —
  // adding snooze_min again here would double the snooze the user asked for.
  // A value in the past means the snooze already expired and the ring is running,
  // which start_ring's own keep-alive wakeup covers.
  if (rs->ring_started_at != 0 && rs->snooze_count > 0) {
    time_t snooze_until = (time_t)rs->ring_started_at;
    if (snooze_until > now) {
      if (sc_schedule(snooze_until, WC_SNOOZE) == 0) {
        ok = false;
      }
    }
  }

  // PRIORITY 3 — the smart window opening.
  if (slot >= 0) {
    time_t win = sc_window_start(cfg, alarm_when);
    time_t ring = sc_ring_deadline(cfg, alarm_when);
    if (win > now && win < ring) {
      sc_schedule(win, WC_WINDOW);
    }
  }

  // PRIORITY 4 — the daily clock-shift check.
  if (cfg->dst_check) {
    sc_schedule(prv_next_dst_check(now), WC_DST);
  }

  // WC_REENTRY is deliberately NOT armed here: it is only wanted while a window
  // is actually open, and it is re-armed on every fire (see sc_arm_reentry).
  if (rs->window_started_at != 0) {
    sc_arm_reentry(now);
  }

  return ok;
}
