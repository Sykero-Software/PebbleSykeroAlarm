// SPDX-License-Identifier: GPL-3.0-only
//
// The NEXT recorded night, dumped off the same Pebble Time 2 with the app's own
// debug dump (`Dump last night` -> `pebble logs`) on the morning of 2026-08-06
// and parsed from the `DBG h###` lines. 618 minutes of the firmware's own
// per-minute movement history, oldest first, starting at 21:50 local on
// 2026-08-05; index 600 is 07:50, the alarm time, and with SEMANTICS_RING_FROM
// the smart window is [07:50, 08:20] = indices 600..630. The history stops at
// index 617 (08:07) simply because that is when the dump was taken.
//
// This night is the CONTROL for night_2026_08_05.h. Everything that made that
// one hard is absent here: the firmware reported ONE unbroken session
// (23:50 -> 08:02), so there is nothing to merge, no awake gap to exclude, and
// the population is the whole night whichever way it is anchored. Keeping it is
// still worth the bytes, for three reasons:
//   1. It is a regression guard on the merging path doing NOTHING when there is
//      nothing to do -- the failure mode of a merger is inventing a gap.
//   2. It measures what the anchor alone is worth: 581 from the start of the
//      recorded history (21:50, an hour of being awake inside it) against 354
//      from the merged onset (23:50), on identical minutes.
//   3. It is a night on which the smart alarm decides nothing: at every
//      percentile from 75 to 95 it fires at 07:51, one minute into the window,
//      because the sleeper was already stirring when the window opened. A night
//      that cannot discriminate is exactly what a sensitivity change must not be
//      tuned against.
// Like the previous night, the median vmc is 0 (431 of 618 minutes are zero) --
// nothing like the synthetic nights in test_sleep_eval.c.
//
// No minute in this dump was excluded (no `!` prefix on any token), so
// is_invalid is false throughout.
#pragma once
#include "sleep_eval.h"

#define NIGHT_0806_LEN 618
// Local minute-of-day of index 0, so a test can name times instead of indices.
#define NIGHT_0806_FIRST_MIN (21 * 60 + 50)
// Epoch seconds of index 0 (2026-08-05 21:50:00 +03:00), derived from the dump's
// own `DBG now=1785992854 local=08-06 08:07:34`. The absolute value only has to
// be self-consistent with the span below for se_mark_awake.
#define NIGHT_0806_FIRST_UTC 1785955800u

// vmc, orientation. is_invalid is false for every minute of this night.
static const struct { uint16_t vmc; uint8_t orient; } k_night_0806[NIGHT_0806_LEN] = {
  { 1298,0x4b}, {  466,0x45}, { 1080,0x45}, {    0,0x56}, {  121,0x56}, {  244,0x66},
  {  855,0x55}, { 2569,0x61}, { 1993,0x74}, {  441,0x74}, { 1589,0x74}, { 3247,0x53},
  { 1587,0x42}, { 1922,0x61}, { 1739,0x52}, { 3935,0x43}, {  832,0x74}, {  759,0x74},
  {  784,0x74}, {  861,0x74}, { 2404,0x63}, { 1020,0x73}, { 1350,0x74}, { 1465,0x74},
  { 1333,0x73}, { 1560,0x73}, { 4110,0x54}, { 1055,0x74}, {  287,0x76}, {  294,0x75},
  {  500,0x74}, { 1042,0x74}, { 1030,0x74}, { 2190,0x73}, { 1440,0x73}, { 2648,0x73},
  { 1121,0x73}, { 1487,0x74}, { 2527,0x74}, { 1785,0x74}, { 2263,0x73}, { 5305,0x51},
  { 4243,0x34}, {  410,0x35}, {   86,0x35}, {   34,0x35}, {   38,0x35}, {   34,0x35},
  {  144,0x35}, {   42,0x36}, {  325,0x36}, {  192,0x35}, {  140,0x35}, {  263,0x25},
  { 1793,0x35}, { 2750,0x78}, { 1912,0x75}, { 2579,0x64}, { 2263,0x63}, {  381,0x72},
  { 1219,0x63}, {  512,0x64}, {    0,0x73}, {  705,0x74}, {  581,0x74}, {  418,0x74},
  { 1388,0x63}, { 3155,0x34}, {    9,0x35}, {    0,0x36}, {  211,0x36}, {  115,0x36},
  {   53,0x46}, {    0,0x36}, { 3045,0x45}, { 8210,0x54}, { 1521,0x51}, {  878,0x50},
  { 3446,0x51}, { 5180,0x53}, { 4033,0x52}, { 1645,0x44}, { 4750,0x42}, { 4923,0x45},
  { 3163,0x55}, { 6326,0x5a}, {   63,0x4a}, { 1691,0x5a}, {   21,0x62}, {    0,0x62},
  {    0,0x62}, { 4214,0x54}, { 7008,0x44}, { 1843,0x7a}, { 1213,0x53}, {  117,0x63},
  {    0,0x53}, {    0,0x53}, {  730,0x63}, {  724,0x63}, {    0,0x63}, {  140,0x53},
  {    0,0x53}, {   78,0x53}, { 1909,0x6f}, {  889,0x70}, {   75,0x53}, {  102,0x44},
  {    0,0x44}, {  570,0x54}, {  431,0x63}, { 2381,0x63}, { 3517,0x52}, { 2548,0x54},
  { 1415,0x74}, {  599,0x3f}, { 4255,0x31}, { 1916,0x3b}, {  410,0x3b}, {    0,0x3b},
  {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b},
  {    0,0x3b}, {    0,0x3b}, { 2122,0x86}, {    0,0x74}, {  876,0x74}, {    0,0x6f},
  {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f},
  {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f},
  {  720,0x6f}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e},
  {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e},
  {    0,0x6e}, {    0,0x6e}, { 1452,0x7e}, {   36,0x67}, {    0,0x67}, {    0,0x67},
  {    0,0x67}, {    0,0x67}, {    0,0x67}, {    0,0x67}, {    0,0x67}, {    0,0x67},
  {    0,0x67}, {    0,0x67}, {    0,0x67}, {  142,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {  148,0x67}, {    0,0x58},
  {  552,0x69}, {    0,0x69}, {    0,0x69}, {    9,0x69}, {    0,0x69}, {    0,0x69},
  { 1531,0x29}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08},
  {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08},
  {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08},
  {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08},
  {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08},
  {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08},
  {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08}, {    0,0x08},
  {    0,0x08}, { 1753,0x49}, {    0,0x69}, {    0,0x69}, {  614,0x78}, {  312,0x77},
  {    0,0x79}, {  100,0x7a}, {    0,0x7a}, {    0,0x7a}, { 1094,0x3c}, {    0,0x3c},
  {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c},
  {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c},
  {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    5,0x2c}, {    0,0x2c}, {    0,0x2c},
  {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c},
  {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c},
  {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, { 1063,0x36}, {    0,0x45},
  {    0,0x45}, {    0,0x45}, {    0,0x45}, {    0,0x45}, {    0,0x45}, {    0,0x45},
  {    0,0x45}, {    0,0x45}, {    0,0x45}, {    0,0x45}, {    0,0x45}, {    0,0x45},
  {    0,0x45}, {    0,0x45}, {    0,0x45}, {    0,0x45}, {    0,0x45}, {  273,0x45},
  {  547,0x3b}, {    0,0x3b}, {  972,0x4b}, {    0,0x6a}, {    0,0x6a}, {    0,0x6a},
  {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {    0,0x6a},
  {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {    0,0x6a},
  {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {  999,0x6a},
  {  134,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b},
  {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b},
  {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b},
  {    0,0x3b}, {    7,0x3b}, {  812,0x78}, {   57,0x77}, {    0,0x77}, {  121,0x66},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x55}, {    0,0x55}, {    0,0x55}, {    0,0x55},
  {    0,0x55}, {    0,0x55}, {    0,0x55}, {    0,0x55}, {    0,0x55}, {    0,0x55},
  {    0,0x55}, {    0,0x55}, {    0,0x55}, {  868,0x55}, { 1419,0x74}, {  477,0x70},
  {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, { 1134,0x6f},
  {  500,0x3e}, {  760,0x73}, {    0,0x74}, {    0,0x74}, {    0,0x74}, { 1192,0x7f},
  {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e},
  {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e},
  {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e},
  {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e},
  {    0,0x6e}, { 1334,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e},
  {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, {    0,0x6e}, { 1529,0x7f}, {    0,0x73},
  {    0,0x74}, {    0,0x74}, {    0,0x73}, {    0,0x73}, {    0,0x73}, {    0,0x74},
  {  793,0x4f}, {  793,0x70}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74},
  {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74},
  {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74},
  {    0,0x74}, { 1333,0x71}, {    0,0x60}, {    0,0x60}, {    0,0x60}, {    0,0x60},
  {    0,0x60}, {    0,0x60}, {    0,0x60}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f},
  { 1134,0x6f}, {    0,0x7e}, {    0,0x7e}, {    0,0x7e}, {    0,0x7e}, {    0,0x7e},
  {    0,0x7e}, { 1169,0x7e}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74},
  {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74},
  {    5,0x74}, {  562,0x6e}, {  354,0x7d}, {    0,0x7d}, { 5434,0x61}, { 1778,0x75},
  { 5779,0x63}, { 2854,0x3b}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c},
  {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c},
  {    0,0x3c}, {  915,0x66}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {   13,0x65}, {  757,0x2c},
  {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c},
  {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {    0,0x2c}, {  755,0x5a}, {    0,0x6a},
  { 1995,0x6a}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b},
  {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b}, {    0,0x3b},
  {    0,0x3b}, { 2176,0x4b}, {    0,0x75}, {    0,0x75}, {    0,0x75}, {    0,0x75},
  {    0,0x75}, {    0,0x75}, {    0,0x75}, {    0,0x75}, {    0,0x75}, { 1011,0x85},
  {  161,0x60}, {    0,0x60}, {    0,0x60}, {    0,0x60}, {    0,0x60}, {    0,0x60},
  {    0,0x60}, {    0,0x60}, {  928,0x71}, {    0,0x72}, {    0,0x72}, {    0,0x72},
  {    0,0x72}, {    0,0x72}, {    0,0x72}, {    0,0x72}, {  568,0x73}, {    0,0x73},
  {    0,0x73}, {    0,0x73}, {    0,0x73}, {    0,0x73}, {    0,0x73}, {    0,0x73},
  {    0,0x73}, { 1221,0x72}, {    0,0x83}, {    0,0x83}, {    0,0x83}, {    0,0x83},
  {  703,0x6f}, {    0,0x5f}, {  475,0x60}, { 2369,0x3c}, {    1,0x3c}, {    0,0x3c},
  {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c},
  {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c}, {    0,0x3c},
  {    0,0x3c}, { 2024,0x71}, {    0,0x72}, {    0,0x72}, {    0,0x72}, {    0,0x72},
  {    0,0x72}, {    0,0x72}, {    0,0x72}, {    0,0x72}, {    0,0x72}, { 1591,0x72},
  {    0,0x1d}, { 1787,0x6b}, {  570,0x79}, {  314,0x63}, {   13,0x63}, {  225,0x63},
  {    0,0x62}, {   88,0x63}, {  121,0x63}, { 3236,0x53}, { 2015,0x55}, { 1816,0x76},
  {    1,0x76}, {    0,0x00}, {   75,0x76}, {    3,0x76}, {  647,0x84}, {   86,0x72},
};

// Load the night into `dst` (which must hold NIGHT_0806_LEN entries).
static void night_0806_load(SleepMinute *dst) {
  for (int i = 0; i < NIGHT_0806_LEN; i++) {
    dst[i].vmc = k_night_0806[i].vmc;
    dst[i].orientation = k_night_0806[i].orient;
    dst[i].is_invalid = false;
  }
}

// Index of a local wall-clock time on this night, e.g. night_0806_idx(7, 50) == 600.
static int night_0806_idx(int hour, int min) {
  int d = hour * 60 + min - NIGHT_0806_FIRST_MIN;
  if (d < 0) {
    d += 24 * 60;
  }
  return d;
}

// The ONE sleep session the firmware reported, transcribed verbatim from the
// watch's own dump run against this night:
//
//   DBG onset merged=08-05 23:50 newest=08-05 23:50 sessions=1 gaps=0
//   DBG sess0 08-05 23:50 -> 08-06 08:02
//   DBG eval pct=90/2min base=0 lvl=581 fire=1 fidx=601 at=08-06 07:51 acc=2006 insuf=0
//   DBG evalonset off=120 base=0 lvl=354 fire=1 fidx=481 at=08-06 07:51 acc=2233
//
// The host replay reproduces both levels and that 07:51 exactly, which is what
// makes this fixture a check on the real chain rather than on itself.
//
// The session's END (08:02) is 11 minutes past the fire point and 6 minutes past
// the alarm actually ringing -- the firmware only closed the session once the
// user was up. Do NOT read it as a bound on the population: se_evaluate is fed
// the whole recorded stretch and stops at the window, not at the session end.
#define NIGHT_0806_SPANS(name) \
  const SleepSpan name[1] = { \
    { NIGHT_0806_FIRST_UTC + (uint32_t)night_0806_idx(23, 50) * 60u, \
      NIGHT_0806_FIRST_UTC + (uint32_t)night_0806_idx(8, 2) * 60u }, \
  }
