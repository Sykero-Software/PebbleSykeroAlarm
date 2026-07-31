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

  return s;
}

void esc_flatten_ramp(EscParams *p) {
  if (p == NULL) {
    return;
  }
  p->vib_start_ms = p->vib_max_ms;
  p->pulses_start = p->pulses_max;
  p->min_gap_s = p->lead_gap_s;
  // The floor esc_clamp would impose anyway; set it here so esc_full_development_s
  // is already honest if a caller reads it before clamping.
  p->tighten_s = 10;
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

  // Burst duration must stay strictly under the gap that follows it, at every
  // point along the ramp. vib_ms/pulses only ever grow toward vib_max_ms/
  // pulses_max while gap_s only ever shrinks toward min_gap_s (esc_step lerps
  // all three from the same fraction), so checking the worst case -- the
  // longest possible burst against the shortest possible gap -- is enough for
  // the whole ramp. Without this, per-field ranges alone permit e.g.
  // pulses_max=8, vib_max_ms=2000, min_gap_s=1: an 8*2000 + 7*250 = 17750 ms
  // burst repeated every 1000 ms, which piles up in the vibe queue as
  // continuous vibration instead of a burst-and-pause pattern.
  //
  // Shrink the burst side first (fewer pulses, then a shorter longest pulse)
  // rather than widening the gap, since that is the gentler edit and is what
  // the user actually asked for with pulses_max/vib_max_ms; only fall back to
  // widening min_gap_s (and lead_gap_s along with it, so lead_gap_s >=
  // min_gap_s stays true) if the burst side has nothing left to give. The loop
  // is bounded: pulses_max can fall at most 7 times and vib_max_ms at most
  // ~196 times (step 10, from 2000 down to 40) before the burst side alone
  // (1 pulse, 40 ms) is already under any min_gap_s >= 1 s, so the fallback
  // branch is unreachable in practice; the iteration cap is defensive only.
  for (int iter = 0; iter < 256; iter++) {
    uint32_t burst_max_ms = (uint32_t)p->pulses_max * p->vib_max_ms;
    if (p->pulses_max > 1) {
      burst_max_ms += (uint32_t)(p->pulses_max - 1) * ESC_INTRA_PULSE_MS;
    }
    if (burst_max_ms < (uint32_t)p->min_gap_s * 1000) {
      break;
    }
    if (p->pulses_max > 1) {
      p->pulses_max--;
      if (p->pulses_max < p->pulses_start) {
        p->pulses_start = p->pulses_max;
      }
    } else if (p->vib_max_ms > 40) {
      p->vib_max_ms = (p->vib_max_ms > 50) ? (uint16_t)(p->vib_max_ms - 10) : 40;
      if (p->vib_max_ms < p->vib_start_ms) {
        p->vib_start_ms = p->vib_max_ms;
      }
    } else {
      p->min_gap_s++;
      if (p->min_gap_s > p->lead_gap_s) {
        p->lead_gap_s = p->min_gap_s;
      }
    }
  }
}

uint16_t esc_full_development_s(const EscParams *p) {
  if (p == NULL) {
    return 0;
  }
  uint32_t sound_done = (uint32_t)p->sound_after_s + p->sound_ramp_s;
  uint32_t full = p->tighten_s > sound_done ? p->tighten_s : sound_done;
  if (full >= p->cap_s) {
    full = p->cap_s > 1 ? (uint32_t)(p->cap_s - 1) : 0;
  }
  return (uint16_t)full;
}
