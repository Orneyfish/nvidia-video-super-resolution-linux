#!/bin/bash
# The battery rule on the real app: `nvcontrol --smoke 8` in the headless fake environment (fake_env.sh), with the
# fake nvidia-smi / supergfxctl logging every call.
#   suspended GPU: 0 nvidia-smi calls in 8 s.
#   active GPU:    2..5 `--query-gpu` calls in 8 s (interval 2 s, no loop), `supergfxctl -g` exactly once.
#   usage: smoke_poll.sh path/to/nvcontrol
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
app=${1:-}
[ -x "$app" ] || { echo "usage: smoke_poll.sh path/to/nvcontrol" >&2; exit 2; }
app=$(cd "$(dirname "$app")" && pwd)/$(basename "$app")

work=$(mktemp -d "${TMPDIR:-/tmp}/nvc-smoke-poll.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT
fails=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; fails=$((fails + 1)); }
count() { grep -c -- "$1" "$2" || true; }  # grep -c prints 0 but exits 1 on no match

run() {  # run RUNTIME NAME: the smoke run, calls logged to $work/NAME.calls
  "$HERE/fake_env.sh" --runtime "$1" --calls "$work/$2.calls" --home "$work/$2-home" -- \
    "$app" --smoke 8 "$work/$2.json" > "$work/$2.out" 2>&1
  local rc=$?
  [ "$rc" = 0 ] && [ -s "$work/$2.json" ] && pass "$2: smoke run exited 0 and wrote its result" \
    || { fail "$2: smoke run rc=$rc"; sed 's/^/    /' "$work/$2.out" | tail -5; }
}

run suspended suspended
n=$(count '^nvidia-smi' "$work/suspended.calls")
[ "$n" = 0 ] && pass "suspended GPU: 0 nvidia-smi calls" || fail "suspended GPU: $n nvidia-smi calls (want 0)"

run active active
n=$(count '--query-gpu' "$work/active.calls")
{ [ "$n" -ge 2 ] && [ "$n" -le 5 ]; } && pass "active GPU: $n --query-gpu calls in 8 s (want 2..5)" \
  || fail "active GPU: $n --query-gpu calls in 8 s (want 2..5)"
n=$(count '^supergfxctl -g' "$work/active.calls")
[ "$n" = 1 ] && pass "hybrid mode read once" || fail "supergfxctl -g called $n times (want 1)"
n=$(count '--query-compute-apps' "$work/active.calls")
{ [ "$n" -ge 1 ] && [ "$n" -le 3 ]; } && pass "process list read $n times (every ~6 s)" || fail "process list read $n times (want 1..3)"

[ "$fails" = 0 ] && echo "smoke_poll: all passed" || echo "smoke_poll: $fails failed"
[ "$fails" = 0 ]
