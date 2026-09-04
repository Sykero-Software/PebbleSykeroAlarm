// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The night's sleep as one short line: "Slept 1.7/6.5 h" -- restful (deep)
// first, total second, in truncated tenths of an hour.
//
// PURE: no SDK calls, no globals, no time(). Host-tested in
// tests/test_sleep_text.c.
//
// The numbers are the same two figures TimeStyle's sleep widget draws
// (health_service_sum_today of HealthMetricSleepRestfulSeconds and
// HealthMetricSleepSeconds), and the tenths are truncated the same way its
// sleep_format_decimal truncates them, so the watchface and the alarm screen
// never disagree by a tenth over the same night.
//
// Writes an EMPTY string when there is nothing to report (total_s <= 0) -- the
// caller's signal to hide the line entirely. "Slept 0.0/0.0 h" would be a
// claim about the night; an absent reading is not one.
//
// buf is always NUL-terminated and never overrun. A buf_len < 1 writes
// nothing at all.
void st_format_slept(int deep_s, int total_s, char *buf, int buf_len);
