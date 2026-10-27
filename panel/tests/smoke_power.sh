#!/bin/bash
# Smoke test of the Power page. Runs `nvcontrol --smoke` three times inside the headless cage of fake_env.sh (fake
# sysfs, fake nvidia-smi/kitty that log their calls; no window on the desktop, the real GPU is never woken):
#   1. GPU suspended   -> no nvidia-smi call, the "asleep" card is shown, no terminal
#   2. GPU active      -> limits from the driver-615 fixture, a limit above the maximum is refused inline,
#                         no terminal opens without a click
#   3. GPU active, NVC_SMOKE_POWER_RUN=140 -> the run button opens exactly one terminal with the shown command,
#                         and the page reads again once the terminal exits
#   usage: smoke_power.sh PATH/TO/nvcontrol      Needs cage and jq. Prints PASS/FAIL lines; exit 0 if all pass.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
[ $# -eq 1 ] && [ -x "$1" ] || { echo "usage: smoke_power.sh PATH/TO/nvcontrol" >&2; exit 2; }
app=$(realpath "$1")
for t in cage jq; do command -v "$t" >/dev/null || { echo "smoke_power.sh: $t is not installed" >&2; exit 2; }; done

scratch=$(mktemp -d "${TMPDIR:-/tmp}/nvc-smoke-power.XXXXXX") || exit 2
trap 'rm -rf "$scratch"' EXIT
fails=0 passes=0

check() {  # DESCRIPTION EXPECTED ACTUAL
  if [ "$2" = "$3" ]; then echo "PASS: $1"; passes=$((passes + 1))
  else echo "FAIL: $1 (expected '$2', got '$3')"; fails=$((fails + 1)); fi
}

# run NAME SECONDS RUNTIME [VAR=VALUE...]: files $scratch/NAME.{json,calls,calls.term,log}
run() {
  local name=$1 secs=$2 rt=$3
  shift 3
  timeout 90 "$HERE/fake_env.sh" --runtime "$rt" --calls "$scratch/$name.calls" -- \
    env "$@" "$app" --smoke "$secs" "$scratch/$name.json" > "$scratch/$name.log" 2>&1
  echo "$?" > "$scratch/$name.rc"
}
res() { jq -r ".results.$2 // \"<missing>\"" "$scratch/$1.json" 2>/dev/null || echo "<no json>"; }
count() { grep -c -- "$2" "$scratch/$1" || true; }
clean_log() {  # GTK/Adw warnings in the app log (cage's own timestamped lines and its exit message are not the app's)
  grep -E 'WARNING|CRITICAL|Gtk-|Adw-|GLib-|Gdk-|Pango' "$scratch/$1.log" | grep -vE '^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+ \[|Lost connection to Wayland' | head -3
}

run asleep 6 suspended
check "asleep: app exits 0" 0 "$(cat "$scratch/asleep.rc")"
check "asleep: no nvidia-smi call" 0 "$(count asleep.calls '^nvidia-smi')"
check "asleep: the asleep card is shown" true "$(res asleep power_asleep_card)"
check "asleep: no limits were read" "<missing>" "$(res asleep power_limit)"
check "asleep: no terminal" 0 "$(count asleep.calls.term .)"
check "asleep: no smoke errors" 0 "$(jq '.errors | length' "$scratch/asleep.json")"
check "asleep: log has no GTK warnings" "" "$(clean_log asleep)"

run awake 6 active
check "awake: app exits 0" 0 "$(cat "$scratch/awake.rc")"
check "awake: the asleep card is hidden" false "$(res awake power_asleep_card)"
check "awake: power limit" "150 W" "$(res awake power_limit)"
check "awake: default power limit" "115 W" "$(res awake power_default)"
check "awake: allowed range (min 1 W, max 150 W)" "1 to 150 W" "$(res awake power_range)"
check "awake: clock limiters" "Idle" "$(res awake power_limiters)"
check "awake: persistence mode from --query-gpu" "Disabled" "$(res awake power_persist)"
check "awake: 151 W is refused inline" "Cannot use 151 W: above the card maximum of 150 W" "$(res awake power_over_max_error)"
check "awake: run button is off at 151 W" false "$(res awake power_over_max_run_sensitive)"
check "awake: one nvidia-smi -q read" 1 "$(count awake.calls '^nvidia-smi -q -d POWER,CLOCK,PERFORMANCE')"
check "awake: no terminal without a click (not even at 151 W)" 0 "$(count awake.calls.term .)"
check "awake: no smoke errors" 0 "$(jq '.errors | length' "$scratch/awake.json")"
check "awake: log has no GTK warnings" "" "$(clean_log awake)"

run click 6 active NVC_SMOKE_POWER_RUN=140
check "click: app exits 0" 0 "$(cat "$scratch/click.rc")"
check "click: the preview shows the command" "sudo nvidia-smi -pl 140" "$(res click power_run_cmd)"
check "click: the fake kitty got exactly this one call" "kitty --hold -e sudo nvidia-smi -pl 140" "$(cat "$scratch/click.calls.term")"
check "click: the page reads again after the terminal exits" 2 "$(count click.calls '^nvidia-smi -q -d POWER,CLOCK,PERFORMANCE')"
check "click: log has no GTK warnings" "" "$(clean_log click)"

echo "$passes passed, $fails failed"
[ "$fails" -eq 0 ]
