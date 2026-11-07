#!/usr/bin/env bash
# Tests of mpv/vsr.lua: LuaJIT unit tests, then the headless mpv groups (groups/*.sh, driven by run_groups.sh).
# Prints PASS/FAIL per check; the exit status is the number of failed checks (capped at 255).
#
#   mpv/tests/run.sh                 unit tests, then all groups (headless mpv, uses the GPU)
#   mpv/tests/run.sh --unit          only the unit tests (no mpv, no GPU)
#   mpv/tests/run.sh --bench         only the real-time playback numbers (720p30, 1080p60; needs an idle machine)
#   mpv/tests/run.sh --only GROUP... only these groups: classification fullscreen keys failures odd_paths power
#                                    adaptive lifecycle bench
#   mpv/tests/run.sh --allow-skip    groups may be skipped (GPU busy or Ollama resident): exit 0 instead of 77
#
# Environment, skips and scratch directory: see run_groups.sh. A group skip (exit 77 from run_groups.sh) prints
# "groups SKIPPED (reason)" and exits 77 unless --allow-skip is given.
set -u
set -o pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MODE=all
ALLOW_SKIP=0
ONLY=()
while [ $# -gt 0 ]; do
  case $1 in
    --unit) MODE=unit ;;
    --bench) MODE=bench ;;
    --allow-skip) ALLOW_SKIP=1 ;;
    --only) MODE=only; shift; while [ $# -gt 0 ] && [[ $1 != --* ]]; do ONLY+=("$1"); shift; done; continue ;;
    *) echo "usage: $0 [--unit | --bench | --only GROUP... | --allow-skip]" >&2; exit 2 ;;
  esac
  shift
done

PASS=0; FAIL=0; SKIP=
OUT=$(mktemp) || exit 1
trap 'rm -f "$OUT"' EXIT

run() {  # run CMD...: streams its output and adds its SUMMARY counts; a runner that fails without one counts 1
  local rc s P F
  "$@" 2>&1 | tee "$OUT"; rc=${PIPESTATUS[0]}
  if [ "$rc" -eq 77 ]; then SKIP=$(sed -n 's/^SKIP mpv groups: //p' "$OUT" | head -1); SKIP=${SKIP:-skipped}; return; fi
  s=$(sed -n 's/^SUMMARY: //p' "$OUT" | tail -1)
  if [ -n "$s" ]; then
    read -r P _ F _ <<<"$s"
    PASS=$((PASS + P)); FAIL=$((FAIL + F))
  elif [ "$rc" -ne 0 ]; then
    FAIL=$((FAIL + 1))
  fi
}

if [ $MODE != bench ] && [ $MODE != only ]; then
  echo "== 0. unit tests (pure logic, LuaJIT)"
  run luajit "$HERE/test_logic.lua"
fi
case $MODE in
  unit) ;;
  bench) run bash "$HERE/run_groups.sh" --only bench ;;
  only) run bash "$HERE/run_groups.sh" --only "${ONLY[@]}" ;;
  all) run bash "$HERE/run_groups.sh" ;;
esac

echo; [ -n "$SKIP" ] && echo "groups SKIPPED ($SKIP)"
echo "SUMMARY: $PASS passed, $FAIL failed"
if [ -n "$SKIP" ] && [ $ALLOW_SKIP -eq 0 ] && [ $FAIL -eq 0 ]; then exit 77; fi
exit $(( FAIL > 255 ? 255 : FAIL ))
