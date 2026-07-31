// SPDX-License-Identifier: GPL-3.0-only
#include "escalation.h"
#include <assert.h>
#include <stdio.h>

// The invariant that prevents a Custom profile from never waking anyone:
// vibration must reach its maximum, and (when sound is available) volume must
// reach its maximum, strictly before cap_s. Monotonic in elapsed throughout.
static void check_invariants(const EscParams *p, bool sound, const char *label) {
  uint16_t prev_vib = 0, prev_gap = 0xFFFF;
  uint8_t prev_pulses = 0, prev_vol = 0;
  bool hit_vib_max = false, hit_vol_max = false;

  for (uint32_t t = 0; t < p->cap_s; t++) {
    EscStep s = esc_step(p, t, sound);
    assert(!s.over_cap);
    // monotonic: vibration and volume never decrease, gap never increases
    assert(s.vib_ms >= prev_vib);
    assert(s.pulses >= prev_pulses);
    assert(s.gap_s <= prev_gap);
    assert(s.volume >= prev_vol);
    // bounded
    assert(s.vib_ms >= p->vib_start_ms && s.vib_ms <= p->vib_max_ms);
    assert(s.gap_s >= p->min_gap_s && s.gap_s <= p->lead_gap_s);
    assert(s.pulses >= p->pulses_start && s.pulses <= p->pulses_max);
    assert(s.light_ms >= ESC_LIGHT_MIN_MS);
    if (!sound) {
      assert(s.volume == 0);
    } else {
      assert(s.volume <= p->vol_max);
    }
    if (s.vib_ms == p->vib_max_ms && s.pulses == p->pulses_max) hit_vib_max = true;
    if (sound && s.volume == p->vol_max) hit_vol_max = true;
    prev_vib = s.vib_ms; prev_gap = s.gap_s;
    prev_pulses = s.pulses; prev_vol = s.volume;
  }
  printf("  %-22s sound=%d vib_max_reached=%d vol_max_reached=%d\n",
         label, (int)sound, (int)hit_vib_max, (int)hit_vol_max);
  assert(hit_vib_max);
  if (sound) assert(hit_vol_max);
  // past the cap
  EscStep past = esc_step(p, p->cap_s, sound);
  assert(past.over_cap);
  past = esc_step(p, p->cap_s + 600, sound);
  assert(past.over_cap);
}

int main(void) {
  // --- the three built-in profiles satisfy the invariants, with and without sound
  const char *names[3] = { "gentle", "normal", "insistent" };
  for (uint8_t id = 0; id <= 2; id++) {
    EscParams p;
    esc_profile(id, &p);
    check_invariants(&p, true, names[id]);
    check_invariants(&p, false, names[id]);
  }

  // --- the documented Normal profile values
  {
    EscParams p;
    esc_profile(ESC_PROFILE_NORMAL, &p);
    assert(p.lead_gap_s == 30 && p.min_gap_s == 5 && p.tighten_s == 360);
    assert(p.vib_start_ms == 80 && p.vib_max_ms == 700);
    assert(p.pulses_start == 1 && p.pulses_max == 3);
    assert(p.sound_after_s == 300 && p.vol_start == 15 && p.vol_max == 100);
    assert(p.cap_s == 900);

    // t=0 is the gentlest step and is silent
    EscStep s0 = esc_step(&p, 0, true);
    assert(s0.gap_s == 30 && s0.vib_ms == 80 && s0.pulses == 1 && s0.volume == 0);
    // light is clamped up from the 80 ms burst to the 500 ms minimum
    assert(s0.light_ms == ESC_LIGHT_MIN_MS);

    // just before sound joins: still silent
    assert(esc_step(&p, 299, true).volume == 0);
    // the moment sound joins: exactly vol_start
    assert(esc_step(&p, 300, true).volume == 15);

    // at tighten_s the vibration ramp is complete
    EscStep sT = esc_step(&p, p.tighten_s, true);
    assert(sT.vib_ms == 700 && sT.pulses == 3 && sT.gap_s == 5);
    // and a 3-pulse 700 ms burst gives a light window longer than the minimum
    assert(sT.light_ms > ESC_LIGHT_MIN_MS);
  }

  // --- muted compression: vibration maxes out at sound_after_s, not tighten_s
  {
    EscParams p;
    esc_profile(ESC_PROFILE_NORMAL, &p);
    EscStep muted_at_sound = esc_step(&p, p.sound_after_s, false);
    assert(muted_at_sound.vib_ms == p.vib_max_ms);
    assert(muted_at_sound.pulses == p.pulses_max);
    assert(muted_at_sound.gap_s == p.min_gap_s);
    assert(muted_at_sound.volume == 0);
    // with sound available the same instant is NOT yet at maximum
    EscStep loud_at_sound = esc_step(&p, p.sound_after_s, true);
    assert(loud_at_sound.vib_ms < p.vib_max_ms);
  }

  // --- esc_clamp repairs a hostile Custom profile so the invariants still hold
  {
    EscParams bad = {
      .lead_gap_s = 5000, .min_gap_s = 9000, .tighten_s = 0,
      .vib_start_ms = 20000, .vib_max_ms = 1,
      .pulses_start = 99, .pulses_max = 0,
      .sound_after_s = 60000, .sound_ramp_s = 0,
      .vol_start = 200, .vol_max = 3,
      .cap_s = 1,
    };
    esc_clamp(&bad);
    check_invariants(&bad, true, "clamped-hostile");
    check_invariants(&bad, false, "clamped-hostile");
  }

  // --- a legitimate Custom profile: very patient, sound late
  {
    EscParams custom = {
      .lead_gap_s = 60, .min_gap_s = 10, .tighten_s = 600,
      .vib_start_ms = 60, .vib_max_ms = 600,
      .pulses_start = 1, .pulses_max = 4,
      .sound_after_s = 480, .sound_ramp_s = 240,
      .vol_start = 5, .vol_max = 60,
      .cap_s = 1200,
    };
    esc_clamp(&custom);
    check_invariants(&custom, true, "custom-patient");
  }

  printf("test_escalation: all assertions passed\n");
  return 0;
}
