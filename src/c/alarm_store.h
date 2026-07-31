// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "alarm_calc.h"
#include "escalation.h"
#include <stdbool.h>
#include <stdint.h>

// --- persist keys. Append only; never renumber. ---
#define PK_ALARMS    1
#define PK_CONFIG    2
#define PK_RUNSTATE  3
#define PK_NIGHTS    4

// Struct versions, bumped when a layout changes so a stale blob is discarded
// rather than misread.
#define CONFIG_VERSION    1
#define RUNSTATE_VERSION  1
#define NIGHTS_VERSION    1

#define SENS_LOW      0
#define SENS_MEDIUM   1
#define SENS_HIGH     2
#define SENS_CUSTOM   3

#define SEMANTICS_RING_STARTS  0
#define SEMANTICS_AWAKE_BY     1

#define STOP_LONG_PRESS  0
#define STOP_TWO_TAP     1
#define STOP_THREE_TAP   2

// Everything the Clay page can set.
typedef struct {
  uint8_t  version;
  bool     smart_enabled;
  uint8_t  smart_window_min;       // 10..60
  uint8_t  time_semantics;         // SEMANTICS_*
  uint8_t  sensitivity;            // SENS_*
  uint8_t  sens_percentile;        // 70..99, used when sensitivity == SENS_CUSTOM
  uint8_t  sens_minutes;           // 1..5,   used when sensitivity == SENS_CUSTOM
  uint8_t  wake_profile;           // ESC_PROFILE_*
  EscParams esc;                   // used when wake_profile == ESC_PROFILE_CUSTOM
  uint8_t  snooze_min;             // 1..30
  uint8_t  snooze_max;             // 0..20, 0 == unlimited
  uint16_t snooze_ramp_offset_s;   // seconds added to `elapsed` per snooze
  uint8_t  stop_gesture;           // STOP_*
  bool     light_pulse;
  bool     dst_check;
  uint8_t  idle_exit_sec;          // 0 == off
} Config;

// Live state that must survive the app being killed or the watch rebooting.
typedef struct {
  uint8_t  version;
  int8_t   pending_slot;        // alarm index currently armed/ringing, -1 none
  uint32_t window_started_at;   // 0 when no smart window is open
  uint32_t ring_started_at;     // 0 when not ringing
  uint32_t deadline_at;         // the hard alarm time for pending_slot
  uint8_t  snooze_count;
  bool     smart_unavailable;   // set when the last window had no usable data
  bool     missed[MAX_ALARMS];  // hit the cap with nobody dismissing it
} RunState;

#define NIGHT_HISTORY    7
#define NIGHT_ALT_COUNT  4
// Sentinel for "did not / would not have fired before the deadline".
#define NIGHT_NO_FIRE    0xFFFF

// One night's outcome, plus what the alternative sensitivities would have done.
// The alternatives are computed once at ring time and stored, so the summary
// screen never has to re-read minute history that may since have been trimmed.
typedef struct {
  uint32_t day_local;                          // days since 1970 in local time
  uint16_t onset_min;                          // minute of day, NIGHT_NO_FIRE if unknown
  uint16_t baseline;
  uint16_t trigger_level;
  uint16_t fired_min;                          // NIGHT_NO_FIRE == deadline fired
  uint32_t acc_at_fire;
  uint8_t  percentile;                         // the one actually in use
  uint8_t  smart_unavailable;                  // 0/1
  uint8_t  alt_percentile[NIGHT_ALT_COUNT];
  uint16_t alt_fired_min[NIGHT_ALT_COUNT];
} NightSummary;

void as_load_alarms(Alarm *out, int *count);
void as_save_alarms(const Alarm *alarms, int count);

// Loads the persisted config, or fills *out with the documented defaults when
// nothing is stored or the stored version does not match.
void as_load_config(Config *out);
void as_save_config(const Config *cfg);

void as_load_runstate(RunState *out);
void as_save_runstate(const RunState *rs);

// Newest-first ring buffer of the last NIGHT_HISTORY nights.
void as_push_night(const NightSummary *n);
int  as_load_nights(NightSummary *out, int max);

// Resolve the profile/sensitivity indirection: with a built-in profile these
// return the preset, with Custom they return the stored values (clamped).
void as_effective_esc(const Config *cfg, EscParams *out);
void as_effective_sens(const Config *cfg, uint8_t *percentile, uint8_t *minutes);
