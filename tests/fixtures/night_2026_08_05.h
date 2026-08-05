// SPDX-License-Identifier: GPL-3.0-only
//
// A REAL recorded night, dumped off the user's Pebble Time 2 with the app's own
// debug dump (`Dump last night` -> `pebble logs`) on 2026-08-05 and parsed from
// the `DBG h###` lines. 640 minutes of the firmware's own per-minute movement
// history, oldest first, starting at 21:50 local on 2026-08-04; index 600 is
// 07:50, the alarm time, and with SEMANTICS_RING_FROM the smart window is
// [07:50, 08:20] = indices 600..630.
//
// Why a recorded night and not another synthetic one: this night is what
// revealed BOTH defects the session-merging code fixes, and neither is
// reproducible from the synthetic generators in test_sleep_eval.c.
//   1. The firmware ended the sleep session at a trip to the toilet and started
//      a new one at 05:20, so hr_read_night anchored the ranking population
//      there: 130 minutes instead of the whole night (P90 level 525, and a
//      population that thin swings the level 392 <-> 955 on a one-minute
//      anchor shift).
//   2. This watch's vmc drops back to 0 between movements, so the trip's
//      longest contiguous run above 4 x median + margin is 2 MINUTES --
//      se_evaluate's own >= 8-minute wake-episode exclusion cannot see it, and
//      the trip's spikes (up to 7950) stay in the population.
//
// The sessions below are the REAL ones the watch reported, and they carry a fact
// worth more than the fixture itself: the session ENDED AT 05:14, seven minutes
// AFTER the movement started at 05:07. The firmware keeps the beginning of an
// arousal inside the sleep session, so se_mark_awake can only exclude 05:14-05:19
// (level 674 -> 616), while 963/674/1207/3785 at 05:07-05:12 stay in the
// population; excluding the whole trip would give 471. Do NOT assume a session
// boundary coincides with the first movement -- an earlier version of this
// fixture guessed 05:07 and produced a level (414) that the watch never computed.
// The night's median vmc is 0, which is also nothing like the synthetic nights.
#pragma once
#include "sleep_eval.h"

#define NIGHT_0805_LEN 640
// Local minute-of-day of index 0, so a test can name times instead of indices.
#define NIGHT_0805_FIRST_MIN (21 * 60 + 50)
// Epoch seconds of index 0 (2026-08-04 21:50:00 +03:00). The absolute value
// only has to be self-consistent with the spans below for se_mark_awake.
#define NIGHT_0805_FIRST_UTC 1785869400u

// vmc, orientation. is_invalid is false for every minute of this night (the
// dump reported no `x`).
static const struct { uint16_t vmc; uint8_t orient; } k_night_0805[NIGHT_0805_LEN] = {
  {  683,0x60}, { 5407,0x53}, { 2436,0x51}, { 1916,0x51}, {  500,0x63}, {  219,0x53},
  {  242,0x53}, { 1722,0x64}, { 2404,0x77}, { 4066,0x75}, {  552,0x88}, {  294,0x8b},
  {    0,0x8d}, {    0,0x8d}, {  481,0x86}, { 5189,0x35}, { 1808,0x34}, {    0,0x34},
  {    0,0x34}, {   88,0x34}, {    0,0x34}, {  689,0x35}, {    0,0x24}, {  913,0x34},
  { 4866,0x30}, { 6162,0x44}, {    0,0x57}, {    0,0x57}, {  587,0x57}, {    0,0x57},
  {   65,0x57}, {   26,0x57}, {    0,0x57}, {   19,0x57}, {   88,0x57}, { 2465,0x76},
  {  714,0x78}, { 1177,0x77}, {  146,0x59}, {  446,0x84}, {  194,0x85}, {  287,0x76},
  {  857,0x73}, { 4602,0x44}, { 1410,0x35}, {   65,0x35}, {  789,0x36}, {  314,0x35},
  {  296,0x25}, {  165,0x25}, {  701,0x35}, {   57,0x25}, {   88,0x35}, {  163,0x35},
  {    1,0x46}, {  308,0x36}, {    0,0x25}, {  252,0x35}, { 3376,0x34}, { 1751,0x73},
  { 3022,0x62}, { 6464,0x55}, { 4303,0x52}, {  479,0x60}, { 4837,0x52}, { 3976,0x63},
  {  308,0x51}, { 1319,0x23}, { 2375,0x43}, { 1918,0x41}, {  273,0x30}, { 4735,0x4f},
  { 1483,0x1f}, {  772,0x22}, {   90,0x4a}, {   25,0x4a}, {   86,0x4a}, {  995,0x4a},
  {  523,0x63}, { 1049,0x5f}, { 1832,0x5e}, { 1818,0x7c}, { 1916,0x3f}, {  978,0x20},
  {  394,0x30}, {    0,0x3f}, { 1394,0x4d}, { 4083,0x60}, { 4769,0x41}, {  583,0x50},
  {  329,0x40}, { 6883,0x33}, { 4557,0x51}, { 4579,0x41}, { 4251,0x62}, {  942,0x54},
  { 3176,0x52}, { 4361,0x54}, { 1005,0x51}, { 3974,0x52}, { 5511,0x51}, { 8088,0x78},
  {    0,0x5b}, {  352,0x6b}, { 1178,0x6b}, { 3533,0x62}, {   86,0x62}, { 2105,0x68},
  { 1481,0x5a}, { 2914,0x5a}, {  988,0x71}, { 1876,0x72}, { 1961,0x71}, {  708,0x5a},
  { 2957,0x2f}, {  926,0x4b}, { 6426,0x75}, {    0,0x75}, {    0,0x75}, {  450,0x75},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {   77,0x65}, { 1088,0x6f}, {    0,0x6f},
  {    0,0x6f}, { 2384,0x4b}, {    0,0x4c}, {    3,0x4b}, {    0,0x4b}, {    0,0x4b},
  {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b},
  {    0,0x4b}, {    0,0x4b}, {  897,0x4b}, {  822,0x76}, {    0,0x77}, { 2662,0x77},
  {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, { 1088,0x73},
  {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f},
  {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f},
  {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, {    0,0x6f}, { 2696,0x7e},
  {    0,0x77}, {  739,0x78}, {    0,0x79}, {    0,0x79}, {    0,0x79}, {    0,0x79},
  {    0,0x79}, {  261,0x79}, {    0,0x7a}, {    0,0x7a}, {  905,0x6a}, {    0,0x6a},
  {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {    0,0x6a}, {  471,0x6a}, {    0,0x6a},
  {  637,0x5b}, {    0,0x3d}, {    0,0x3d}, {    0,0x3d}, {    0,0x3d}, {    0,0x3d},
  {    0,0x3d}, {    0,0x3d}, {    0,0x3d}, {    0,0x3d}, {    0,0x3d}, { 1504,0x4c},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  { 1024,0x77}, {  448,0x6a}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x5b}, { 1101,0x45}, {    0,0x44}, {    0,0x44}, {    0,0x44},
  {  616,0x45}, { 1117,0x69}, {    0,0x54}, {    0,0x54}, {    0,0x54}, {    0,0x54},
  {    0,0x54}, {    0,0x54}, {    0,0x54}, {    0,0x54}, {    0,0x54}, {    0,0x54},
  {    0,0x54}, {    0,0x54}, {    0,0x54}, { 1044,0x55}, {    9,0x4b}, {    0,0x4b},
  {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4c},
  {    0,0x4c}, {    0,0x4c}, {    0,0x4c}, {    0,0x4c}, {    0,0x4c}, {    0,0x4c},
  {    0,0x4c}, {    0,0x4c}, {    0,0x4c}, {    0,0x4c}, {    0,0x4c}, {    0,0x4c},
  { 1880,0x7c}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74},
  {    0,0x74}, {    0,0x74}, {    3,0x74}, { 1109,0x4e}, {    0,0x4e}, {    0,0x4e},
  {    0,0x4e}, {    0,0x4e}, {    0,0x4e}, {    0,0x4e}, { 1518,0x4c}, {    0,0x4c},
  {    0,0x4b}, {  903,0x6a}, { 1252,0x79}, {    0,0x74}, {    0,0x74}, {    0,0x74},
  {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {  414,0x73},
  {  728,0x3d}, {    0,0x2d}, {    0,0x2d}, { 1918,0x7f}, {    1,0x7f}, {    0,0x7f},
  {    0,0x7f}, {    0,0x7f}, {    0,0x7f}, {    0,0x7f}, {    0,0x7f}, {    0,0x7f},
  {    0,0x7f}, {    0,0x7f}, {  373,0x70}, {   48,0x62}, {    0,0x62}, {    0,0x62},
  {    0,0x62}, {    0,0x62}, {    0,0x62}, {    0,0x62}, {    0,0x62}, {    0,0x62},
  {    0,0x62}, {    0,0x62}, {    0,0x62}, {    0,0x62}, {    0,0x62}, { 2409,0x62},
  {    0,0x73}, {    0,0x73}, {    0,0x73}, {    0,0x73}, {    0,0x73}, {    0,0x73},
  {  676,0x73}, { 1404,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b},
  {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b},
  {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b},
  {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b},
  {    0,0x4b}, { 1623,0x6a}, {    0,0x76}, {    0,0x76}, {    0,0x76}, { 1198,0x77},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {  963,0x6a},
  {  674,0x69}, {    0,0x79}, {    0,0x79}, { 1207,0x79}, { 3785,0x51}, {    0,0x41},
  { 7950,0x62}, { 2261,0x64}, {    0,0x64}, {    0,0x64}, {    0,0x64}, { 1107,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, { 1015,0x2d}, {    0,0x2d}, {    0,0x2d}, {    0,0x2d},
  { 1589,0x3d}, {  628,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x64}, {    0,0x64},
  {    0,0x64}, {    0,0x64}, {    0,0x64}, {    0,0x64}, {    0,0x64}, {    0,0x64},
  {    0,0x64}, {    0,0x64}, {    0,0x64}, {    0,0x64}, {    0,0x64}, {    0,0x64},
  {    0,0x64}, {    0,0x64}, { 1616,0x7f}, {    0,0x4e}, {    0,0x4e}, {    0,0x4e},
  {    0,0x4e}, {    0,0x4e}, {  525,0x4e}, {  362,0x74}, {    0,0x74}, {    0,0x74},
  {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74},
  {    0,0x74}, {    0,0x74}, {    0,0x74}, {  764,0x71}, {    0,0x71}, {    0,0x71},
  {    0,0x71}, {    0,0x71}, {  183,0x71}, {  791,0x7e}, {    0,0x7e}, {    0,0x7e},
  {   50,0x7e}, {    0,0x7e}, {    0,0x7e}, {    0,0x7e}, {    0,0x7e}, {    0,0x7e},
  {    0,0x7e}, {    0,0x7e}, {    0,0x7e}, {  595,0x7e}, {    0,0x74}, {    0,0x74},
  {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74}, {    0,0x74},
  {    0,0x74}, { 5667,0x7e}, {  342,0x6f}, { 1271,0x70}, {    0,0x70}, {    0,0x70},
  {    0,0x70}, {    0,0x70}, {    0,0x70}, {    0,0x70}, {    0,0x70}, {    0,0x70},
  {    0,0x70}, {    0,0x70}, {    0,0x70}, {    0,0x70}, {    0,0x70}, {    0,0x70},
  {    0,0x70}, {    0,0x70}, {  119,0x70}, { 2028,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {  955,0x78}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65}, {    0,0x65},
  {    0,0x65}, { 2159,0x65}, {  392,0x73}, {    0,0x73}, {    0,0x73}, {    0,0x73},
  {    0,0x73}, {    0,0x73}, { 1151,0x73}, {    0,0x2d}, {    0,0x2d}, {    0,0x2d},
  { 1704,0x4b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b}, {    0,0x5b},
  {    0,0x5b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {    0,0x4b}, {  967,0x4b},
  {   52,0x76}, {    0,0x76}, {    0,0x76}, {    0,0x76}, {    0,0x76}, {    0,0x76},
  {    0,0x76}, {    0,0x76}, {    0,0x76}, {    0,0x76}, {    0,0x75}, {    0,0x64},
  {   19,0x2c}, {  747,0x74}, {  635,0x85}, {  242,0x5b}, {  473,0x4b}, {  880,0x5a},
  { 4365,0x3d}, {  943,0x62}, {    0,0x62}, {    0,0x62}, {    9,0x62}, { 2517,0x61},
  {  281,0x5e}, {  529,0x86}, {  614,0x75}, { 2055,0x75}, {  984,0x75}, { 7969,0x43},
  { 2883,0x42}, {  429,0x76}, {  841,0x33}, { 1710,0x73}, {   52,0x81}, {  134,0x8e},
  { 1148,0x74}, {    1,0x71}, {    0,0x71}, {  504,0x48}, {    0,0x48}, { 1329,0x75},
  { 1032,0x74}, {  614,0x73}, {   53,0x8f}, {    3,0x72},
};

// Load the night into `dst` (which must hold NIGHT_0805_LEN entries).
static void night_0805_load(SleepMinute *dst) {
  for (int i = 0; i < NIGHT_0805_LEN; i++) {
    dst[i].vmc = k_night_0805[i].vmc;
    dst[i].orientation = k_night_0805[i].orient;
    dst[i].is_invalid = false;
  }
}

// Index of a local wall-clock time on this night, e.g. night_0805_idx(7, 50) == 600.
static int night_0805_idx(int hour, int min) {
  int d = hour * 60 + min - NIGHT_0805_FIRST_MIN;
  if (d < 0) {
    d += 24 * 60;
  }
  return d;
}

// The two sleep sessions the firmware reported, NEWEST FIRST (as
// health_service_activities_iterate delivers them), transcribed verbatim from the
// watch's own dump run against this night with the merging build:
//
//   DBG onset merged=08-04 23:50 newest=08-05 05:20 sessions=2 gaps=1
//   DBG sess0 08-05 05:20 -> 08-05 07:56
//   DBG sess1 08-04 23:50 -> 08-05 05:14
//   DBG awake0 08-05 05:14 -> 08-05 05:20 (excluded)
//   DBG evalonset off=120 base=0 lvl=616 fire=1 fidx=486 at=08-05 07:56
//
// The host replay reproduces that lvl=616 and that 07:56 exactly, which is what
// makes this fixture a check on the real chain rather than on itself.
#define NIGHT_0805_SPANS(name) \
  const SleepSpan name[2] = { \
    { NIGHT_0805_FIRST_UTC + (uint32_t)night_0805_idx(5, 20) * 60u, \
      NIGHT_0805_FIRST_UTC + (uint32_t)night_0805_idx(7, 56) * 60u }, \
    { NIGHT_0805_FIRST_UTC + (uint32_t)night_0805_idx(23, 50) * 60u, \
      NIGHT_0805_FIRST_UTC + (uint32_t)night_0805_idx(5, 14) * 60u }, \
  }
