// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "alarm_store.h"

// The "Last night" screen's text, in two blocks.
//
// PURE: no SDK calls, no globals, no time(). Host-tested in
// tests/test_night_text.c, which is the only reason the wording below can be
// changed with any confidence -- this screen is what the sensitivity setting is
// tuned from, so a mislabelled row is a wrong setting, not a cosmetic bug.
//
// `head` is the glance, drawn large: label and value on SEPARATE LINES so a
// value can never break mid-number at 24 pt. `body` is the detail, drawn small:
// levels, the sensitivity table, earlier nights, and the explanation.
//
// Both buffers are always NUL-terminated and never overrun. `head` comes back
// empty when there is nothing to glance at (no nights recorded, or the night
// could not be judged) -- the caller must tolerate an empty head.
void nt_build(const NightSummary *nights, int count,
              char *head, int head_len, char *body, int body_len);

// The phone setting whose percentile this is: "Low" (95), "Medium" (90),
// "High" (82) or "Custom" for anything else.
//
// The mapping is INVERTED from the layman's expectation -- Low is the least
// eager setting and carries the LARGEST number -- which is why the explanation
// text in nt_build says so explicitly. The fourth row the summary stores (75) has
// no named setting at all and must never be given an invented one like
// "Highest"; it is reachable only through the Custom slider.
const char *nt_sens_name(uint8_t percentile);
