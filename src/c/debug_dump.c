// SPDX-License-Identifier: GPL-3.0-only
#include "debug_dump.h"

#if SA_DEBUG_DUMP

#include "scheduler.h"
#include <pebble.h>

// Lines per tick, and the gap between ticks. The debug link drops lines when a
// hundred of them arrive at once, and a dropped history line is
// indistinguishable from a minute the watch never recorded -- which is exactly
// the thing being measured, so pace it.
#define DBG_LINES_PER_TICK   5
#define DBG_TICK_MS          200
// Samples per packed history line. 8 x "12345.7f " is ~72 chars, comfortably
// inside the firmware's log-line limit.
#define DBG_SAMPLES_PER_LINE 8
// How much history to read around the alarm that rang: enough to cover a whole
// night's sleep before it plus the alarm window after it.
#define DBG_HIST_BEFORE_H    10
#define DBG_HIST_AFTER_MIN   40

static const Alarm *s_alarms;
static int s_count;
static const Config *s_cfg;
static const RunState *s_rs;

static SleepMinute *s_hist;
static int s_hist_max;
static int s_hist_n;
static time_t s_hist_first;
static int s_win_idx;

static time_t s_alarm_t;   // the occurrence that last rang, 0 if none found
static time_t s_win_t;     // its smart-window start

static int s_phase;
static int s_idx;
static AppTimer *s_timer;
// A smart window or a ring is live, so the borrowed scratch buffer is off limits.
static bool s_cycle_live;

// ---------------------------------------------------------------- formatting

static const char *prv_fmt_t(time_t t, char *buf, int n) {
  if (t == 0) {
    snprintf(buf, n, "-");
    return buf;
  }
  struct tm tm = *localtime(&t);
  // The % 100 is not cosmetic: without it gcc-14 cannot bound the field widths
  // and warns that the output may be truncated (-Wformat-truncation). Every
  // field is two digits by construction, so this states that to the compiler
  // rather than silencing a real risk.
  snprintf(buf, n, "%02d-%02d %02d:%02d", (tm.tm_mon + 1) % 100,
           tm.tm_mday % 100, tm.tm_hour % 100, tm.tm_min % 100);
  return buf;
}

// A NightSummary minute-of-day, or "--:--" for the no-fire sentinel.
static const char *prv_fmt_mod(uint16_t mod, char *buf, int n) {
  if (mod == NIGHT_NO_FIRE) {
    snprintf(buf, n, "--:--");
  } else {
    snprintf(buf, n, "%02u:%02u", (unsigned)(mod / 60) % 100u,
             (unsigned)(mod % 60) % 100u);
  }
  return buf;
}

// ---------------------------------------------------------------- history read

#if PBL_IF_HEALTH_ELSE(1, 0)

#define DBG_CHUNK 60
static HealthMinuteData s_chunk[DBG_CHUNK];

// The firmware's own sleep-session start. hr_read_night anchors the ranking
// population there, so a replay that starts anywhere else computes a DIFFERENT
// baseline and trigger level -- it would look like a faithful reproduction and
// silently disagree with what the watch actually decided.
static time_t s_onset;

static bool prv_dbg_onset_cb(HealthActivity activity, time_t time_start,
                             time_t time_end, void *context) {
  if (activity == HealthActivitySleep) {
    s_onset = time_start;
    return false;   // newest-first: the first hit is the current session
  }
  return true;
}

static time_t prv_session_onset(time_t now) {
  s_onset = 0;
  health_service_activities_iterate(HealthActivitySleep, now - 20 * SECONDS_PER_HOUR,
                                    now, HealthIterationDirectionPast,
                                    prv_dbg_onset_cb, NULL);
  return s_onset;
}

// Chunked read of an explicit UTC range. Deliberately a copy of
// hr_read_night's loop rather than a call into it: that function anchors itself
// on the current sleep session and on time(NULL), and this needs a range
// centred on an alarm that already rang. *first_utc receives the timestamp the
// firmware actually started at, which is what makes an index a wall-clock time.
static int prv_read_range(SleepMinute *out, int max, time_t from, time_t to,
                          time_t *first_utc) {
  int n = 0;
  time_t cursor = from;
  *first_utc = 0;
  while (n < max && cursor < to) {
    time_t cs = cursor;
    time_t ce = cursor + (time_t)DBG_CHUNK * SECONDS_PER_MINUTE;
    if (ce > to) {
      ce = to;
    }
    uint32_t got = health_service_get_minute_history(s_chunk, DBG_CHUNK, &cs, &ce);
    if (got == 0) {
      break;
    }
    if (n == 0) {
      *first_utc = cs;
    }
    for (uint32_t i = 0; i < got && n < max; i++) {
      out[n].vmc = s_chunk[i].vmc;
      out[n].orientation = s_chunk[i].orientation;
      out[n].is_invalid = s_chunk[i].is_invalid;
      n++;
    }
    if (ce <= cursor) {
      break;
    }
    cursor = ce;
  }
  return n;
}

#else

static int prv_read_range(SleepMinute *out, int max, time_t from, time_t to,
                          time_t *first_utc) {
  *first_utc = 0;
  return 0;
}

static time_t prv_session_onset(time_t now) { return 0; }

#endif

// ---------------------------------------------------------------- the phases

static void prv_dump_head(void) {
  time_t now = time(NULL);
  struct tm lt = *localtime(&now);
  struct tm gt = *gmtime(&now);
  APP_LOG(APP_LOG_LEVEL_INFO, "DBG ---- dump begin ----");
  APP_LOG(APP_LOG_LEVEL_INFO,
          "DBG now=%lu local=%02d-%02d %02d:%02d:%02d utc=%02d:%02d isdst=%d",
          (unsigned long)now, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min,
          lt.tm_sec, gt.tm_hour, gt.tm_min, lt.tm_isdst);

  // The localtime/mktime round trip, probed RAW -- ac_next_occurrence corrects
  // for a bad one, so this is the only way to see whether the firmware still
  // gets it wrong. Asked for a wall-clock time; if what comes back is not that
  // time, tm_isdst = -1 was misresolved and every occurrence would be off by
  // the difference without the correction.
  uint16_t probe = (s_count > 0) ? s_alarms[0].minute_of_day : 7 * 60 + 50;
  struct tm t2 = *localtime(&now);
  t2.tm_hour = probe / 60;
  t2.tm_min = probe % 60;
  t2.tm_sec = 0;
  t2.tm_isdst = -1;
  time_t naive = mktime(&t2);
  struct tm back = *localtime(&naive);
  int got = back.tm_hour * 60 + back.tm_min;
  APP_LOG(APP_LOG_LEVEL_INFO,
          "DBG tzprobe want=%02u:%02u raw=%02d:%02d isdst_out=%d %s",
          (unsigned)(probe / 60), (unsigned)(probe % 60), back.tm_hour,
          back.tm_min, back.tm_isdst, got == (int)probe ? "OK" : "SKEW");

  APP_LOG(APP_LOG_LEVEL_INFO,
          "DBG cfg smart=%d win=%umin sem=%u sens=%u pct=%u mins=%u prof=%u",
          (int)s_cfg->smart_enabled, (unsigned)s_cfg->smart_window_min,
          (unsigned)s_cfg->time_semantics, (unsigned)s_cfg->sensitivity,
          (unsigned)s_cfg->sens_percentile, (unsigned)s_cfg->sens_minutes,
          (unsigned)s_cfg->wake_profile);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "DBG cfg2 snz=%u/%u off=%u rampvib=%d v=%u",
          (unsigned)s_cfg->snooze_min, (unsigned)s_cfg->snooze_max,
          (unsigned)s_cfg->snooze_ramp_offset_s,
          (int)s_cfg->esc_ramp_vib, (unsigned)s_cfg->version);

  unsigned missed = 0;
  for (int i = 0; i < MAX_ALARMS; i++) {
    if (s_rs->missed[i]) {
      missed |= (1u << i);
    }
  }
  char a[20], b[20], c[20];
  // snz is the one that settles "did it ring twice this morning": a second ring
  // at the alarm time after an early one is a chain of snoozes if snz > 0, and a
  // deadline that was never cancelled if snz == 0.
  APP_LOG(APP_LOG_LEVEL_INFO,
          "DBG rs slot=%d win=%s ring=%s dl=%s snz=%u unavail=%d missed=%02x",
          (int)s_rs->pending_slot,
          prv_fmt_t((time_t)s_rs->window_started_at, a, sizeof a),
          prv_fmt_t((time_t)s_rs->ring_started_at, b, sizeof b),
          prv_fmt_t((time_t)s_rs->deadline_at, c, sizeof c),
          (unsigned)s_rs->snooze_count, (int)s_rs->smart_unavailable, missed);
  char d[20];
  // The double-ring fix's whole state: which occurrence has already rung. A ring
  // that ended early must leave this set, or the same occurrence is armed again.
  APP_LOG(APP_LOG_LEVEL_INFO, "DBG rs2 served_slot=%d served_at=%s",
          (int)s_rs->served_slot,
          prv_fmt_t((time_t)s_rs->served_at, d, sizeof d));
}

// One alarm, with both the occurrence the app computes next and the most recent
// past one -- the past one is the alarm that actually rang last night, so its
// rendered wall clock is a direct check that the app agrees with the user about
// what time the alarm was set for.
static void prv_dump_alarm(int i) {
  time_t now = time(NULL);
  const Alarm *al = &s_alarms[i];
  Alarm probe = *al;
  probe.enabled = true;      // ac_next_occurrence returns 0 for a disabled slot
  probe.skip_next = false;   // ... and skips one for skip_next; want the raw grid
  time_t next = ac_next_occurrence(&probe, now);
  time_t prev = ac_next_occurrence(&probe, now - 25 * SECONDS_PER_HOUR);
  if (prev > now) {
    prev = 0;   // nothing in the last 25 h (weekday pattern skipped it)
  }
  char a[20], b[20];
  APP_LOG(APP_LOG_LEVEL_INFO,
          "DBG a%d en=%d skip=%d %02u:%02u mask=%02x prev=%s next=%s",
          i, (int)al->enabled, (int)al->skip_next,
          (unsigned)(al->minute_of_day / 60), (unsigned)(al->minute_of_day % 60),
          (unsigned)al->weekday_mask,
          prv_fmt_t(prev, a, sizeof a), prv_fmt_t(next, b, sizeof b));
}

static NightSummary s_nights[NIGHT_HISTORY];
static int s_nights_n;

static void prv_dump_night(int k) {
  const NightSummary *n = &s_nights[k];
  char a[8], b[8];
  APP_LOG(APP_LOG_LEVEL_INFO,
          "DBG n%d day=%lu pct=%u onset=%s base=%u lvl=%u",
          k, (unsigned long)n->day_local, (unsigned)n->percentile,
          prv_fmt_mod(n->onset_min, a, sizeof a), (unsigned)n->baseline,
          (unsigned)n->trigger_level);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "DBG n%d fired=%s bydl=%u unavail=%u acc=%lu",
          k, prv_fmt_mod(n->fired_min, b, sizeof b),
          (unsigned)n->fired_by_deadline, (unsigned)n->smart_unavailable,
          (unsigned long)n->acc_at_fire);
  char p[4][8];
  APP_LOG(APP_LOG_LEVEL_INFO, "DBG n%d alt %u=%s %u=%s %u=%s %u=%s", k,
          (unsigned)n->alt_percentile[0], prv_fmt_mod(n->alt_fired_min[0], p[0], 8),
          (unsigned)n->alt_percentile[1], prv_fmt_mod(n->alt_fired_min[1], p[1], 8),
          (unsigned)n->alt_percentile[2], prv_fmt_mod(n->alt_fired_min[2], p[2], 8),
          (unsigned)n->alt_percentile[3], prv_fmt_mod(n->alt_fired_min[3], p[3], 8));
}

// Read the night around the alarm that rang and re-run the real se_evaluate over
// it, at the configured sensitivity and at all four the summary offers. This is
// the answer to "did it fire on real movement or at window start regardless":
// fidx is the window minute it chose, and if every percentile picks the same
// minute the sensitivity setting is inert.
static void prv_dump_eval(void) {
  char a[20], b[20], c[20];
  if (s_alarm_t == 0) {
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "DBG eval: no alarm occurrence in the last 25 h -- history skipped");
    return;
  }
  time_t from = s_alarm_t - DBG_HIST_BEFORE_H * SECONDS_PER_HOUR;
  time_t to = s_alarm_t + DBG_HIST_AFTER_MIN * SECONDS_PER_MINUTE;
  time_t now = time(NULL);
  if (to > now) {
    to = now;
  }
  s_hist_n = prv_read_range(s_hist, s_hist_max, from, to, &s_hist_first);
  APP_LOG(APP_LOG_LEVEL_INFO, "DBG eval alarm=%s win=%s asked=%s",
          prv_fmt_t(s_alarm_t, a, sizeof a), prv_fmt_t(s_win_t, b, sizeof b),
          prv_fmt_t(from, c, sizeof c));
  if (s_hist_n <= 0) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "DBG eval: no minute history returned");
    return;
  }
  s_win_idx = (s_win_t > s_hist_first)
                  ? (int)((s_win_t - s_hist_first) / SECONDS_PER_MINUTE) : 0;
  APP_LOG(APP_LOG_LEVEL_INFO, "DBG hist first=%s n=%d widx=%d",
          prv_fmt_t(s_hist_first, a, sizeof a), s_hist_n, s_win_idx);

  uint8_t pct = 90, mins = 2;
  as_effective_sens(s_cfg, &pct, &mins);
  SleepEvalCfg sc;
  se_default_cfg(&sc, pct, mins);
  // is_restful is not recoverable after the fact (it was the firmware's live
  // state at ring time), so this reports what the algorithm would decide
  // without the deep-sleep veto -- fidx is the number that matters here.
  SleepEvalResult r = se_evaluate(s_hist, s_hist_n, s_win_idx, false, &sc);
  APP_LOG(APP_LOG_LEVEL_INFO,
          "DBG eval pct=%u/%umin base=%u lvl=%u fire=%d fidx=%d at=%s acc=%lu insuf=%d",
          (unsigned)pct, (unsigned)mins, (unsigned)r.baseline,
          (unsigned)r.trigger_level, (int)r.fire, r.fired_index,
          prv_fmt_t(r.fired_index >= 0
                        ? s_hist_first + (time_t)r.fired_index * SECONDS_PER_MINUTE
                        : 0, b, sizeof b),
          (unsigned long)r.acc, (int)r.insufficient_data);

  // The same evaluation ANCHORED AT THE SLEEP SESSION, which is what
  // hr_read_night feeds se_evaluate in the real run. Different population start
  // -> different baseline and trigger level, so this is the line to compare
  // against the night summary, not the one above.
  time_t onset = prv_session_onset(time(NULL));
  APP_LOG(APP_LOG_LEVEL_INFO, "DBG onset session=%s",
          prv_fmt_t(onset, a, sizeof a));
  if (onset > s_hist_first
      && (int)((onset - s_hist_first) / SECONDS_PER_MINUTE) < s_win_idx) {
    int off = (int)((onset - s_hist_first) / SECONDS_PER_MINUTE);
    SleepEvalResult orr = se_evaluate(s_hist + off, s_hist_n - off,
                                      s_win_idx - off, false, &sc);
    APP_LOG(APP_LOG_LEVEL_INFO,
            "DBG evalonset off=%d base=%u lvl=%u fire=%d fidx=%d at=%s acc=%lu",
            off, (unsigned)orr.baseline, (unsigned)orr.trigger_level,
            (int)orr.fire, orr.fired_index,
            prv_fmt_t(orr.fired_index >= 0
                          ? s_hist_first
                                + (time_t)(off + orr.fired_index) * SECONDS_PER_MINUTE
                          : 0, b, sizeof b),
            (unsigned long)orr.acc);
  }

  static const uint8_t k_alt[4] = { 95, 90, 82, 75 };
  for (int k = 0; k < 4; k++) {
    SleepEvalCfg ac;
    se_default_cfg(&ac, k_alt[k], mins);
    SleepEvalResult ar = se_evaluate(s_hist, s_hist_n, s_win_idx, false, &ac);
    APP_LOG(APP_LOG_LEVEL_INFO, "DBG evalalt pct=%u lvl=%u fire=%d fidx=%d at=%s acc=%lu",
            (unsigned)k_alt[k], (unsigned)ar.trigger_level, (int)ar.fire,
            ar.fired_index,
            prv_fmt_t(ar.fired_index >= 0
                          ? s_hist_first + (time_t)ar.fired_index * SECONDS_PER_MINUTE
                          : 0, c, sizeof c),
            (unsigned long)ar.acc);
  }
}

// vmc.orientation pairs, 'x' for an invalid minute. The index is the minute
// offset from `first`, so a gap in the log is visible as a jump in the index.
static void prv_dump_hist_line(int i) {
  char line[112];
  int p = snprintf(line, sizeof line, "DBG h%03d", i);
  for (int k = 0; k < DBG_SAMPLES_PER_LINE && i + k < s_hist_n; k++) {
    if (p >= (int)sizeof line) {
      break;
    }
    const SleepMinute *s = &s_hist[i + k];
    if (s->is_invalid) {
      p += snprintf(line + p, sizeof line - p, " x");
    } else {
      p += snprintf(line + p, sizeof line - p, " %u.%02x", (unsigned)s->vmc,
                    (unsigned)s->orientation);
    }
  }
  APP_LOG(APP_LOG_LEVEL_INFO, "%s", line);
}

// ---------------------------------------------------------------- the pump

static void prv_tick(void *ctx) {
  s_timer = NULL;
  int budget = DBG_LINES_PER_TICK;
  while (budget > 0) {
    switch (s_phase) {
      case 0:
        prv_dump_head();
        s_phase = 1;
        s_idx = 0;
        budget = 0;   // that phase is already five lines
        break;
      case 1:
        if (s_idx >= s_count) {
          s_phase = 2;
          s_idx = 0;
          break;
        }
        prv_dump_alarm(s_idx++);
        budget--;
        break;
      case 2:
        if (s_idx >= s_nights_n) {
          s_phase = 3;
          s_idx = 0;
          break;
        }
        prv_dump_night(s_idx++);
        budget -= 3;
        break;
      case 3:
        // The history read borrows main's s_night, which the smart-window poll
        // owns while a window is open and record_night reads at ring time. The
        // SUMMARIES above never touch it, so only this phase is skipped -- an
        // earlier version declined the whole dump and threw away exactly the
        // runstate line that would have explained a live cycle.
        if (s_cycle_live) {
          APP_LOG(APP_LOG_LEVEL_WARNING,
                  "DBG eval skipped: a cycle is live, s_night is in use");
        } else {
          prv_dump_eval();
        }
        s_phase = 4;
        s_idx = 0;
        budget = 0;
        break;
      case 4:
        if (s_idx >= s_hist_n) {
          s_phase = 5;
          break;
        }
        prv_dump_hist_line(s_idx);
        s_idx += DBG_SAMPLES_PER_LINE;
        budget--;
        break;
      default:
        APP_LOG(APP_LOG_LEVEL_INFO, "DBG ---- dump end ----");
        return;
    }
  }
  s_timer = app_timer_register(DBG_TICK_MS, prv_tick, NULL);
}

void dbg_dump(const Alarm *alarms, int count, const Config *cfg,
              const RunState *rs, SleepMinute *scratch, int scratch_len) {
  if (alarms == NULL || cfg == NULL || rs == NULL || scratch == NULL
      || scratch_len < 60) {
    return;
  }
  s_alarms = alarms;
  s_count = count;
  s_cfg = cfg;
  s_rs = rs;
  s_hist = scratch;
  s_hist_max = scratch_len;
  s_hist_n = 0;
  s_hist_first = 0;
  s_win_idx = 0;
  s_phase = 0;
  s_idx = 0;
  s_nights_n = as_load_nights(s_nights, NIGHT_HISTORY);
  s_cycle_live = (rs->window_started_at != 0 || rs->ring_started_at != 0);

  // Which alarm rang most recently: the latest occurrence in the last 25 h.
  // ENABLED slots win outright -- a disabled slot's grid position still has
  // "occurrences" it never rang (the seeded 06:15 picked itself on the
  // emulator), and reading the night around a time nothing happened at wastes
  // the one dump the user has to trigger by hand. A disabled slot is only
  // considered when no enabled one has fired, because that is the one case
  // where it is the likely answer: a one-time alarm is disabled BY ringing.
  time_t now = time(NULL);
  time_t best_en = 0, best_any = 0;
  for (int i = 0; i < count; i++) {
    Alarm probe = alarms[i];
    bool was_enabled = probe.enabled;
    probe.enabled = true;
    probe.skip_next = false;
    // ac_next_occurrence returns the FIRST occurrence after its base, so for a
    // daily alarm a base of now-25h yields YESTERDAY's -- which picked the wrong
    // alarm on the first real dump. Walk forward and keep the last one that is
    // still in the past.
    time_t t = 0;
    time_t cur = now - 25 * SECONDS_PER_HOUR;
    for (int step = 0; step < 40; step++) {
      time_t nx = ac_next_occurrence(&probe, cur);
      if (nx == 0 || nx > now) {
        break;
      }
      t = nx;
      cur = nx;
    }
    if (t == 0) {
      continue;
    }
    if (t > best_any) {
      best_any = t;
    }
    if (was_enabled && t > best_en) {
      best_en = t;
    }
  }
  s_alarm_t = (best_en != 0) ? best_en : best_any;
  s_win_t = (s_alarm_t != 0) ? sc_window_start(cfg, s_alarm_t) : 0;

  if (s_timer) {
    app_timer_cancel(s_timer);
  }
  // Late enough that the log link is up and the launch sequence is not delayed.
  s_timer = app_timer_register(1200, prv_tick, NULL);
}

#endif  // SA_DEBUG_DUMP
