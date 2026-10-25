#!/bin/sh
# Headless tests for browser/: host protocol, host + private mpv, extension logic, manifest lint.
#   browser/tests/run.sh            (exit status 1 when any test fails)
# Environment (optional):
#   NVVSR_TEST_MPV     private mpv binary (default ~/.local/opt/mpv-vsr/bin/mpv); mpv tests skip without it
#   NVVSR_TEST_CLIP    media file for the mpv tests (default: a 20 s 720p clip generated with ffmpeg)
#   NVVSR_GPU_LOCK     path of a lock file another agent creates while it measures the GPU; this waits for its
#                      removal (up to 5 minutes) before running anything
#   TMPDIR             where the temporary HOME and XDG directories go
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE" || exit 2

if [ -n "${NVVSR_GPU_LOCK:-}" ]; then
  waited=0
  while [ -e "$NVVSR_GPU_LOCK" ]; do
    if [ "$waited" -ge 300 ]; then
      echo "FAIL gpu-lock (still held after 300 s: $NVVSR_GPU_LOCK)"
      echo "SUMMARY: 0 PASS, 1 FAIL, 0 SKIP"
      exit 1
    fi
    [ "$waited" -eq 0 ] && echo "WAIT gpu-lock held, waiting for $NVVSR_GPU_LOCK"
    sleep 5
    waited=$((waited + 5))
  done
fi

# Python suites (host protocol, host + mpv)
py_out=$(python3 run_py.py 2>&1)
py_rc=$?
printf '%s\n' "$py_out" | grep -E '^(PASS|FAIL|SKIP) '
printf '%s\n' "$py_out" | grep -E '^PYTHON SUMMARY' || echo "PYTHON SUMMARY: crashed (rc $py_rc)"
if [ "$py_rc" -ne 0 ] && ! printf '%s\n' "$py_out" | grep -q '^FAIL '; then
  printf '%s\n' "$py_out" | tail -20
fi

# Node suites (extension logic, manifest lint); TAP lines become PASS/FAIL lines
node_out=$(cd "$HERE/.." && node --test --test-reporter=tap tests/logic.test.js tests/manifest.test.js 2>&1)
node_rc=$?
printf '%s\n' "$node_out" | grep -E '^(not )?ok [0-9]+ - ' | sed -E 's/^ok [0-9]+ - /PASS /; s/^not ok [0-9]+ - /FAIL /'
node_pass=$(printf '%s\n' "$node_out" | grep -c -E '^ok [0-9]+ - ')
node_fail=$(printf '%s\n' "$node_out" | grep -c -E '^not ok [0-9]+ - ')
echo "NODE SUMMARY: $node_pass PASS, $node_fail FAIL (node --test exit $node_rc)"

py_pass=$(printf '%s\n' "$py_out" | grep -c -E '^PASS ')
py_fail=$(printf '%s\n' "$py_out" | grep -c -E '^FAIL ')
py_skip=$(printf '%s\n' "$py_out" | grep -c -E '^SKIP ')
echo "SUMMARY: $((py_pass + node_pass)) PASS, $((py_fail + node_fail)) FAIL, $py_skip SKIP"

if [ "$py_rc" -ne 0 ] || [ "$node_rc" -ne 0 ] || [ "$((py_fail + node_fail))" -ne 0 ]; then
  exit 1
fi
exit 0
