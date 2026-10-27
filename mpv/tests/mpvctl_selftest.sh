#!/bin/bash
# Self-test for mpvctl: starts a headless mpv and checks get/set/cmd, errors, long socket paths, stdout hygiene.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
: "${SCR:=$(mktemp -d)}"
MPV="$HOME/.local/opt/mpv-vsr/bin/mpv"
CTL="$SCR/mpvctl"
# The directory makes the full path longer than the 108-byte sun_path limit; the basename stays short.
SOCKDIR="$SCR/mpvctl-selftest-$(printf 'x%.0s' $(seq 1 90))"
SOCK="$SOCKDIR/mpv.sock"
pass=0
fail=0

result() {  # result NAME CONDITION_EXIT_STATUS
    if [ "$2" = 0 ]; then
        echo "PASS $1"
        pass=$((pass + 1))
    else
        echo "FAIL $1"
        fail=$((fail + 1))
    fi
}

mkdir -p "$SCR" "$SOCKDIR"
g++ -std=c++17 -O2 -Wall -Wextra -o "$CTL" "$HERE/mpvctl.cpp" 2>"$SCR/mpvctl-build.log"
result "compile without warnings" "$([ -x "$CTL" ] && [ ! -s "$SCR/mpvctl-build.log" ]; echo $?)"
if [ ! -x "$CTL" ]; then
    cat "$SCR/mpvctl-build.log" >&2
    echo "SUMMARY: $pass passed, $((fail + 1)) failed"
    exit 1
fi

result "socket path longer than 110 bytes" "$([ ${#SOCK} -gt 110 ]; echo $?)"

rm -f "$SOCK"
# mpv gets the basename and runs inside the directory, like the client does.
(cd "$SOCKDIR" && exec "$MPV" --no-config --idle=yes --vo=null --ao=null \
    --input-ipc-server="$(basename "$SOCK")") >"$SCR/mpvctl-selftest-mpv.log" 2>&1 &
MPV_PID=$!
trap 'kill "$MPV_PID" 2>/dev/null; wait "$MPV_PID" 2>/dev/null; rm -f "$SOCK"' EXIT

for _ in $(seq 1 100); do
    [ -S "$SOCK" ] && "$CTL" "$SOCK" get volume >/dev/null 2>&1 && break
    sleep 0.1
done

# mpv reports volume as a double ("100.000000"), so compare numerically.
num_is() {  # num_is VALUE N
    [ "$(printf '%.0f' "$1" 2>/dev/null)" = "$2" ]
}

out=$("$CTL" "$SOCK" get volume 2>/dev/null)
result "get volume = 100 (long socket path)" "$(num_is "$out" 100; echo $?)"

"$CTL" "$SOCK" get volume >"$SCR/mpvctl-stdout.txt" 2>/dev/null
result "stdout holds only one numeric data line" "$([ "$(wc -l <"$SCR/mpvctl-stdout.txt")" = 1 ] && grep -Eq '^[0-9.]+$' "$SCR/mpvctl-stdout.txt"; echo $?)"

"$CTL" "$SOCK" set volume 50 >/dev/null 2>&1
result "set volume 50 exits 0" "$?"
out=$("$CTL" "$SOCK" get volume 2>/dev/null)
result "get volume after set = 50" "$(num_is "$out" 50; echo $?)"

out=$("$CTL" "$SOCK" cmd '["get_property","pause"]' 2>/dev/null)
result "cmd round trip returns data (pause = false)" "$([ "$out" = false ]; echo $?)"

"$CTL" "$SOCK" get no-such-property-xyz >"$SCR/mpvctl-bad-out.txt" 2>"$SCR/mpvctl-bad-err.txt"
rc=$?
result "bad property exits 1" "$([ "$rc" = 1 ]; echo $?)"
result "bad property prints error on stderr only" "$([ ! -s "$SCR/mpvctl-bad-out.txt" ] && grep -q '^error: ' "$SCR/mpvctl-bad-err.txt"; echo $?)"

echo "SUMMARY: $pass passed, $fail failed"
[ "$fail" = 0 ]
