#!/bin/bash
# About page smoke test: runs `nvcontrol --smoke 4 OUT` through fake_env.sh (headless cage, fake sysfs and tools), where
# the About page's on_smoke hook clicks "Copy diagnostics" and reports the report and the clipboard check.
#   usage: smoke_about.sh [path/to/nvcontrol]     (needs cage, jq; hyprctl optional for the window count)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
bin=${1:-$HERE/../../build/panel-p11/nvcontrol}
[ -x "$bin" ] || { echo "smoke_about.sh: no executable $bin" >&2; exit 2; }
bin=$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")
command -v jq >/dev/null || { echo "smoke_about.sh: jq is not installed" >&2; exit 2; }

work=$(mktemp -d "${TMPDIR:-/tmp}/nvc-about.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT
home=$work/home calls=$work/calls.log result=$work/result.json report=$work/report.txt
mkdir -p "$home/.config/nvvsr"

fails=0 passes=0
ok() { passes=$((passes + 1)); echo "PASS: $1"; }
bad() { fails=$((fails + 1)); echo "FAIL: $1"; }
check() { if "${@:2}" >/dev/null; then ok "$1"; else bad "$1"; fi; }
absent() { ! grep -Fq -- "$1" "$report"; }

# The GPU is asleep (suspended) for the whole run: no page of the app may wake it, so the calls log must stay empty.
# Windows on the real desktop (read only). Without a Hyprland session the check is skipped, not passed.
desktop_windows() {
  command -v hyprctl >/dev/null || return 1
  hyprctl -j clients 2>/dev/null | jq 'length' 2>/dev/null
}
before=$(desktop_windows)

"$HERE/fake_env.sh" --runtime suspended --home "$home" --calls "$calls" -- "$bin" --smoke 4 "$result" >"$work/app.log" 2>&1
rc=$?
after=$(desktop_windows)

check "nvcontrol --smoke exits 0 and writes the result (rc=$rc)" test "$rc" -eq 0 -a -s "$result"
[ -s "$result" ] || { echo "FAIL: no result file; app log:"; tail -20 "$work/app.log"; echo "$fails failed"; exit 1; }

check "the smoke run reports no errors" jq -e '.errors == []' "$result"
check "the app log has no GTK warnings" test -z "$(grep -E "WARNING|CRITICAL" "$work/app.log")"
check "the About page reports 10 version rows" jq -e '.results.version_rows == "10"' "$result"
check "Copy diagnostics wrote a report" jq -e '(.results.report | length) > 200' "$result"
check "the report length matches the copied text" jq -e \
  '(.results.report | length) == (.results.report_length | tonumber)' "$result"
check "the clipboard text equals the report" jq -e '.results.clipboard_matches == "yes"' "$result"

jq -r '.results.report // ""' "$result" > "$report"
for name in "NV Control" libnvvsr "NVIDIA kernel driver" "NVIDIA VFX wheel" "Private mpv" "System mpv" VLC GTK \
  libadwaita Kernel; do
  check "report has the version row: $name" grep -Fq "  $name: " "$report"
done
check "the report shows the scratch home as ~ (redaction)" absent "$home"
check "the report has no nvidia-smi reading (the GPU was not queried)" \
  grep -Fq 'last reading: none' "$report"

smi=$(grep -c '^nvidia-smi' "$calls" 2>/dev/null); smi=${smi:-0}
check "nvidia-smi was never called (calls: $smi)" test "$smi" -eq 0

if [ -n "$before" ] && [ -n "$after" ]; then
  check "the desktop window count is unchanged ($before -> $after)" test "$before" -eq "$after"
else
  echo "SKIP: the desktop window count (no Hyprland session: hyprctl not usable)"
fi

echo "smoke_about: $passes passed, $fails failed"
[ "$fails" -eq 0 ]
