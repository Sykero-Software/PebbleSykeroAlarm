// SPDX-License-Identifier: GPL-3.0-only
//
// CONTRACT WITH THE PHONE: this file re-parses, through the REAL ac_parse_set
// in ../src/c/alarm_calc.c, the SAME literal packed strings tests/alarm_pack.
// test.js already asserts packAlarmSet produces. The two test files are
// independently-maintained pins of one wire format (leading '-' = disabled,
// "HH:MM|DDDDDDD" with bit0 = Monday) with no cross-check other than this
// file existing -- phone/watch message-format drift is this project's
// single most-repeated bug class (see the superrepo CLAUDE.md's
// message-key-drift history). If you change what alarm_pack.test.js asserts
// a packed string looks like, update the matching literal here too (and vice
// versa).
//
// Build/run (from the PebbleSykeroAlarm root, matching test_alarm_calc.c's
// and test_escalation.c's own convention):
//   gcc -std=c11 -Wall -I src/c -o /tmp/t_pack_contract
//     tests/test_pack_contract.c src/c/alarm_calc.c && /tmp/t_pack_contract
// Wired into `npm run test:c` (and so into `npm test`) via
// tests/run_c_tests.sh -- see that script for the exact invocation used.
#include "alarm_calc.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  Alarm out[MAX_ALARMS];
  int n;

  // From alarm_pack.test.js's 'packs enabled and disabled slots' /
  // 'round-trips through unpackAlarmSet':
  //   packAlarmSet([{enabled:true,time:'07:00',days:D('1111100')},
  //                 {enabled:false,time:'08:30',days:D('0000011')},
  //                 {enabled:true,time:'05:15',days:D('0000000')}])
  //     => "07:00|1111100;-08:30|0000011;05:15|0000000"
  n = ac_parse_set("07:00|1111100;-08:30|0000011;05:15|0000000", out, MAX_ALARMS);
  assert(n == 3);
  assert(out[0].minute_of_day == 7 * 60);
  assert(out[0].weekday_mask == 0x1F);      // Mon-Fri, bit0=Mon .. bit4=Fri
  assert(out[0].enabled == true);
  assert(out[1].minute_of_day == 8 * 60 + 30);
  assert(out[1].weekday_mask == 0x60);      // Sat+Sun, bit5+bit6
  assert(out[1].enabled == false);
  assert(out[2].minute_of_day == 5 * 60 + 15);
  assert(out[2].weekday_mask == 0);         // one-time
  assert(out[2].enabled == true);
  printf("3-slot literal OK\n");

  // From alarm_pack.test.js's 'round-trips all SLOT_COUNT (8) slots by
  // content, not just the truncated count' -- the exact 8-slot literal that
  // test asserts packAlarmSet produces (alternating enabled/disabled and
  // weekday/one-time), fed through the real parser here.
  n = ac_parse_set(
      "00:00|1111100;-01:00|0000000;02:00|1111100;-03:00|0000000;"
      "04:00|1111100;-05:00|0000000;06:00|1111100;-07:00|0000000",
      out, MAX_ALARMS);
  assert(n == MAX_ALARMS);
  for (int i = 0; i < MAX_ALARMS; i++) {
    assert(out[i].minute_of_day == i * 60);
    assert(out[i].enabled == (i % 2 == 0));
    assert(out[i].weekday_mask == (i % 2 == 0 ? 0x1F : 0));
  }
  printf("8-slot literal OK\n");

  // From alarm_pack.test.js's 'an empty slot list packs to the empty string':
  //   packAlarmSet([]) === ''  -- the watch must read this as "no alarms".
  n = ac_parse_set("", out, MAX_ALARMS);
  assert(n == 0);
  printf("empty-set literal OK\n");

  printf("ALL PACK/PARSE CONTRACT CASES PASSED\n");
  return 0;
}
