// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Wake-escalation parameters. A profile is just a preset of these; the Clay
// "Custom" profile exposes all of them.
typedef struct {
  uint16_t lead_gap_s;      // silence between bursts at the start
  uint16_t min_gap_s;       // silence between bursts once fully ramped
  uint16_t tighten_s;       // time over which gap/pulse reach their extremes
  uint16_t vib_start_ms;    // pulse length at the start
  uint16_t vib_max_ms;      // pulse length once fully ramped
  uint8_t  pulses_start;    // pulses per burst at the start
  uint8_t  pulses_max;      // pulses per burst once fully ramped
  uint16_t sound_after_s;   // when sound joins (0 = immediately)
  uint16_t sound_ramp_s;    // time from vol_start to vol_max after joining
  uint8_t  vol_start;       // 0..100
  uint8_t  vol_max;         // 0..100
  uint16_t cap_s;           // total ring length; after this, stop making noise
} EscParams;

// What to do for the burst starting at `elapsed_s`.
//
// No backlight field: the app asks the system for an interaction-length backlight
// (light_enable_interaction) rather than holding it for a duration of its own
// choosing, so the hold time is the user's configured backlight timeout and the
// system's own ambient/backlight settings apply. See play_burst in main.c.
typedef struct {
  uint16_t gap_s;      // wait this long after the burst before the next one
  uint16_t vib_ms;     // length of each pulse
  uint8_t  pulses;     // pulses in this burst
  uint8_t  volume;     // 0 == play nothing
  bool     over_cap;   // true once elapsed_s >= cap_s: make no noise at all
} EscStep;

#define ESC_PROFILE_GENTLE    0
#define ESC_PROFILE_NORMAL    1
#define ESC_PROFILE_INSISTENT 2
#define ESC_PROFILE_CUSTOM    3

// Silence between the pulses inside one burst.
#define ESC_INTRA_PULSE_MS    250

// Fill *out with a built-in profile. ESC_PROFILE_CUSTOM yields the Normal preset
// as a starting point (the caller overwrites it from persisted settings).
void esc_profile(uint8_t profile_id, EscParams *out);

// Flatten the VIBRATION escalation: full-strength pulses from the first burst, at
// a constant gap. The sound stage is deliberately untouched — it still joins at
// sound_after_s and still ramps vol_start -> vol_max.
//
// This is the DEFAULT (config `esc_ramp_vib` off). A vibration that starts below
// the threshold that wakes a given sleeper trains them to sleep through the
// channel, which degrades the later strong pulses too — so the alarm must be at
// full strength from the first buzz, and only its *frequency* varies. The user
// chose to keep the ramp available rather than delete it (2026-07-31), hence a
// switch rather than new profile constants.
//
// Collapses tighten_s as well, which is not cosmetic: esc_full_development_s
// feeds the "awake by HH:MM" semantics, so leaving tighten_s at 360-600 s would
// start the ring minutes earlier than needed for a ramp that no longer exists.
//
// Call between esc_profile() and esc_clamp() — esc_clamp must stay the final gate.
void esc_flatten_ramp(EscParams *p);

// The burst to play at `elapsed_s` seconds into the ring.
//
// When `sound_available` is false (speaker muted system-wide, Quiet Time, or a
// platform without a speaker) the sound stage can never contribute, so the
// vibration ramp is compressed to reach its maximum at sound_after_s instead of
// tighten_s and volume stays 0. Without this, a muted user's escalation would
// silently stop improving halfway. (Inert after esc_flatten_ramp: every lerped
// field then has equal endpoints, so compressing the ramp changes nothing.)
EscStep esc_step(const EscParams *p, uint32_t elapsed_s, bool sound_available);

// Force a parameter set into the ranges the Clay page allows and repair any
// inversion (min > max, zero durations). After esc_clamp, esc_step is guaranteed
// to reach maximum vibration — and maximum volume when sound is available —
// strictly before cap_s. This is what stops a Custom profile from being tuned
// into something that never wakes anyone. It also guarantees the longest
// possible burst (pulses_max pulses at vib_max_ms) is strictly shorter than the
// shortest possible gap (min_gap_s), so a Custom profile cannot be tuned into
// back-to-back bursts that pile up in the vibe queue as continuous vibration.
void esc_clamp(EscParams *p);

// The point at which the escalation is fully developed: vibration at maximum AND
// (when audible) volume at maximum. This is what the "awake by HH:MM" time
// semantics subtracts from the alarm time, so that at HH:MM the alarm has just
// reached full strength rather than only just started.
//
// Note this is deliberately NOT cap_s: cap_s is when the alarm gives up, which
// would push the ring absurdly early.
uint16_t esc_full_development_s(const EscParams *p);
