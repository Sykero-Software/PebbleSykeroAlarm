// SPDX-License-Identifier: GPL-3.0-only
#ifndef ALARM_CALC_H
#define ALARM_CALC_H
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define MAX_ALARMS 8

// One alarm slot. weekday_mask bit0=Monday … bit6=Sunday; 0 means a one-time
// alarm (fires at the next occurrence of minute_of_day, then is disabled by the
// caller). skip_next makes the *next* occurrence be skipped exactly once.
typedef struct {
  uint16_t minute_of_day;   // 0..1439
  uint8_t  weekday_mask;
  bool     enabled;
  bool     skip_next;
} Alarm;

// Next time this alarm fires, strictly after `now`, in local wall-clock terms.
// Returns 0 if it never fires (disabled).
//
// Computed via localtime/mktime rather than by adding multiples of 86400, so a
// 07:00 alarm stays at wall-clock 07:00 across a DST transition: the real time
// that elapses is an hour shorter (spring) or longer (autumn) than the wall-clock
// difference, which is the correct behaviour for an alarm clock.
time_t ac_next_occurrence(const Alarm *a, time_t now);

// Index of the alarm that fires soonest, or -1 if none will. On success writes
// that alarm's time to *out_when (may be NULL).
int ac_next_alarm(const Alarm *alarms, int count, time_t now, time_t *out_when);

// Parse the packed AlarmSet wire format into out[]:
//     "07:00|1111100;-08:30|0000011"
// slots separated by ';', each "[-]HH:MM|DDDDDDD" where the seven digits are
// Mon..Sun and a leading '-' means the slot is disabled. Malformed slots are
// skipped. Returns the number of alarms written (<= max).
//
// Hand-rolled digit parsing on purpose: atoi/strtol are not exported by the
// Core Devices firmware and hard-fault on hardware.
int ac_parse_set(const char *s, Alarm *out, int max);

#endif
