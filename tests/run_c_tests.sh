#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Compiles and runs every host-side C test (tests/test_*.c) with plain gcc
# against the C sources under test -- no Pebble SDK, no emulator, just the
# pure-C modules exercised on the host. Run via `npm run test:c` (and so by
# `npm test`, which chains this after the node --test suite) so a single
# command actually re-runs these on every change, instead of them only being
# run by hand (which is how they were run before this script existed).
#
# Must be invoked with the PebbleSykeroAlarm package root as the working
# directory (true for `npm run`, which always cds there first) -- the -I and
# source paths below are relative to it, matching the gcc invocations
# documented in each test file's own header comment.
set -euo pipefail

cd "$(dirname "$0")/.."

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

run_one() {
  local name="$1"; shift
  echo "--- $name ---"
  gcc -std=c11 -Wall -I src/c -o "$tmp/$name" "$@"
  "$tmp/$name"
}

run_one t_alarm_calc     tests/test_alarm_calc.c     src/c/alarm_calc.c
run_one t_escalation     tests/test_escalation.c     src/c/escalation.c
run_one t_sleep_eval     tests/test_sleep_eval.c     src/c/sleep_eval.c

# t_pack_contract is different: it takes its packed strings as argv, generated
# HERE by invoking the REAL compiled packer (src/pkjs/alarm_pack.js, generated
# by tsc from src/ts/alarm_pack.ts) via node -- not hardcoded literals in the
# .c file. This is what makes the test a real pipeline check: a packer
# regression (e.g. pad2 dropping a leading zero, or daysToDigits inverting the
# weekday order) changes what node prints here, and the C assertions -- which
# encode the same semantic slot inputs, not the packed string itself -- catch
# it. `npm test`'s `pretest` already runs tsc, but regenerate unconditionally
# here too so this script also works standalone (`bash tests/run_c_tests.sh`,
# or `npm run test:c` alone, neither of which runs `pretest`).
echo "--- regenerating src/pkjs (tsc), for the real packer below ---"
./node_modules/.bin/tsc

pack_js="src/pkjs/alarm_pack.js"

gen_packed() {
  # $1 is a small JS expression (as the body of an IIFE) that returns an array
  # of slot objects; prints packAlarmSet(that array).
  node -e "
    const { packAlarmSet } = require('./$pack_js');
    const D = (s) => s.split('').map((c) => c === '1');
    process.stdout.write(packAlarmSet($1));
  "
}

packed3="$(gen_packed "[
  { enabled: true,  time: '07:00', days: D('1111100') },
  { enabled: false, time: '08:30', days: D('0000011') },
  { enabled: true,  time: '05:15', days: D('0000000') },
]")"

packed8="$(gen_packed "(function () {
  const slots = [];
  for (let i = 0; i < 8; i++) {
    slots.push({
      enabled: i % 2 === 0,
      time: '0' + i + ':00',
      days: i % 2 === 0 ? D('1111100') : D('0000000'),
    });
  }
  return slots;
})()")"

packed_empty="$(gen_packed "[]")"

echo "--- t_pack_contract (fed from the real compiled packer) ---"
gcc -std=c11 -Wall -I src/c -o "$tmp/t_pack_contract" tests/test_pack_contract.c src/c/alarm_calc.c
"$tmp/t_pack_contract" "$packed3" "$packed8" "$packed_empty"

echo "--- all host C tests passed ---"
