// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdbool.h>
#include <stdint.h>

// 12 h of per-minute samples is the most this module will look at.
#define SE_MAX_SAMPLES 720
// Below this many usable minutes there is no distribution worth taking a
// percentile of, so the smart alarm stands down and the deadline handles it.
// Applies to the pre-window history first; if that alone is too thin, the
// whole recorded stretch (history + window) is retried before giving up --
// a short pre-window gap is not the same thing as "not enough sleep data".
#define SE_MIN_USABLE  60

// One minute of the firmware's own movement history, reduced to what the
// algorithm needs. Deliberately NOT HealthMinuteData: keeping this a plain struct
// is what lets host tests replay recorded real nights.
typedef struct {
  uint16_t vmc;          // Vector Magnitude Counts for the minute
  uint8_t  orientation;  // yaw in the low nibble, pitch in the high nibble
  bool     is_invalid;
} SleepMinute;

typedef struct {
  uint8_t  percentile;        // 70..99; the trigger level is this percentile of
                              // the night's own vmc distribution
  uint8_t  required_minutes;  // 1..5 consecutive contributing minutes
  uint16_t settle_minutes;    // minutes after onset excluded from the baseline
  uint16_t min_margin;        // the trigger level is at least baseline + this
  uint16_t orient_bonus;      // added to the accumulator on a turn-over
  uint8_t  orient_step;       // a nibble must move at least this many of the 16
                              // steps to count as a turn-over (ignores jitter)
} SleepEvalCfg;

typedef struct {
  bool     fire;
  int      fired_index;       // index into samples, -1 when not firing
  uint16_t baseline;          // median vmc of the ranking population (the
                              // history strictly before the alarm window, or
                              // the whole recorded stretch if that history
                              // alone was too thin -- see SE_MIN_USABLE),
                              // taken BEFORE wake-episode exclusion: the
                              // exclusion threshold is derived from this
                              // baseline, so it has to come first
  uint16_t trigger_level;
  uint32_t acc;               // accumulator value at the fire point (or final)
  bool     insufficient_data;
} SleepEvalResult;

// Fill *out with the standard configuration for a given sensitivity.
void se_default_cfg(SleepEvalCfg *out, uint8_t percentile, uint8_t required_minutes);

// ---------------------------------------------------------------- sleep spans
//
// One sleep session as the firmware reports it, in epoch seconds. Deliberately
// uint32_t rather than time_t: that keeps this header free of the
// pebble.h/time.h seam the SDK forces on anything touching struct tm (the SDK
// ships no <time.h> at all), so both the module and its host tests stay
// #ifdef-free.
typedef struct {
  uint32_t start;
  uint32_t end;
} SleepSpan;

// The most sleep sessions one night is expected to produce. A night with more
// arousals than this simply stops merging at the limit.
#define SE_MAX_SESSIONS 8
// Two sessions this far apart or closer are the same night with a wake episode
// between them, not two separate sleeps. Measured need: the firmware ends the
// session at every night waking, so a 05:07-05:19 trip to the toilet made the
// "current session" start at 05:20 and the ranking population 130 minutes
// instead of the whole night (recorded night of 2026-08-05). Merging generously
// is safe BECAUSE the gap minutes are then excluded outright by
// se_mark_awake -- an over-long merge costs nothing, whereas failing to merge
// shrinks the population, and below SE_MIN_USABLE the smart alarm stands down
// altogether.
#define SE_SESSION_MERGE_GAP_S (90u * 60u)

// Merge sleep sessions belonging to the same night and report the awake
// stretches between them.
//
// `spans` is NEWEST-FIRST, exactly as health_service_activities_iterate
// delivers with HealthIterationDirectionPast. Walking from the newest session
// backwards, an older session is taken to belong to the same night when the
// gap to what has been merged so far is at most `max_gap_s`; the first larger
// gap ends the night.
//
// Returns the merged onset (the oldest merged session's start), or 0 when there
// is nothing to merge. Every gap merged ACROSS is appended to gaps[] and
// *n_gaps -- the caller must feed those to se_mark_awake, or the awake minutes
// it just pulled into the population would set the trigger level. Merging
// therefore STOPS when gaps[] is full rather than merging across a gap it
// cannot report.
uint32_t se_merge_sessions(const SleepSpan *spans, int n, uint32_t max_gap_s,
                           SleepSpan *gaps, int max_gaps, int *n_gaps);

// Mark every minute overlapping one of `gaps` invalid, so the ranking
// population excludes it. `first_utc` is the timestamp of samples[0].
//
// This is the reliable way to keep a night waking out of the population: the
// firmware ENDING a sleep session is direct evidence of being awake, whereas
// se_evaluate's own wake-episode exclusion has to infer it from run length and
// cannot see arousals whose movement is intermittent (measured on the recorded
// night of 2026-08-05: the longest contiguous elevated run inside a 12-minute
// trip to the toilet was 2 minutes, far below SE_WAKE_RUN_MINUTES, because the
// per-minute vmc drops back to 0 between steps).
//
// A minute is marked when it OVERLAPS the gap at all, not only when it starts
// inside it: at worst that excludes one extra minute at each edge, which is the
// harmless direction.
void se_mark_awake(SleepMinute *samples, int count, uint32_t first_utc,
                   const SleepSpan *gaps, int n_gaps);

// Decide whether to ring now.
//
// samples[0..count) is the night from sleep onset onwards, oldest first.
// window_start is the first index belonging to the alarm window. is_restful is
// the firmware's current sleep state (HealthActivityRestfulSleep), passed in
// rather than read here so this module stays pure.
//
// The accumulator is leaky by construction: subtracting the trigger level every
// minute is what drains it at rest, so no separate decay constant is needed. A
// single spike, however large, cannot fire — the duration test requires
// required_minutes consecutive contributing minutes.
SleepEvalResult se_evaluate(const SleepMinute *samples, int count, int window_start,
                            bool is_restful, const SleepEvalCfg *cfg);

// Index of the first minute of a quiet stretch at least quiet_minutes long in
// which vmc never exceeds awake_above. Used only when the firmware's own sleep
// session is unavailable (it needs 60 minutes before it reports one). -1 if there
// is no such stretch.
int se_find_onset(const SleepMinute *samples, int count, uint16_t awake_above,
                  int quiet_minutes);
