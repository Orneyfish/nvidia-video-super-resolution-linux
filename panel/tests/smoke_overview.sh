#!/bin/bash
# Smoke test of the Overview page. Runs `nvcontrol --smoke 8` five times inside the headless cage of fake_env.sh (fake
# sysfs, fake nvidia-smi/supergfxctl that log their calls; no window on the desktop, the real GPU is never woken):
#   1. GPU suspended                    -> 0 nvidia-smi calls, the asleep card replaces the tiles
#   2. GPU suspended, NVC_SMOKE_WAKE=1  -> "Wake and read" pressed once: exactly 1 --query-gpu, the tiles show the reading
#   3. GPU active                       -> tiles, detail rows and the process row from the fake CSV
#   4. GPU active, NVC_SMOKE_POLL=5   -> the choice is saved in panel.json, never more queries than the 2 s default
#   5. GPU active, NVC_SMOKE_POLL=0   -> "Off" is saved and polling stops: fewer queries than the default (what is left
#                                         is the smoke run's own two F5 refreshes and the Power page's persistence read)
#   usage: smoke_overview.sh PATH/TO/nvcontrol      Needs cage and jq. Prints PASS/FAIL lines; exit 0 if all pass.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
[ $# -eq 1 ] && [ -x "$1" ] || { echo "usage: smoke_overview.sh PATH/TO/nvcontrol" >&2; exit 2; }
app=$(realpath "$1")
for t in cage jq; do command -v "$t" >/dev/null || { echo "smoke_overview.sh: $t is not installed" >&2; exit 2; }; done

scratch=$(mktemp -d "${TMPDIR:-/tmp}/nvc-smoke-overview.XXXXXX") || exit 2
trap 'rm -rf "$scratch"' EXIT
fails=0 passes=0

check() {  # DESCRIPTION EXPECTED ACTUAL
  if [ "$2" = "$3" ]; then echo "PASS: $1"; passes=$((passes + 1))
  else echo "FAIL: $1 (expected '$2', got '$3')"; fails=$((fails + 1)); fi
}

# run NAME RUNTIME [VAR=VALUE...]: files $scratch/NAME.{json,calls,log,rc}, fake HOME $scratch/NAME.home
run() {
  local name=$1 rt=$2
  shift 2
  timeout 90 "$HERE/fake_env.sh" --runtime "$rt" --calls "$scratch/$name.calls" --home "$scratch/$name.home" -- \
    env "$@" "$app" --smoke 8 "$scratch/$name.json" > "$scratch/$name.log" 2>&1
  echo "$?" > "$scratch/$name.rc"
}
res() { jq -r ".results.$2 // \"<missing>\"" "$scratch/$1.json" 2>/dev/null || echo "<no json>"; }
count() { grep -c -- "$2" "$scratch/$1" || true; }
saved_interval() { jq -r '.poll_interval // "<missing>"' "$scratch/$1.home/.config/nvvsr/panel.json" 2>/dev/null || echo "<no file>"; }
clean_log() {  # GTK/Adw warnings in the app log (cage's own timestamped lines and its exit message are not the app's)
  grep -E 'WARNING|CRITICAL|Gtk-|Adw-|GLib-|Gdk-|Pango' "$scratch/$1.log" | grep -vE '^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+ \[|Lost connection to Wayland' | head -3
}
desktop_windows() {  # windows on the real desktop (read only); empty without a Hyprland session
  command -v hyprctl >/dev/null || return 1
  hyprctl -j clients 2>/dev/null | jq 'length' 2>/dev/null
}
before=$(desktop_windows)

run asleep suspended
check "asleep: app exits 0" 0 "$(cat "$scratch/asleep.rc")"
check "asleep: no nvidia-smi call" 0 "$(count asleep.calls '^nvidia-smi')"
check "asleep: the asleep card is shown" true "$(res asleep overview_asleep_card)"
check "asleep: the tiles are hidden" false "$(res asleep overview_tiles)"
check "asleep: state chip" Asleep "$(res asleep overview_chip)"
check "asleep: driver and PCI address come from sysfs" "Driver 615.78.08 - PCI 0000:01:00.0" "$(res asleep overview_sub)"
check "asleep: hybrid mode is still read (supergfxctl does not touch the GPU)" Hybrid "$(res asleep overview_hybrid)"
check "asleep: runtime state" "suspended, control: auto, D3cold" "$(res asleep overview_runtime)"
check "asleep: processes not read yet" "Not read yet" "$(res asleep overview_procs)"
check "asleep: default refresh interval is 2 s" 2 "$(res asleep overview_poll)"
check "asleep: panel.json is not written without a change" "<no file>" "$(saved_interval asleep)"
check "asleep: no smoke errors" 0 "$(jq '.errors | length' "$scratch/asleep.json")"
check "asleep: log has no GTK warnings" "" "$(clean_log asleep)"

run wake suspended NVC_SMOKE_WAKE=1
check "wake: app exits 0" 0 "$(cat "$scratch/wake.rc")"
check "wake: the asleep card was shown before the press" true "$(res wake overview_pre_wake_card)"
check "wake: exactly one --query-gpu after one press" 1 "$(count wake.calls '--query-gpu')"
check "wake: the card gives way to the tiles with the reading" false "$(res wake overview_wake_card)"
check "wake: the tiles show the reading" "1312 / 8192 MiB" "$(res wake overview_wake_mem)"
check "wake: the next state tick shows the card again (the fake GPU is still suspended)" true "$(res wake overview_asleep_card)"
check "wake: no smoke errors" 0 "$(jq '.errors | length' "$scratch/wake.json")"
check "wake: log has no GTK warnings" "" "$(clean_log wake)"

run active active
check "active: app exits 0" 0 "$(cat "$scratch/active.rc")"
check "active: the asleep card is hidden" false "$(res active overview_asleep_card)"
check "active: the tiles are shown" true "$(res active overview_tiles)"
check "active: state chip" Awake "$(res active overview_chip)"
check "active: GPU name" "NVIDIA GeForce RTX 3070 Ti Laptop GPU" "$(res active overview_title)"
check "active: driver and PCI address" "Driver 615.78.08 - PCI 0000:01:00.0" "$(res active overview_sub)"
check "active: memory tile" "1312 / 8192 MiB" "$(res active overview_mem)"
check "active: memory bar" 0.16 "$(res active overview_mem_bar)"
check "active: utilisation tile" "12 %" "$(res active overview_util)"
check "active: temperature tile" "47 °C" "$(res active overview_temp)"
check "active: temperature bar" 0.47 "$(res active overview_temp_bar)"
check "active: power tile" "18.5 W" "$(res active overview_power)"
check "active: power limit" "limit 125 W" "$(res active overview_power_sub)"
check "active: graphics clock" "1230 MHz" "$(res active overview_gclk)"
check "active: memory clock" "6000 MHz" "$(res active overview_mclk)"
check "active: performance state" P0 "$(res active overview_pstate)"
check "active: PCIe link" "Gen 4 x16" "$(res active overview_pcie)"
check "active: fan ([N/A] in the CSV)" "not reported" "$(res active overview_fan)"
check "active: hybrid mode" Hybrid "$(res active overview_hybrid)"
check "active: power source" "AC power" "$(res active overview_source)"
check "active: runtime power management" "active, control: auto, D0" "$(res active overview_runtime)"
check "active: process row" "mpv PID 4242 512 MiB" "$(res active overview_procs)"
check "active: readings are not marked stale" false "$(res active overview_stale)"
check "active: no error text" "" "$(res active overview_error)"
check "active: refresh interval 2 s" 2 "$(res active overview_poll)"
check "active: no smoke errors" 0 "$(jq '.errors | length' "$scratch/active.json")"
check "active: log has no GTK warnings" "" "$(clean_log active)"
base=$(count active.calls '--query-gpu')

run poll5 active NVC_SMOKE_POLL=5
check "5 s: app exits 0" 0 "$(cat "$scratch/poll5.rc")"
check "5 s: the row shows 5" 5 "$(res poll5 overview_poll)"
check "5 s: panel.json has poll_interval 5" 5 "$(saved_interval poll5)"
n=$(count poll5.calls '--query-gpu')
check "5 s: no more --query-gpu calls than the 2 s default ($n <= $base)" ok "$([ "$n" -ge 1 ] && [ "$n" -le "$base" ] && echo ok || echo "$n")"
check "5 s: log has no GTK warnings" "" "$(clean_log poll5)"

run poll0 active NVC_SMOKE_POLL=0
check "Off: app exits 0" 0 "$(cat "$scratch/poll0.rc")"
check "Off: the row shows Off (0)" 0 "$(res poll0 overview_poll)"
check "Off: panel.json has poll_interval 0" 0 "$(saved_interval poll0)"
n=$(count poll0.calls '--query-gpu')
check "Off: fewer --query-gpu calls than the 2 s default ($n < $base)" ok "$([ "$n" -ge 1 ] && [ "$n" -lt "$base" ] && echo ok || echo "$n")"
check "Off: log has no GTK warnings" "" "$(clean_log poll0)"

after=$(desktop_windows)
if [ -n "$before" ] && [ -n "$after" ]; then
  check "the desktop window count is unchanged" "$before" "$after"
else
  echo "SKIP: the desktop window count (no Hyprland session: hyprctl not usable)"
fi

echo "smoke_overview: $passes passed, $fails failed"
[ "$fails" -eq 0 ]
