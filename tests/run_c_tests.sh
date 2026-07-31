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
run_one t_pack_contract  tests/test_pack_contract.c  src/c/alarm_calc.c

echo "--- all host C tests passed ---"
