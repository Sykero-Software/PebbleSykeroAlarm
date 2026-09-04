// SPDX-License-Identifier: GPL-3.0-only
//
// gcc -std=c11 -Wall -I src/c -o /tmp/t tests/test_sleep_text.c src/c/sleep_text.c
#include "sleep_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  char b[32];

  // --- the ordinary case: deep first, total second, truncated tenths --------
  st_format_slept(6300, 23400, b, sizeof(b));      // 1.75 h of 6.5 h
  assert(strcmp(b, "Slept 1.7/6.5 h") == 0);       // truncated, not rounded

  st_format_slept(0, 3600, b, sizeof(b));
  assert(strcmp(b, "Slept 0.0/1.0 h") == 0);

  // A long night: two-digit hours must still fit and read correctly.
  st_format_slept(4 * 3600, 12 * 3600, b, sizeof(b));
  assert(strcmp(b, "Slept 4.0/12.0 h") == 0);

  // --- nothing to show -----------------------------------------------------
  // No data at all (health off, or the night not recorded yet) is the caller's
  // signal to hide the whole line, so it must be an EMPTY string, never
  // "Slept 0.0/0.0 h" -- a zeroed line reads as "you did not sleep", which is
  // a claim this function has no basis to make.
  st_format_slept(0, 0, b, sizeof(b));
  assert(b[0] == '\0');
  st_format_slept(1000, 0, b, sizeof(b));
  assert(b[0] == '\0');
  st_format_slept(0, -5, b, sizeof(b));
  assert(b[0] == '\0');

  // --- nonsense in, sane out ----------------------------------------------
  // Deep sleep can never exceed the total; the firmware's two sums are read
  // independently and a sample landing between them could disagree by a
  // minute. Clamp rather than print an impossible pair.
  st_format_slept(9000, 7200, b, sizeof(b));
  assert(strcmp(b, "Slept 2.0/2.0 h") == 0);
  st_format_slept(-60, 7200, b, sizeof(b));
  assert(strcmp(b, "Slept 0.0/2.0 h") == 0);

  // --- never overruns, always terminated -----------------------------------
  char small[8];
  memset(small, 'x', sizeof(small));
  st_format_slept(6300, 23400, small, (int)sizeof(small));
  assert(small[sizeof(small) - 1] == '\0');

  // A zero-length (or absurd) buffer must not be written to at all.
  char guard[2] = { 'a', 'b' };
  st_format_slept(6300, 23400, guard, 0);
  assert(guard[0] == 'a' && guard[1] == 'b');

  printf("test_sleep_text: OK\n");
  return 0;
}
