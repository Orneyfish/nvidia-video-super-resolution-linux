#!/bin/bash
# Headless smoke test of the settings editor (src/ui_editor.cpp) and the badge preview (src/ui_badge.cpp): drives
# editor_harness inside cage (run_cage.sh: scratch HOME, no desktop window) against temp copies of mpv's vsr.conf and
# a vlcrc, then judges the files and the harness' json line. Needs cage and jq. Never touches the real config.
#   usage: smoke_editor.sh PATH/TO/editor_harness      (NVC_SHOT_DIR=dir keeps a PNG of every run there)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
[ $# -eq 1 ] && [ -x "$1" ] || { echo "usage: smoke_editor.sh PATH/TO/editor_harness" >&2; exit 2; }
HARNESS=$(realpath "$1")
for tool in cage jq; do command -v "$tool" >/dev/null || { echo "smoke_editor.sh: $tool is not installed" >&2; exit 2; }; done

T=$(mktemp -d "${TMPDIR:-/tmp}/nvc-editor.XXXXXX") || exit 2
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { echo "PASS: $1"; pass=$((pass + 1)); }
bad() { echo "FAIL: $1"; fail=$((fail + 1)); }
check() { local name=$1; shift; if "$@" >/dev/null 2>&1; then ok "$name"; else bad "$name"; fi; }

# the real config files the editor could ever be pointed at by mistake
real_fingerprint() {
  find "$HOME/.config/mpv/script-opts" "$HOME/.config/vlc" -maxdepth 1 \( -name 'vsr.conf*' -o -name 'vlcrc*' \) \
    -printf '%p %T@ %s\n' 2>/dev/null | sort | sha1sum
}
before=$(real_fingerprint)

# run NAME KIND SCENARIO FILE: sets $json to the harness' result line (or {} when there is none)
run() {
  local name=$1 kind=$2 scen=$3 file=$4 shot=()
  [ -n "${NVC_SHOT_DIR:-}" ] && shot=(--shot "$NVC_SHOT_DIR/$name.png")
  json=$T/$name.json
  "$HERE/run_cage.sh" -- "$HARNESS" --kind "$kind" --scenario "$scen" "${shot[@]}" "$file" >"$T/$name.out" 2>"$T/$name.err"
  local rc=$?
  grep -m1 '^{' "$T/$name.out" >"$json" || echo '{}' >"$json"
  check "$name: harness exits 0 without an error" test "$rc" -eq 0 -a "$(jq -r '.error // "none"' "$json")" = none
  check "$name: no GTK warnings" test "$(grep -cE 'WARNING|CRITICAL' "$T/$name.err")" -eq 0
  [ "$rc" -eq 0 ] || { echo "  rc=$rc error=$(jq -r '.error // ""' "$json")"; tail -n 5 "$T/$name.err" | sed 's/^/  /'; }
}
j() { jq -e "$1" "$json" >/dev/null 2>&1; }  # an expression on the last result

new_mpv() {  # a vsr.conf with an unknown key and an active value of its own, next to the documented defaults
  mkdir -p "$T/$1"
  sed -e '/^#enabled=yes$/a my_unknown_key=keep me # note' -e 's/^#quality=auto$/quality=high/' \
    "$REPO/mpv/script-opts/vsr.conf" >"$T/$1/vsr.conf"
  cp "$T/$1/vsr.conf" "$T/$1/orig"
}

# --- mpv: switch + spin, Save ------------------------------------------------------------------------------------
new_mpv save
run mpv-save mpv save "$T/save/vsr.conf"
check "mpv save: Save and Revert start disabled" j '.save_initial == false and .revert_initial == false'
check "mpv save: an edit enables Save and shows the row's Default button" j '.save_after_edit == true and .default_shown == true'
check "mpv save: success message, toast, on_saved, Save disabled again" \
  j '.message_error == false and (.message | startswith("Saved.")) and .toasts == "Saved mpv (vsr.conf)" and .on_saved == 1 and .save_after_save == false'
check "mpv save: the editor shows the file after the reload" j '.switch_after == false and .spin_after == 20'
sed -e 's/^#enabled=yes$/enabled=no/' -e 's/^#osd_size=11$/osd_size=20/' "$T/save/orig" >"$T/save/expected"
check "mpv save: exactly the two keys changed; comments, unknown key, active key and order intact" \
  cmp -s "$T/save/vsr.conf" "$T/save/expected"
check "mpv save: .bak-nvvsr holds the original" cmp -s "$T/save/vsr.conf.bak-nvvsr" "$T/save/orig"
check "mpv save: the editor was told about every change (preview feed)" j '.on_change == true'
check "badge: 16:9 preview" j '.badge_aspect > 1.7 and .badge_aspect < 1.85'
check "badge: corner 9 is top right" j '.badge_pos9.count > 0 and .badge_pos9.cx > 0.5 and .badge_pos9.cy < 0.5'
check "badge: corner 1 is bottom left" j '.badge_pos1.count > 0 and .badge_pos1.cx < 0.5 and .badge_pos1.cy > 0.5'
check "badge: corner 5 is the centre" j '.badge_pos5.count > 0 and (.badge_pos5.cx - 0.5 | fabs) < 0.1 and (.badge_pos5.cy - 0.5 | fabs) < 0.1'
check "badge: size 40 draws more than size 11" j '.badge_size40.count > 2 * .badge_size11.count'
check "badge: opacity 0.3 is dimmer than 1.0" j '.badge_opacity03.peak < .badge_size11.peak - 60'

# --- mpv: reset one row to its default, Save: back to the original text -------------------------------------------
new_mpv default
run mpv-default mpv default "$T/default/vsr.conf"
check "mpv default: Default button works, Save enabled, button fades out after saving" \
  j '.spin_saved == 20 and .save_after_default == true and .default_hidden == true and .spin_after == 11 and .save_after_save == false'
check "mpv default: two saves, two toasts" j '.on_saved == 2 and .toasts == "Saved mpv (vsr.conf)|Saved mpv (vsr.conf)"'
check "mpv default: the file is byte-identical to the original again (#osd_size=11 is back)" \
  cmp -s "$T/default/vsr.conf" "$T/default/orig"

# --- vlc: [nvvsr] section, 1/0 booleans, a documented key and a missing key ----------------------------------------
mkdir -p "$T/vlc"
cat >"$T/vlc/orig" <<'EOF'
# VLC media player configuration file
# unknown stuff stays

[core] # VLC core
audio-filter=normvol

[nvvsr] # NVIDIA VSR video output
#nvvsr-min-height=360
nvvsr-quality=high
unknown-nvvsr-key=keep

[qt] # Qt interface
qt-recents=file:///x
EOF
cp "$T/vlc/orig" "$T/vlc/vlcrc"
cat >"$T/vlc/expected" <<'EOF'
# VLC media player configuration file
# unknown stuff stays

[core] # VLC core
audio-filter=normvol

[nvvsr] # NVIDIA VSR video output
nvvsr-min-height=480
nvvsr-quality=high
unknown-nvvsr-key=keep
nvvsr-ac-only=0

[qt] # Qt interface
qt-recents=file:///x
EOF
run vlc-save vlc save "$T/vlc/vlcrc"
check "vlc save: toast and reload" j '.toasts == "Saved VLC (vlcrc)" and .switch_after == false and .spin_after == 480'
check "vlc save: keys land inside [nvvsr] (0/1 style), everything else untouched" cmp -s "$T/vlc/vlcrc" "$T/vlc/expected"
check "vlc save: .bak-nvvsr holds the original" cmp -s "$T/vlc/vlcrc.bak-nvvsr" "$T/vlc/orig"

# --- blocked(): VLC running -------------------------------------------------------------------------------------
mkdir -p "$T/blocked"
cp "$T/vlc/orig" "$T/blocked/vlcrc"
run vlc-blocked vlc blocked "$T/blocked/vlcrc"
check "blocked: inline error, no toast, Save stays enabled" \
  j '.message_error == true and (.message | startswith("VLC is running")) and .toasts == "" and .on_saved == 0 and .save_after_save == true'
check "blocked: the file is untouched and no backup was made" \
  test -z "$(cmp "$T/blocked/vlcrc" "$T/vlc/orig" 2>&1)" -a ! -e "$T/blocked/vlcrc.bak-nvvsr"

# --- a change behind the editor's back: dialog, Reload keeps the disk, Overwrite writes only our key --------------
new_mpv conflict
run mpv-conflict mpv conflict "$T/conflict/vsr.conf"
check "conflict: the dialog is shown instead of writing, with the option's name" \
  j '.dialog1_heading == "File changed outside NV Control" and (.dialog1_body | contains("Automatic super resolution")) and (.message | startswith("The file changed"))'
check "conflict: Reload shows the value found on disk and leaves nothing to save" j '.switch_after_reload == true and .save_after_reload == false'
check "conflict: a second conflict offers the dialog again; Overwrite saves our value" \
  j '.dialog2_heading == "File changed outside NV Control" and .spin_after_overwrite == 20 and .save_after_overwrite == false and (.message_after_overwrite | startswith("Saved.")) and .toasts == "Saved mpv (vsr.conf)" and .on_saved == 1'
{ cat "$T/conflict/orig"; echo "enabled=yes"; echo "osd_size=20"; } >"$T/conflict/expected"
check "conflict: the other edit (enabled=yes) survived, only osd_size was overwritten" \
  cmp -s "$T/conflict/vsr.conf" "$T/conflict/expected"
check "conflict: Reload never wrote our enabled=no" test "$(grep -cx 'enabled=no' "$T/conflict/vsr.conf")" -eq 0
check "conflict: the backup is the file as it was before the overwrite" grep -qx 'osd_size=25' "$T/conflict/vsr.conf.bak-nvvsr"

# --- the real home --------------------------------------------------------------------------------------------
check "the real mpv/vlc config files were not touched" test "$before" = "$(real_fingerprint)"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
