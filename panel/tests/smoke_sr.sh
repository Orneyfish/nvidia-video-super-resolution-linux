#!/bin/bash
# Smoke test of the Super Resolution page (src/page_sr.cpp). Runs `nvcontrol --smoke 4 OUT` four times through
# fake_env.sh (headless cage, fake sysfs/pgrep/nvidia-smi; no window on the desktop) with a fake HOME that holds an
# install.json, a fake private mpv, a fake nvvsr_probe (next to the fake libnvvsr, so the real probe can never be
# found first) and a copy of mpv/script-opts/vsr.conf:
#   base          status rows (VLC module missing), NVC_SMOKE_EDIT=1 saves one option per editor, the probe is NOT pressed
#   probe ok      NVC_SMOKE_PROBE=1, the fake probe prints "ok"
#   probe fail    NVC_SMOKE_PROBE=1, the fake probe prints an error and exits 1
#   vlc running   --vlc-running + NVC_SMOKE_EDIT=1: the VLC editor must refuse and write nothing
#   usage: smoke_sr.sh PATH/TO/nvcontrol      Needs cage and jq (hyprctl optional, for the window count).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
[ $# -eq 1 ] && [ -x "$1" ] || { echo "usage: smoke_sr.sh PATH/TO/nvcontrol" >&2; exit 2; }
bin=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
for t in cage jq; do command -v "$t" >/dev/null || { echo "smoke_sr.sh: $t is not installed" >&2; exit 2; }; done

work=$(mktemp -d "${TMPDIR:-/tmp}/nvc-sr.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT
fails=0 passes=0
ok() { passes=$((passes + 1)); echo "PASS: $1"; }
bad() { fails=$((fails + 1)); echo "FAIL: $1"; }
check() { if "${@:2}" >/dev/null 2>&1; then ok "$1"; else bad "$1"; fi; }
eq() {  # DESCRIPTION EXPECTED ACTUAL
  if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (expected '$2', got '$3')"; fi
}

# the real config the page could ever be pointed at by mistake
real_fingerprint() {
  find "$HOME/.config/mpv/script-opts" "$HOME/.config/vlc" "$HOME/.config/nvvsr" "$HOME/.local/share/nvvsr" \
    -maxdepth 1 \( -name 'vsr.conf*' -o -name 'vlcrc*' -o -name 'panel.json*' -o -name install.json -o -name libdir \) \
    -printf '%p %T@ %s\n' 2>/dev/null | sort | sha1sum
}
desktop_windows() {  # read only; without a Hyprland session the check is skipped, not passed
  command -v hyprctl >/dev/null || return 1
  hyprctl -j clients 2>/dev/null | jq 'length' 2>/dev/null
}
fp_before=$(real_fingerprint)
win_before=$(desktop_windows)

# new_world NAME: sets $home and $repo. An installation with everything but the VLC module.
new_world() {
  local d=$work/$1 vfx wheel
  home=$d/home repo=$d/repo
  vfx=$home/venv/site-packages/nvvfx/libs wheel=$home/venv/site-packages/nvidia_vfx.libs
  mkdir -p "$vfx" "$wheel" "$home/.config/nvvsr" "$home/.config/vapoursynth" "$home/.config/mpv/script-opts" \
    "$home/.config/vlc" "$home/.local/share/nvvsr" "$home/.local/bin" "$home/.local/opt/mpv-vsr/bin" \
    "$repo/core/include" "$repo/build/core" "$repo/vapoursynth/build"
  : > "$vfx/libnvVFXVideoSuperRes.so"; : > "$wheel/libcudart.so.12"
  echo "$vfx:$wheel" > "$home/.config/nvvsr/libdir"
  echo '"/usr/lib/libvsscript.so" = ["/usr/bin/true","/usr/lib/libtrue.so","1"]' > "$home/.config/vapoursynth/vapoursynth.toml"
  : > "$repo/build/core/libnvvsr.so"; : > "$repo/vapoursynth/build/libvsnvvsr.so"   # no vlc/build/libnvvsr_plugin.so
  printf '#!/bin/bash\nexit 0\n' > "$home/.local/bin/mpv-sr"
  # the private mpv: only `--vf=help` is ever asked of it
  printf '#!/bin/bash\necho "mpv $*" >> "$FAKE_CALLS"\necho "  vapoursynth  VapourSynth filter"\n' > "$home/.local/opt/mpv-vsr/bin/mpv"
  # the probe: logs its call, then answers as FAKE_PROBE says
  cat > "$repo/build/core/nvvsr_probe" <<'EOF'
#!/bin/bash
echo "probe NVVSR_LIBDIR=${NVVSR_LIBDIR-unset}" >> "$PROBE_LOG"
if [ "${FAKE_PROBE:-ok}" = ok ]; then echo ok; exit 0; fi
echo "no usable GPU: driver too old"
exit 1
EOF
  chmod +x "$home/.local/bin/mpv-sr" "$home/.local/opt/mpv-vsr/bin/mpv" "$repo/build/core/nvvsr_probe"
  cat > "$home/.local/share/nvvsr/install.json" <<EOF
{"schema": 1, "version": "0.1.0", "repo": "$repo", "prefix": "$home/.local", "home": "$home",
 "nvvsr_libdir": "$vfx:$wheel",
 "paths": {"libnvvsr": "$repo/build/core/libnvvsr.so", "vapoursynth_plugin": "$repo/vapoursynth/build/libvsnvvsr.so",
           "vlc_plugin": "$repo/vlc/build/libnvvsr_plugin.so", "mpv_wrapper": "$home/.local/bin/mpv-sr",
           "private_mpv": "$home/.local/opt/mpv-vsr/bin/mpv"},
 "components": {"core": {"status": "installed", "detail": ""}}, "entries": []}
EOF
  cp "$REPO/mpv/script-opts/vsr.conf" "$home/.config/mpv/script-opts/vsr.conf"
  cp "$home/.config/mpv/script-opts/vsr.conf" "$d/vsr.conf.orig"
  printf '# VLC media player configuration file\n[core] # VLC core\naudio-filter=normvol\n' > "$home/.config/vlc/vlcrc"
  cp "$home/.config/vlc/vlcrc" "$d/vlcrc.orig"
  : > "$d/probe.log"
}

# run NAME [fake_env options] -- [VAR=VALUE...]: files $work/NAME/{result.json,calls,app.log,rc}
run() {
  local name=$1 d=$work/$1
  shift
  local opts=()
  while [ "$1" != -- ]; do opts+=("$1"); shift; done
  shift
  timeout 90 "$HERE/fake_env.sh" --runtime suspended --home "$home" --calls "$d/calls" "${opts[@]}" -- \
    env NVC_REPO_DIR="$repo" PROBE_LOG="$d/probe.log" "$@" "$bin" --smoke 4 "$d/result.json" > "$d/app.log" 2>&1
  echo $? > "$d/rc"
}
res() { jq -r ".results.\"$2\" // \"<missing>\"" "$work/$1/result.json" 2>/dev/null || echo "<no json>"; }
count() { grep -c -- "$2" "$work/$1" || true; }
common() {  # the checks every run must pass
  local n=$1
  eq "$n: nvcontrol --smoke exits 0" 0 "$(cat "$work/$n/rc")"
  check "$n: the smoke run reports no errors" jq -e '.errors == [] and .icons_missing == []' "$work/$n/result.json"
  check "$n: the app log has no GTK warnings" test -z "$(grep -E 'WARNING|CRITICAL|Gtk-|Adw-|GLib-|Gdk-|Pango' "$work/$n/app.log" | grep -vE '^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]+ \[|Lost connection to Wayland' | head -3)"
  eq "$n: nvidia-smi was never called" 0 "$(count "$n/calls" '^nvidia-smi')"
}

# --- base: status card, editors, no probe ---------------------------------------------------------------------------
new_world base; base_home=$home
run base -- NVC_SMOKE_EDIT=1
common base
eq "status: headline (the VLC module is missing) and installed mode" "ok | Ready for mpv | Installed (version 0.1.0)" "$(res base sr_row_headline)"
for id in libnvvsr libdir vsplugin mpv mpv_sr vsscript; do
  check "status: $id is OK" test "$(res base "sr_row_$id" | cut -c1-5)" = "ok | "
done
eq "status: the recorded libnvvsr shows as a development build" \
  "ok | libnvvsr (core library) | $repo/build/core/libnvvsr.so (development build)" "$(res base sr_row_libnvvsr)"
eq "status: the missing VLC module shows its fix hint" \
  "missing | VLC module (libnvvsr_plugin.so) | Build it (cmake -S vlc -B vlc/build) or run install/install.sh." "$(res base sr_row_vlc)"
eq "status: mpv --vf=help ran exactly once" 1 "$(count base/calls 'mpv --no-config --vf=help')"
eq "check button: sensitive once the status is in" true "$(res base sr_check_button_sensitive)"
eq "probe: not run without the press (no result)" "<missing>" "$(res base sr_probe)"
eq "probe: not run without the press (the fake was not called)" 0 "$(wc -l < "$work/base/probe.log")"

check "mpv editor: saved" test "$(res base sr_mpv_message | cut -c1-6)" = "Saved."
sed 's/^#osd_size=11$/osd_size=20/' "$work/base/vsr.conf.orig" > "$work/base/vsr.conf.want"
check "mpv editor: only osd_size changed in vsr.conf, comments and order preserved" \
  cmp -s "$base_home/.config/mpv/script-opts/vsr.conf" "$work/base/vsr.conf.want"
check "mpv editor: .bak-nvvsr holds the original" \
  cmp -s "$base_home/.config/mpv/script-opts/vsr.conf.bak-nvvsr" "$work/base/vsr.conf.orig"
check "vlc editor: saved" test "$(res base sr_vlc_message | cut -c1-6)" = "Saved."
{ cat "$work/base/vlcrc.orig"; printf '\n[nvvsr]\nnvvsr-min-height=480\n'; } > "$work/base/vlcrc.want"
check "vlc editor: [nvvsr] section added to vlcrc, the rest untouched" \
  cmp -s "$base_home/.config/vlc/vlcrc" "$work/base/vlcrc.want"
check "vlc editor: .bak-nvvsr holds the original" cmp -s "$base_home/.config/vlc/vlcrc.bak-nvvsr" "$work/base/vlcrc.orig"
check "VLC was asked about (pgrep -x vlc) before writing" grep -q '^pgrep -x vlc' "$work/base/calls"

# --- the probe ------------------------------------------------------------------------------------------------------
new_world ok
run ok -- NVC_SMOKE_PROBE=1 FAKE_PROBE=ok
common ok
eq "probe ok: summary and detail" "Super resolution works: ok" "$(res ok sr_probe)"
eq "probe ok: reported as ok" true "$(res ok sr_probe_ok)"
eq "probe ok: the fake probe ran once, with the libdir file's directories" \
  "probe NVVSR_LIBDIR=$home/venv/site-packages/nvvfx/libs:$home/venv/site-packages/nvidia_vfx.libs" "$(cat "$work/ok/probe.log")"
check "probe ok: no editor was touched without NVC_SMOKE_EDIT" \
  cmp -s "$home/.config/mpv/script-opts/vsr.conf" "$work/ok/vsr.conf.orig"

new_world fail
run fail -- NVC_SMOKE_PROBE=1 FAKE_PROBE=fail
common fail
eq "probe fail: Not usable with the probe's text" "Not usable: no usable GPU: driver too old" "$(res fail sr_probe)"
eq "probe fail: reported as not ok" false "$(res fail sr_probe_ok)"
eq "probe fail: the fake probe ran once" 1 "$(wc -l < "$work/fail/probe.log")"

# --- VLC running: the editor refuses -------------------------------------------------------------------------------
new_world busy; busy_home=$home
run busy --vlc-running -- NVC_SMOKE_EDIT=1
common busy
check "vlc running: the VLC editor refuses with a message" \
  test "$(res busy sr_vlc_message | cut -c1-14)" = "VLC is running"
check "vlc running: vlcrc is unchanged and no backup was made" \
  test "$(cmp "$busy_home/.config/vlc/vlcrc" "$work/busy/vlcrc.orig" 2>&1)" = "" -a ! -e "$busy_home/.config/vlc/vlcrc.bak-nvvsr"
check "vlc running: the mpv editor still saves" test "$(res busy sr_mpv_message | cut -c1-6)" = "Saved."

# --- the outside world ----------------------------------------------------------------------------------------------
check "the real mpv/vlc/nvvsr config was not touched" test "$fp_before" = "$(real_fingerprint)"
win_after=$(desktop_windows)
if [ -n "$win_before" ] && [ -n "$win_after" ]; then
  eq "the desktop window count is unchanged" "$win_before" "$win_after"
else
  echo "SKIP: the desktop window count (no Hyprland session: hyprctl not usable)"
fi

echo "smoke_sr: $passes passed, $fails failed"
[ "$fails" -eq 0 ]
