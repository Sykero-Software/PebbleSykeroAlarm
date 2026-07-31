// SPDX-License-Identifier: GPL-3.0-only
#include <stddef.h>
#include "escalation.h"

static uint32_t prv_lerp_u32(uint32_t a, uint32_t b, uint32_t num, uint32_t den) {
  if (den == 0) {
    return b;
  }
  if (num >= den) {
    return b;
  }
  if (b >= a) {
    return a + (b - a) * num / den;
  }
  return a - (a - b) * num / den;
}

static uint16_t prv_clamp_u16(uint32_t v, uint16_t lo, uint16_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return (uint16_t)v;
}

static uint8_t prv_clamp_u8(uint32_t v, uint8_t lo, uint8_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return (uint8_t)v;
}

void esc_profile(uint8_t profile_id, EscParams *out) {
  if (out == NULL) {
    return;
  }
  switch (profile_id) {
    case ESC_PROFILE_GENTLE:
      *out = (EscParams){
        .lead_gap_s = 45, .min_gap_s = 10, .tighten_s = 600,
        .vib_start_ms = 60, .vib_max_ms = 500,
        .pulses_start = 1, .pulses_max = 3,
        .sound_after_s = 480, .sound_ramp_s = 300,
        .vol_start = 10, .vol_max = 70,
        .cap_s = 1200,
      };
      break;
    case ESC_PROFILE_INSISTENT:
      *out = (EscParams){
        .lead_gap_s = 15, .min_gap_s = 3, .tighten_s = 180,
        .vib_start_ms = 200, .vib_max_ms = 700,
        .pulses_start = 2, .pulses_max = 3,
        .sound_after_s = 60, .sound_ramp_s = 180,
        .vol_start = 30, .vol_max = 100,
        .cap_s = 900,
      };
      break;
    case ESC_PROFILE_NORMAL:
    case ESC_PROFILE_CUSTOM:
    default:
      *out = (EscParams){
        .lead_gap_s = 30, .min_gap_s = 5, .tighten_s = 360,
        .vib_start_ms = 80, .vib_max_ms = 700,
        .pulses_start = 1, .pulses_max = 3,
        .sound_after_s = 300, .sound_ramp_s = 300,
        .vol_start = 15, .vol_max = 100,
        .cap_s = 900,
      };
      break;
  }
}

EscStep esc_step(const EscParams *p, uint32_t elapsed_s, bool sound_available) {
  EscStep s = {0};
  if (p == NULL) {
    return s;
  }
  if (elapsed_s >= p->cap_s) {
    s.over_cap = true;
    s.gap_s = p->min_gap_s;
    s.vib_ms = 0;
    s.pulses = 0;
    s.volume = 0;
    s.light_ms = ESC_LIGHT_MIN_MS;
    return s;
  }

  // Muted: compress the vibration ramp so it is fully developed by the moment
  // sound would otherwise have taken over.
  uint32_t ramp_den = p->tighten_s;
  if (!sound_available && p->sound_after_s > 0 && p->sound_after_s < ramp_den) {
    ramp_den = p->sound_after_s;
  }

  s.gap_s = (uint16_t)prv_lerp_u32(p->lead_gap_s, p->min_gap_s, elapsed_s, ramp_den);
  s.vib_ms = (uint16_t)prv_lerp_u32(p->vib_start_ms, p->vib_max_ms, elapsed_s, ramp_den);
  s.pulses = (uint8_t)prv_lerp_u32(p->pulses_start, p->pulses_max, elapsed_s, ramp_den);

  if (!sound_available || elapsed_s < p->sound_after_s) {
    s.volume = 0;
  } else {
    uint32_t into = elapsed_s - p->sound_after_s;
    s.volume = (uint8_t)prv_lerp_u32(p->vol_start, p->vol_max, into, p->sound_ramp_s);
  }

  // Burst duration: `pulses` pulses with ESC_INTRA_PULSE_MS between them.
  uint32_t burst = (uint32_t)s.pulses * s.vib_ms;
  if (s.pulses > 1) {
    burst += (uint32_t)(s.pulses - 1) * ESC_INTRA_PULSE_MS;
  }
  s.light_ms = burst < ESC_LIGHT_MIN_MS ? ESC_LIGHT_MIN_MS : (uint16_t)burst;
  return s;
}

void esc_clamp(EscParams *p) {
  if (p == NULL) {
    return;
  }
  p->lead_gap_s = prv_clamp_u16(p->lead_gap_s, 2, 120);
  p->min_gap_s = prv_clamp_u16(p->min_gap_s, 1, 60);
  if (p->min_gap_s > p->lead_gap_s) {
    p->min_gap_s = p->lead_gap_s;
  }
  p->vib_start_ms = prv_clamp_u16(p->vib_start_ms, 40, 2000);
  p->vib_max_ms = prv_clamp_u16(p->vib_max_ms, 40, 2000);
  if (p->vib_max_ms < p->vib_start_ms) {
    p->vib_max_ms = p->vib_start_ms;
  }
  p->pulses_start = prv_clamp_u8(p->pulses_start, 1, 8);
  p->pulses_max = prv_clamp_u8(p->pulses_max, 1, 8);
  if (p->pulses_max < p->pulses_start) {
    p->pulses_max = p->pulses_start;
  }
  p->vol_start = prv_clamp_u8(p->vol_start, 1, 100);
  p->vol_max = prv_clamp_u8(p->vol_max, 1, 100);
  if (p->vol_max < p->vol_start) {
    p->vol_max = p->vol_start;
  }
  p->cap_s = prv_clamp_u16(p->cap_s, 120, 3600);

  // The ramps must complete inside the cap, or the escalation could never reach
  // its maximum and the alarm would stay gentle forever. Leave a margin so the
  // maximum is actually played at least once before the cap.
  uint16_t margin = 30;
  uint16_t usable = p->cap_s > margin ? (uint16_t)(p->cap_s - margin) : 1;

  p->sound_after_s = prv_clamp_u16(p->sound_after_s, 0, usable);
  p->tighten_s = prv_clamp_u16(p->tighten_s, 10, usable);
  p->sound_ramp_s = prv_clamp_u16(p->sound_ramp_s, 10, usable);
  if ((uint32_t)p->sound_after_s + p->sound_ramp_s > usable) {
    p->sound_ramp_s = (uint16_t)(usable - p->sound_after_s);
    if (p->sound_ramp_s < 10) {
      // Not enough room for a ramp: start sound earlier instead of shortening
      // the ramp below usefulness.
      p->sound_after_s = usable > 10 ? (uint16_t)(usable - 10) : 0;
      p->sound_ramp_s = 10;
    }
  }
  // The muted path compresses the vibration ramp to sound_after_s, so that must
  // be non-zero for the compression to be meaningful.
  if (p->sound_after_s == 0) {
    p->sound_after_s = 1;
  }
}
