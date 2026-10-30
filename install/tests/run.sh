#!/usr/bin/env bash
# Tests of install/install.sh, install/lib/manifest.sh, install/build-mpv-vsr.sh (dry run only) and the installed
# layout of mpv/vsr.vpy. Prints PASS/FAIL per check; the exit status is the number of failures.
#
#   install/tests/run.sh
#
# GPU-free by construction: NVVSR_INSTALL_SKIP_GPU_CHECK=1, no libnvvsr probe, no player runs a video (only
# `mpv --vf=help` and `vlc --list`, headless). Everything happens in a scratch directory (NVVSR_TEST_TMP, default a
# mktemp -d) with --home pointing into it; the real home is checksummed before and after and must not change.
# sudo/pacman/paru/yay are shimmed and must never be called. NVVSR_TEST_MPV overrides the private mpv used for the
# `--vf=help` check (default ~/.local/opt/mpv-vsr/bin/mpv, only executed with --no-config).
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
INSTALL=$REPO/install/install.sh
BUILD_MPV=$REPO/install/build-mpv-vsr.sh
MANIFEST_SH=$REPO/install/lib/manifest.sh
REAL_HOME=$HOME
MPV_REAL=${NVVSR_TEST_MPV:-$REAL_HOME/.local/opt/mpv-vsr/bin/mpv}
export NVVSR_INSTALL_SKIP_GPU_CHECK=1

if [ -n "${NVVSR_TEST_TMP:-}" ]; then SCR=$NVVSR_TEST_TMP; mkdir -p "$SCR"
else SCR=$(mktemp -d "${TMPDIR:-/tmp}/nvvsr-install-test.XXXXXX"); trap 'rm -rf "$SCR"' EXIT; fi
export TMPDIR=$SCR/tmp
mkdir -p "$TMPDIR"

PASS=0; FAIL=0
ok() { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1${2:+: $2}"; FAIL=$((FAIL + 1)); }
eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$2', expected '$3'"; fi; }
t() { local n=$1; shift; if "$@" >/dev/null 2>&1; then ok "$n"; else bad "$n"; fi; }     # command succeeds
f() { local n=$1; shift; if "$@" >/dev/null 2>&1; then bad "$n"; else ok "$n"; fi; }     # command fails
has() { grep -q -- "$2" "$1"; }                                                          # has FILE REGEX
RC=0
run() { local log=$1; shift; "$INSTALL" "$@" >"$log" 2>&1; RC=$?; }                       # run LOG install.sh-args

# ---- shims: sudo and friends must never run ----------------------------------------------------------------------
SHIM=$SCR/shim; mkdir -p "$SHIM"
for tool in sudo pacman paru yay; do
  printf '#!/bin/sh\necho "%s $*" >>"%s/shim-called"\nexit 99\n' "$tool" "$SCR" >"$SHIM/$tool"; chmod 755 "$SHIM/$tool"
done
export PATH=$SHIM:$PATH

# ---- snapshots ---------------------------------------------------------------------------------------------------
snap() { # dir out : directories, symlinks, files (mode, size, mtime) and sha256 of every file; no directory mtimes
  ( cd "$1" && { find . -type d -printf 'd %p %m\n'; find . -type l -printf 'l %p -> %l\n'
                 find . -type f -printf 'f %p %m %s %T@\n'; find . -type f -print0 | sort -z | xargs -0 -r sha256sum
               } | LC_ALL=C sort ) >"$2"
}
snap_real() { # the places the installer may write in the real home
  local d
  for d in .config/mpv .config/vlc .config/vapoursynth .config/nvvsr .config/systemd .config/google-chrome/NativeMessagingHosts \
           .local/share/nvvsr .local/share/vlc .local/bin .local/share/applications/org.nvvsr.Control.desktop; do
    if [ -e "$REAL_HOME/$d" ]; then
      echo "== $d"
      ( cd "$REAL_HOME" && { find "$d" -printf '%p %y %m %s %T@ %l\n'; find "$d" -type f -print0 | sort -z | xargs -0 -r sha256sum; } | LC_ALL=C sort )
    else echo "== $d (absent)"; fi
  done
}
snap_real >"$SCR/real-before.txt"

LIBPY=$(ls -1 /usr/lib/libpython3*.so.1.0 | sort -V | tail -n 1)
VSKEY=$(ldconfig -p 2>/dev/null | sed -n 's/^[[:space:]]*libvsscript\.so[^=]*=>[[:space:]]*//p' | head -1)
VSKEY=${VSKEY:-/usr/lib/libvsscript.so}
mk_full_home() { # a home that already has the configuration the installer touches
  local h=$1
  mkdir -p "$h/.config/vapoursynth" "$h/.config/mpv" "$h/.config/vlc" "$h/.config/nvvsr" "$h/.local/bin" \
           "$h/.config/google-chrome/NativeMessagingHosts"
  printf '"/usr/lib/python3/site-packages/vapoursynth/libvsscript.so" = ["/usr/bin/python","%s","1791887590"]\n' "$LIBPY" >"$h/.config/vapoursynth/vapoursynth.toml"
  printf 'hwdec=auto\n' >"$h/.config/mpv/mpv.conf"
  printf 'F8 cycle pause\n' >"$h/.config/mpv/input.conf"
  printf '# fake vlcrc\n[core]\n' >"$h/.config/vlc/vlcrc"
  printf '/old/libs\n' >"$h/.config/nvvsr/libdir"
  printf '#!/bin/sh\necho my own mpv-sr\n' >"$h/.local/bin/mpv-sr"; chmod 755 "$h/.local/bin/mpv-sr"
  printf '{"name":"org.other.ext"}\n' >"$h/.config/google-chrome/NativeMessagingHosts/org.other.ext.json"
  find "$h" -type f -exec touch -d '2026-01-02 03:04:05' {} +
}

# ---- fake wheel: a zip with the real wheel's layout, served over file:// ------------------------------------------------
WHEELDIR=$SCR/wheel
WHEEL_FILE=nvidia_vfx-0.2.0.0-cp312-abi3-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl
rm -rf "$WHEELDIR"
mkdir -p "$WHEELDIR/tree/nvvfx/libs" "$WHEELDIR/tree/nvidia_vfx.libs" "$WHEELDIR/tree/nvidia_vfx-0.2.0.0.dist-info" "$WHEELDIR/wheels"
printf 'fake' >"$WHEELDIR/tree/nvvfx/libs/libVideoFX.so"
printf 'fake' >"$WHEELDIR/tree/nvvfx/libs/libnvVFXVideoSuperRes.so"
printf 'fake' >"$WHEELDIR/tree/nvidia_vfx.libs/libcudart-fake.so"
printf 'Metadata-Version: 2.1\nName: nvidia-vfx\nVersion: 0.2.0.0\n' >"$WHEELDIR/tree/nvidia_vfx-0.2.0.0.dist-info/METADATA"
printf 'Wheel-Version: 1.0\nTag: cp312-abi3-manylinux_2_27_x86_64\n' >"$WHEELDIR/tree/nvidia_vfx-0.2.0.0.dist-info/WHEEL"
bsdtar --format zip -cf "$WHEELDIR/wheels/$WHEEL_FILE" -C "$WHEELDIR/tree" nvvfx nvidia_vfx.libs nvidia_vfx-0.2.0.0.dist-info
WHEEL_SHA=$(sha256sum "$WHEELDIR/wheels/$WHEEL_FILE" | cut -d' ' -f1)
export NVVSR_WHEEL_URL=file://$WHEELDIR/wheels/$WHEEL_FILE
export NVVSR_WHEEL_SHA256=$WHEEL_SHA

echo "== 0. static checks"
t "bash -n install.sh" bash -n "$INSTALL"
t "bash -n build-mpv-vsr.sh" bash -n "$BUILD_MPV"
t "bash -n tests/run.sh" bash -n "${BASH_SOURCE[0]}"
t "bash -n manifest.sh" bash -n "$MANIFEST_SH"
if command -v shellcheck >/dev/null; then
  t "shellcheck install.sh build-mpv-vsr.sh run.sh" shellcheck -x "$INSTALL" "$BUILD_MPV" "${BASH_SOURCE[0]}"
else echo "NOTE shellcheck is not installed: only bash -n was run on the scripts"; fi

echo "== 1. usage and guards"
run "$SCR/u1.log"; eq "no command -> exit 2" "$RC" 2
run "$SCR/u2.log" frobnicate; eq "unknown command -> exit 2" "$RC" 2
run "$SCR/u3.log" install --bogus; eq "unknown option -> exit 2" "$RC" 2
run "$SCR/u4.log" --help; eq "--help -> exit 0" "$RC" 0
mkdir -p "$SCR/guard"
run "$SCR/u5.log" install --home "$SCR/guard" --prefix /usr/local/nvvsr --dry-run; eq "system prefix refused" "$RC" 2
run "$SCR/u6.log" install --home "$SCR/guard" --prefix "$SCR/guard" --dry-run; eq "prefix = home refused" "$RC" 2
run "$SCR/u7.log" install --home "$SCR/guard" --prefix "$SCR/guard/a:b" --dry-run; eq "colon in prefix refused" "$RC" 2
run "$SCR/u8.log" install --home "$SCR/does-not-exist" --dry-run; eq "missing --home refused" "$RC" 2

echo "== 2. check"
run "$SCR/c1.log" check --home "$SCR/guard"; eq "check exit status" "$RC" 0
has "$SCR/c1.log" "unzip" && has "$SCR/c1.log" "sha256sum" && has "$SCR/c1.log" "cmake" && has "$SCR/c1.log" "GPU-free" && ok "check lists tools, skips the driver query" || bad "check output"
# a PATH without vlc: reported missing with the exact pacman line, nothing is run
NOVLC=$SCR/novlc; mkdir -p "$NOVLC"
for p in /usr/bin/*; do [ "${p##*/}" = vlc ] || ln -s "$p" "$NOVLC/"; done
PATH=$SHIM:$NOVLC "$INSTALL" check --home "$SCR/guard" >"$SCR/c2.log" 2>&1; RC=$?
eq "check with vlc missing -> exit 1" "$RC" 1
has "$SCR/c2.log" "missing  vlc" && has "$SCR/c2.log" "sudo pacman -S --needed .*vlc" && ok "check prints the sudo pacman -S line" || bad "pacman hint"

echo "== 3. manifest.sh"
M=$SCR/m; mkdir -p "$M"
printf 'a\nline\nb\n' >"$M/f"; chmod 640 "$M/f"
eq "remove-line removes the exact line" "$("$MANIFEST_SH" remove-line "$M/f" line)" 1
eq "remove-line keeps the rest" "$(cat "$M/f")" "$(printf 'a\nb')"
eq "remove-line keeps the mode" "$(stat -c %a "$M/f")" 640
printf '{"schema": 2}\n' >"$M/bad.json"
f "validate rejects a bad install.json" "$MANIFEST_SH" validate "$M/bad.json"
printf 'meta\tversion\t0.1.0\nmeta\tprefix\t/p\nmeta\thome\t/h\nmeta\trepo\t/r\nmeta\tgit_dirty\t0\ncomp\tcore\tinstalled\t-\nentry\tfile\t/h/x\tcore\t1\t-\t%s\t-\n' \
  "$(printf 0%.0s $(seq 64))" | "$MANIFEST_SH" to-json "$M/ok.json"
t "to-json writes a valid file" "$MANIFEST_SH" validate "$M/ok.json"
stat -c %Y "$M/ok.json" >"$M/mtime1"; sleep 1.1
"$MANIFEST_SH" to-tsv "$M/ok.json" | "$MANIFEST_SH" to-json "$M/ok.json"
eq "unchanged state does not rewrite install.json" "$(stat -c %Y "$M/ok.json")" "$(cat "$M/mtime1")"

echo "== 4. build-mpv-vsr.sh (argument handling and dry run only; it is never run for real here)"
"$BUILD_MPV" --dry-run >"$SCR/b1.log" 2>&1; eq "build-mpv dry run exit 0" "$?" 0
has "$SCR/b1.log" "ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209" && has "$SCR/b1.log" "v0.41.0.tar.gz" &&
  has "$SCR/b1.log" "vapoursynth-script.pc" && has "$SCR/b1.log" "-Dvapoursynth=enabled" && ok "dry run prints URL, pinned sha256, shim and flags" || bad "build-mpv dry run content"
"$BUILD_MPV" --bogus >/dev/null 2>&1; eq "build-mpv unknown option -> 2" "$?" 2
"$BUILD_MPV" --prefix relative/dir --dry-run >/dev/null 2>&1; eq "build-mpv relative prefix -> 2" "$?" 2
"$BUILD_MPV" --prefix /usr/local --dry-run >/dev/null 2>&1; eq "build-mpv system prefix -> 2" "$?" 2
"$BUILD_MPV" --jobs 0 --dry-run >/dev/null 2>&1; eq "build-mpv --jobs 0 -> 2" "$?" 2
eq "build-mpv dry run wrote nothing in the scratch tmp" "$(ls "$TMPDIR" | grep -c nvvsr-mpv-build)" 0

echo "== 5. full install into a fake home that already has configuration"
H=$SCR/h1; mk_full_home "$H"
P=$H/.local/share/nvvsr
snap "$H" "$SCR/h1.before"
cp "$H/.config/mpv/mpv.conf" "$SCR/mpv.conf.orig"
run "$SCR/i0.log" install --home "$H" --mpv-bin "$MPV_REAL" --dry-run
eq "dry run exit 0" "$RC" 0
has "$SCR/i0.log" "would" && has "$SCR/i0.log" "vapoursynth.toml" && ok "dry run lists the plan" || bad "dry run plan"
snap "$H" "$SCR/h1.dry"
eq "dry run writes nothing" "$(diff "$SCR/h1.before" "$SCR/h1.dry" | wc -l)" 0
if command -v setsid >/dev/null; then
  setsid --wait "$INSTALL" install --home "$H" --mpv-bin "$MPV_REAL" </dev/null >"$SCR/i1.log" 2>&1; RC=$?
  eq "no terminal and no --yes: refused (exit 3)" "$RC" 3
  has "$SCR/i1.log" "re-run with --yes" && ok "the refusal says why" || bad "refusal text"
  snap "$H" "$SCR/h1.refused"
  eq "refusal wrote nothing" "$(diff "$SCR/h1.before" "$SCR/h1.refused" | wc -l)" 0
else echo "NOTE setsid missing: refusal test skipped"; fi

if gcc -O2 -Wall -Wextra -o "$SCR/pty-answer" "$HERE/pty-answer.c" -lutil; then ok "pty-answer compiles"; else bad "pty-answer compiles"; fi
PTY_ANSWER=$SCR/pty-answer
"$PTY_ANSWER" n "$INSTALL" install --home "$H" --mpv-bin "$MPV_REAL" >"$SCR/i2.log" 2>&1; RC=$?
eq "answering n on a terminal: exit 3" "$RC" 3
snap "$H" "$SCR/h1.declined"
eq "declined install wrote nothing" "$(diff "$SCR/h1.before" "$SCR/h1.declined" | wc -l)" 0

t "private mpv exists for the vf=help check" test -x "$MPV_REAL"
run "$SCR/i3.log" install --home "$H" --mpv-bin "$MPV_REAL" --yes
eq "install --yes exit 0" "$RC" 0
tail -n 12 "$SCR/i3.log" | sed 's/^/    | /'

for x in lib/libnvvsr.so.0.1.0 lib/libvsnvvsr.so lib/libnvvsr_plugin.so mpv/vsr.lua mpv/vsr.vpy mpv/script-opts/vsr.conf install.json \
         vfx/nvvfx/libs/libnvVFXVideoSuperRes.so; do t "installed $x" test -f "$P/$x"; done
t "no pip or venv in the prefix" test ! -e "$P/venv"
eq "install.json: wheel_version is the wheel's METADATA version" "$(jq -r .wheel_version "$P/install.json")" 0.2.0.0
t "libnvvsr.so.0 is a symlink" test -L "$P/lib/libnvvsr.so.0"
t "libnvvsr.so is a symlink" test -L "$P/lib/libnvvsr.so"
t "VLC module in the user plugin dir" test -f "$H/.local/share/vlc/plugins/video_output/libnvvsr_plugin.so"
t "mpv-sr is executable" test -x "$H/.local/bin/mpv-sr"
t "vlc-sr is executable" test -x "$H/.local/bin/vlc-sr"
t "mpv-sr parses" bash -n "$H/.local/bin/mpv-sr"
t "vlc-sr parses" bash -n "$H/.local/bin/vlc-sr"
f "no wrapper named mpv or vlc (system players are not shadowed)" test -e "$H/.local/bin/mpv" -o -e "$H/.local/bin/vlc"
has "$H/.local/bin/mpv-sr" "^exec \"\$MPV\" --script=$P/mpv/vsr.lua \"\$@\"" && ok "mpv-sr execs the private mpv with vsr.lua" || bad "mpv-sr content"
has "$H/.local/bin/vlc-sr" "VLC_PLUGIN_PATH=$H/.local/share/vlc/plugins" && ok "vlc-sr sets VLC_PLUGIN_PATH" || bad "vlc-sr content"
eq "user mpv.conf untouched" "$(cat "$H/.config/mpv/mpv.conf")" "hwdec=auto"
eq "user input.conf untouched" "$(cat "$H/.config/mpv/input.conf")" "F8 cycle pause"
eq "user vlcrc untouched" "$(cat "$H/.config/vlc/vlcrc")" "$(printf '# fake vlcrc\n[core]')"
eq "installer never writes under google-chrome (only the fixture is there)" "$(cd "$H/.config/google-chrome" && find . -type f | LC_ALL=C sort | tr '\n' ' ')" "./NativeMessagingHosts/org.other.ext.json "
eq "vsr.conf template installed for the user (absent before)" "$(cmp "$H/.config/mpv/script-opts/vsr.conf" "$REPO/mpv/script-opts/vsr.conf" && echo same)" same
eq "libdir file = the two wheel dirs in the prefix vfx" "$(cat "$H/.config/nvvsr/libdir")" "$P/vfx/nvvfx/libs:$P/vfx/nvidia_vfx.libs"
BAK=$(ls "$H/.config/nvvsr/" | grep '^libdir.bak-nvvsr-' | head -1)
eq "old libdir backed up with its content" "$(cat "$H/.config/nvvsr/$BAK" 2>/dev/null)" /old/libs
BAK=$(ls "$H/.local/bin/" | grep '^mpv-sr.bak-nvvsr-' | head -1)
eq "pre-existing mpv-sr backed up" "$(tail -n1 "$H/.local/bin/$BAK" 2>/dev/null)" "echo my own mpv-sr"
BAK=$(ls "$H/.config/vapoursynth/" | grep '^vapoursynth.toml.bak-nvvsr-' | head -1)
eq "vapoursynth.toml backup = the original line" "$(cat "$H/.config/vapoursynth/$BAK" 2>/dev/null | wc -l)" 1
eq "vapoursynth.toml: original line + one appended" "$(wc -l <"$H/.config/vapoursynth/vapoursynth.toml")" 2
# copy rule: exe, libpython and mtime are the existing entry's values (mk_full_home writes mtime 1791887590)
EXPECT_LINE="\"$VSKEY\" = [\"/usr/bin/python\",\"$LIBPY\",\"1791887590\"]"
eq "appended line copies exe, libpython and mtime of the existing entry" "$(tail -n1 "$H/.config/vapoursynth/vapoursynth.toml")" "$EXPECT_LINE"
eq "other toml line unchanged" "$(head -n1 "$H/.config/vapoursynth/vapoursynth.toml")" "$(head -n1 "$H/.config/vapoursynth/$BAK")"

echo "== 6. install.json"
J=$P/install.json
BADS=""                                  # names of failed assertions, reported as one check like before
jchk() { jq -e --arg h "$H" --arg p "$P" --arg r "$REPO" "$1" "$J" >/dev/null 2>&1 || BADS="$BADS; $2"; }
libdirs_ok() { local d IFS=:; [ -n "$1" ] || return 1; for d in $1; do [ -d "$d" ] || return 1; done; }
jchk '.schema == 1' schema
jchk '.prefix == $p and .home == $h and .repo == $r' "prefix/home/repo"
libdirs_ok "$(jq -r '.nvvsr_libdir // ""' "$J")" || BADS="$BADS; nvvsr_libdir"
for c in core vapoursynth mpv vlc wheel; do jchk ".components.$c.status == \"installed\"" "component $c"; done
for k in lib libnvvsr vapoursynth_plugin vlc_plugin mpv_script mpv_wrapper vlc_wrapper vsr_conf libdir_file vfx install_json; do
  jchk ".paths.$k" "paths.$k"; done
jchk '.wheel.installed and (.wheel.sha256 | type == "string" and length == 64)' wheel
jchk '.private_mpv.vapoursynth == true' private_mpv
jchk '[.entries[].type] | unique | contains(["dir","file","symlink","line","tree"])' "entry types"
jchk '.entries | all(.path | startswith($h + "/"))' "entry inside home"
jchk '[.entries[] | select(.type == "line")] | length == 1 and ((.[0].backup // "") != "") and .[0].created == false' "toml line entry"
while IFS=$'\t' read -r typ path backup; do   # the filesystem part: jq cannot test it
  case $typ in file|symlink|dir|tree|line) [ -e "$path" ] || [ -L "$path" ] || BADS="$BADS; entry exists $path" ;; esac
  if [ -n "$backup" ]; then [ -e "$backup" ] || [ -L "$backup" ] || BADS="$BADS; backup exists $backup"; fi
done < <(jq -r '.entries[] | [.type, .path, (.backup // "")] | @tsv' "$J")
if [ -z "$BADS" ]; then ok "install.json: schema, components, paths, entries exist, backups exist"
else bad "install.json content" "${BADS#; }"; fi
t "install.json validates with manifest.sh" "$MANIFEST_SH" validate "$P/install.json"

echo "== 7. libraries find libnvvsr.so.0 in the prefix"
for lib in "$P/lib/libvsnvvsr.so" "$P/lib/libnvvsr_plugin.so" "$H/.local/share/vlc/plugins/video_output/libnvvsr_plugin.so"; do
  rp=$(readelf -d "$lib" | sed -n 's/.*(RUNPATH)[^[]*\[\(.*\)\].*/\1/p')
  case ":$rp:" in *':$ORIGIN:'*) ok "$(basename "$lib"): RUNPATH has \$ORIGIN" ;; *) bad "RUNPATH of $lib" "$rp" ;; esac
  case ":$rp:" in *":$P/lib:"*) ok "$(basename "$lib"): RUNPATH has $P/lib" ;; *) bad "RUNPATH of $lib" "$rp" ;; esac
  eq "$(basename "$lib"): ldd resolves libnvvsr.so.0 in the prefix" "$(ldd "$lib" | sed -n 's/^[[:space:]]*libnvvsr\.so\.0 => \([^ ]*\).*/\1/p')" "$P/lib/libnvvsr.so.0"
done
eq "libnvvsr.so.0 SONAME" "$(readelf -d "$P/lib/libnvvsr.so.0" | sed -n 's/.*(SONAME)[^[]*\[\(.*\)\].*/\1/p')" libnvvsr.so.0

echo "== 8. mpv-sr and vsr.vpy in the installed layout"
mpv_env=(env -u DISPLAY -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS HOME="$SCR/mpvhome" XDG_CONFIG_HOME="$SCR/mpvhome" XDG_RUNTIME_DIR="$SCR/mpvhome")
mkdir -p "$SCR/mpvhome"
"${mpv_env[@]}" "$H/.local/bin/mpv-sr" --no-config --vf=help 2>&1 | grep -Eq '^[[:space:]]+vapoursynth[[:space:]]' && ok "mpv-sr starts the private mpv (vf=help lists vapoursynth)" || bad "mpv-sr vf=help"

echo "== 9. status, idempotence, repair"
run "$SCR/s1.log" status --home "$H"; eq "status of a healthy install -> 0" "$RC" 0
snap "$H" "$SCR/h1.installed"
run "$SCR/i4.log" install --home "$H" --mpv-bin "$MPV_REAL" --yes
eq "second install exit 0" "$RC" 0
has "$SCR/i4.log" "nothing to change" && ok "second install reports nothing to change" || bad "second install output"
snap "$H" "$SCR/h1.again"
eq "second install changes nothing (files, modes, sizes, mtimes, content, build tree)" "$(diff "$SCR/h1.installed" "$SCR/h1.again" | wc -l)" 0
run "$SCR/i5.log" install --home "$H" --mpv-bin "$MPV_REAL" --dry-run
has "$SCR/i5.log" "nothing to change" && ok "dry run of an installed system: nothing to change" || bad "dry run on installed"
rm -f "$P/lib/libvsnvvsr.so"
run "$SCR/s2.log" status --home "$H"; eq "status notices a missing file -> 2" "$RC" 2
run "$SCR/i6.log" install --home "$H" --mpv-bin "$MPV_REAL" --yes
t "re-install repairs the missing file" test -f "$P/lib/libvsnvvsr.so"
run "$SCR/s3.log" status --home "$H"; eq "status after repair -> 0" "$RC" 0
snap "$H" "$SCR/h1.repaired"

echo "== 10. VLC finds the module (headless, scratch HOME, no window, nothing played)"
if command -v vlc >/dev/null; then
  mkdir -p "$SCR/vlchome/.config" "$SCR/vlchome/.cache" "$SCR/vlchome/.local/share" "$SCR/vlcrt"; chmod 700 "$SCR/vlcrt"
  vlc_env=(env -u DISPLAY -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u XDG_SESSION_TYPE -u VLC_PLUGIN_PATH
           HOME="$SCR/vlchome" XDG_CONFIG_HOME="$SCR/vlchome/.config" XDG_CACHE_HOME="$SCR/vlchome/.cache"
           XDG_DATA_HOME="$SCR/vlchome/.local/share" XDG_RUNTIME_DIR="$SCR/vlcrt")
  "${vlc_env[@]}" timeout 90 "$H/.local/bin/vlc-sr" -I dummy --list >"$SCR/vlc1.log" 2>&1
  eq "vlc-sr --list lists the nvvsr modules (display + filter)" "$(grep -c '^[[:space:]]*nvvsr[[:space:]]' "$SCR/vlc1.log")" 2
  "${vlc_env[@]}" timeout 90 vlc -I dummy --list >"$SCR/vlc2.log" 2>&1
  eq "plain vlc does not load it (VLC 3.0 has no per-user plugin directory; hence vlc-sr)" "$(grep -c '^[[:space:]]*nvvsr[[:space:]]' "$SCR/vlc2.log")" 0
else echo "NOTE vlc not installed: VLC check skipped"; fi

echo "== 11. uninstall restores everything"
run "$SCR/d1.log" uninstall --home "$H" --dry-run; eq "uninstall --dry-run exit 0" "$RC" 0
has "$SCR/d1.log" "would" && ok "uninstall dry run lists the plan" || bad "uninstall plan"
snap "$H" "$SCR/h1.udry"
rm -f "$H/.local/share/vlc/plugins/plugins.dat"   # a VLC cache VLC may have written; uninstall handles it too
snap "$H" "$SCR/h1.udry2"
run "$SCR/d2.log" uninstall --home "$H" --yes; eq "uninstall exit 0" "$RC" 0
f "uninstall removed <prefix>/vfx" test -e "$P/vfx"
snap "$H" "$SCR/h1.after"
if diff "$SCR/h1.before" "$SCR/h1.after" >"$SCR/h1.diff"; then ok "uninstall: home is byte-identical to before the install (dirs, modes, mtimes, content)"
else bad "uninstall restore" "$(head -5 "$SCR/h1.diff" | tr '\n' ' ')"; fi
run "$SCR/d3.log" uninstall --home "$H" --yes; eq "uninstall again: nothing to do, exit 0" "$RC" 0
run "$SCR/s4.log" status --home "$H"; eq "status after uninstall -> 1" "$RC" 1

echo "== 12. bare home: --no-vlc --skip-wheel, partial mpv, user changes survive uninstall"
H2=$SCR/h2; mkdir -p "$H2"; P2=$H2/.local/share/nvvsr
snap "$H2" "$SCR/h2.before"
NVVSR_INSTALL_SKIP_MPV_BUILD=1 run "$SCR/j1.log" install --home "$H2" --no-vlc --skip-wheel --build-mpv --mpv-bin "$H2/.local/opt/mpv-vsr/bin/mpv" --yes
eq "bare install exit 0" "$RC" 0
has "$SCR/j1.log" "build skipped" && ok "--build-mpv is a no-op with NVVSR_INSTALL_SKIP_MPV_BUILD=1" || bad "skip mpv build"
f "no private mpv was built" test -e "$H2/.local/opt"
f "--no-vlc: no VLC module, no vlc-sr" test -e "$H2/.local/share/vlc" -o -e "$H2/.local/bin/vlc-sr"
f "--skip-wheel: no vfx, no libdir file" test -e "$P2/vfx" -o -e "$H2/.config/nvvsr/libdir"
# no entry to copy: python3 (readlink -f), the highest /usr/lib/libpython3*.so.1.0 and its mtime
HIGHLIB=$(ls -1 /usr/lib/libpython3*.so.1.0 | sort -V | tail -n 1)
eq "toml created with exactly the libvsscript line (system fallback)" "$(cat "$H2/.config/vapoursynth/vapoursynth.toml")" "\"$VSKEY\" = [\"$(readlink -f "$(command -v python3)")\",\"$HIGHLIB\",\"$(stat -L -c %Y "$HIGHLIB")\"]"
f "GPU probe skipped with NVVSR_INSTALL_SKIP_GPU_CHECK=1" grep -q "GPU probe:" "$SCR/j1.log"
t "nvvsr_probe installed as an executable" test -x "$P2/bin/nvvsr_probe"
t "nvvsr_probe recorded in paths and as a file entry" test "$(grep -c -- "$P2/bin/nvvsr_probe" "$P2/install.json")" -ge 2
jq -e '.components.mpv.status == "partial" and .components.vlc.status == "skipped" and .components.wheel.status == "skipped" and .nvvsr_libdir == null' "$P2/install.json" >/dev/null && ok "components: mpv partial (no private mpv), vlc/wheel skipped, libdir null" || bad "partial components"
# a file of ours that the user edited is kept as a backup when the installer rewrites it, and restored by nothing else
echo "# my tweak" >>"$H2/.local/bin/mpv-sr"
NVVSR_INSTALL_SKIP_MPV_BUILD=1 run "$SCR/j1b.log" install --home "$H2" --no-vlc --skip-wheel --mpv-bin "$H2/.local/opt/mpv-vsr/bin/mpv" --yes
eq "re-install after the user edited mpv-sr: exit 0" "$RC" 0
SIDE=$(ls "$H2/.local/bin/" | grep '^mpv-sr.bak-nvvsr-' | head -1)
eq "the user's edited mpv-sr is kept as a backup" "$(tail -n1 "$H2/.local/bin/$SIDE" 2>/dev/null)" "# my tweak"
f "...and mpv-sr is ours again" grep -q "my tweak" "$H2/.local/bin/mpv-sr"
eq "second install left the toml alone (the entry exists): one line" "$(wc -l <"$H2/.config/vapoursynth/vapoursynth.toml")" 1
eq "...and made no toml backup" "$(ls "$H2/.config/vapoursynth/" | grep -c bak-nvvsr)" 0
rm -f "$H2/.local/bin/$SIDE"
"$H2/.local/bin/mpv-sr" --version >"$SCR/j2.log" 2>&1; eq "mpv-sr without the private mpv: exit 127" "$?" 127
has "$SCR/j2.log" "private mpv not found" && ok "mpv-sr explains what is missing" || bad "mpv-sr message"
echo "# edited by the panel" >>"$H2/.config/mpv/script-opts/vsr.conf"
echo '"/user/own.so" = ["a","b","1"]' >>"$H2/.config/vapoursynth/vapoursynth.toml"
run "$SCR/j3.log" uninstall --home "$H2" --yes; eq "uninstall exit 0 (bare home)" "$RC" 0
has "$SCR/j3.log" "keep .*vsr.conf" && ok "uninstall keeps the vsr.conf you changed" || bad "vsr.conf kept"
eq "toml: only our line removed, the user's line stays" "$(cat "$H2/.config/vapoursynth/vapoursynth.toml")" '"/user/own.so" = ["a","b","1"]'
eq "everything else is gone (only the two kept files remain)" "$(cd "$H2" && find . -type f | LC_ALL=C sort | tr '\n' ' ')" "./.config/mpv/script-opts/vsr.conf ./.config/vapoursynth/vapoursynth.toml "
f "no empty directories of ours are left" test -e "$H2/.local"
f "prefix removed" test -e "$P2"
f "uninstall removed <prefix>/bin/nvvsr_probe" test -e "$P2/bin/nvvsr_probe"

echo "== 12b. vapoursynth.toml fallback with a fake python3 (PATH) and fake libpython dir (NVVSR_INSTALL_PYLIBDIR)"
FPY=$SCR/fakepy/bin; FLIB=$SCR/fakelib; mkdir -p "$FPY" "$FLIB"
printf '#!/bin/sh\nexit 0\n' >"$FPY/python3"; chmod 755 "$FPY/python3"
for v in 3.9 3.10 3.11; do : >"$FLIB/libpython$v.so.1.0"; done
touch -d '2026-03-04 05:06:07' "$FLIB/libpython3.11.so.1.0"
H4=$SCR/h4; mkdir -p "$H4"
PATH=$FPY:$PATH NVVSR_INSTALL_PYLIBDIR=$FLIB NVVSR_INSTALL_SKIP_MPV_BUILD=1 run "$SCR/m1.log" install --home "$H4" --no-vlc --skip-wheel --build-mpv --mpv-bin "$H4/.local/opt/mpv-vsr/bin/mpv" --yes
eq "fake python layout: install exit 0" "$RC" 0
eq "fallback line: python3 from PATH (readlink -f), highest libpython (3.11 by sort -V), its mtime" "$(cat "$H4/.config/vapoursynth/vapoursynth.toml")" "\"$VSKEY\" = [\"$(readlink -f "$FPY/python3")\",\"$FLIB/libpython3.11.so.1.0\",\"$(stat -L -c %Y "$FLIB/libpython3.11.so.1.0")\"]"
run "$SCR/m2.log" uninstall --home "$H4" --yes; eq "fake python layout: uninstall exit 0" "$RC" 0
f "fake python layout: uninstall removed the toml we created" test -e "$H4/.config/vapoursynth/vapoursynth.toml"

echo "== 13. a re-run with different options, --with-panel, uninstall"
H3=$SCR/h3; mkdir -p "$H3/.config/vapoursynth"
printf '"/some/other.so" = ["a","b","1"]' >"$H3/.config/vapoursynth/vapoursynth.toml"   # no trailing newline
snap "$H3" "$SCR/h3.before"
run "$SCR/k0.log" install --home "$H3" --no-mpv --no-vlc --skip-wheel --with-panel --dry-run
has "$SCR/k0.log" "panel" && ok "dry run mentions the panel" || bad "panel plan"
"$PTY_ANSWER" y "$INSTALL" install --home "$H3" --no-vlc --skip-wheel --mpv-bin "$MPV_REAL" >"$SCR/k1.log" 2>&1; RC=$?
eq "interactive yes: exit 0" "$RC" 0
t "...core is installed" test -f "$H3/.local/share/nvvsr/lib/libnvvsr.so.0"
eq "no-newline toml: our line appended on its own line" "$(sed -n 2p "$H3/.config/vapoursynth/vapoursynth.toml" | cut -d= -f1)" "\"$VSKEY\" "
run "$SCR/k2.log" install --home "$H3" --no-vlc --skip-wheel --mpv-bin "$MPV_REAL" --yes; eq "re-run with --yes" "$RC" 0
jq -e '.components.core.status == "installed"' "$H3/.local/share/nvvsr/install.json" >/dev/null && ok "install.json lists core after the re-run" || bad "merged manifest"
run "$SCR/k3.log" uninstall --home "$H3" --yes; eq "uninstall exit 0 (re-run home)" "$RC" 0
snap "$H3" "$SCR/h3.after"
eq "re-run home byte-identical after uninstall" "$(diff "$SCR/h3.before" "$SCR/h3.after" | wc -l)" 0

echo "== 13b. wheel: a hash mismatch and a corrupt download are refused, nothing is left behind"
H4=$SCR/h4; mkdir -p "$H4"
NVVSR_WHEEL_SHA256=$(printf '0%.0s' $(seq 64)) "$INSTALL" install --home "$H4" --no-mpv --no-vlc --yes >"$SCR/w1.log" 2>&1; RC=$?
eq "hash mismatch: install exit 1" "$RC" 1
has "$SCR/w1.log" "sha256 is .*pinned" && ok "hash mismatch: the message names both hashes" || bad "hash mismatch message"
f "hash mismatch: no vfx tree" test -e "$H4/.local/share/nvvsr/vfx"
eq "hash mismatch: no .whl left under the home" "$(find "$H4" -name '*.whl' | wc -l)" 0
eq "hash mismatch: no installer temp directory left" "$(ls "$TMPDIR" | grep -c '^nvvsr-install\.')" 0
eq "hash mismatch: install.json records the wheel as failed" "$(jq -r .components.wheel.status "$H4/.local/share/nvvsr/install.json")" failed
H5=$SCR/h5; mkdir -p "$H5"; printf 'this is not a zip file\n' >"$SCR/corrupt.whl"
NVVSR_WHEEL_URL=file://$SCR/corrupt.whl NVVSR_WHEEL_SHA256=$(sha256sum "$SCR/corrupt.whl" | cut -d' ' -f1) \
  "$INSTALL" install --home "$H5" --no-mpv --no-vlc --yes >"$SCR/w2.log" 2>&1; RC=$?
eq "corrupt wheel (hash ok): install exit 1" "$RC" 1
has "$SCR/w2.log" "not a valid wheel" && ok "corrupt wheel: the message says why" || bad "corrupt wheel message"
f "corrupt wheel: no vfx tree left" test -e "$H5/.local/share/nvvsr/vfx"
eq "corrupt wheel: install.json records the wheel as failed" "$(jq -r .components.wheel.status "$H5/.local/share/nvvsr/install.json")" failed

echo "== 13c. --with-panel: fake nvcontrol (NVVSR_PANEL_BIN, no panel build), launcher with --home, uninstall; a failed build"
PB=$SCR/panelbin; mkdir -p "$PB"
printf '#!/bin/sh\necho fake nvcontrol\n' >"$PB/nvcontrol"; chmod 755 "$PB/nvcontrol"
printf '#!/bin/sh\n' >"$PB/notexec"; chmod 644 "$PB/notexec"
H6=$SCR/h6; P6=$H6/.local/share/nvvsr; DESK=$H6/.local/share/applications/org.nvvsr.Control.desktop
mkdir -p "$H6"
run "$SCR/n0.log" install --home "$H6" --no-mpv --no-vlc --skip-wheel --with-panel --dry-run
has "$SCR/n0.log" "would build .*panel" && ok "panel dry run: build listed" || bad "panel dry run: build"
has "$SCR/n0.log" "would .*bin/nvcontrol" && ok "panel dry run: install listed" || bad "panel dry run: install"
has "$SCR/n0.log" "would launch .*install-launcher.sh --bin .*bin/nvcontrol --home $H6" && ok "panel dry run: launcher listed with --bin and --home" || bad "panel dry run: launcher"
f "panel dry run: no desktop entry written" test -e "$DESK"
NVVSR_PANEL_BIN=$PB/nvcontrol run "$SCR/n1.log" install --home "$H6" --no-mpv --no-vlc --skip-wheel --with-panel --yes
eq "panel install: exit 0" "$RC" 0
t "panel install: desktop entry written under --home" test -f "$DESK"
has "$DESK" "^Exec=$P6/bin/nvcontrol$" && ok "panel install: launcher Exec is the installed binary" || bad "panel install: Exec line"
t "panel install: $P6/bin/nvcontrol is executable" test -x "$P6/bin/nvcontrol"
t "panel install: the installed binary is the one named by NVVSR_PANEL_BIN" cmp -s "$PB/nvcontrol" "$P6/bin/nvcontrol"
eq "panel install: install.json lists panel as installed" "$(jq -r .components.panel.status "$P6/install.json")" installed
run "$SCR/n2.log" uninstall --home "$H6" --yes; eq "panel uninstall: exit 0" "$RC" 0
f "panel uninstall: desktop entry removed" test -e "$DESK"
f "panel uninstall: binary removed" test -e "$P6/bin/nvcontrol"
BADC=$SCR/badcmake; mkdir -p "$BADC"; REAL_CMAKE=$(command -v cmake)
printf '#!/bin/sh\ncase "$*" in *panel*) echo "fake cmake: no panel build in tests" >&2; exit 1 ;; esac\nexec %s "$@"\n' "$REAL_CMAKE" >"$BADC/cmake"
chmod 755 "$BADC/cmake"
H7=$SCR/h7; mkdir -p "$H7"
PATH=$BADC:$PATH NVVSR_PANEL_BIN=$PB/notexec run "$SCR/n3.log" install --home "$H7" --no-mpv --no-vlc --skip-wheel --with-panel --yes
eq "panel failed build: install exit 1" "$RC" 1
has "$SCR/n3.log" "build failed" && ok "panel failed build: reported" || bad "panel failed build: message"
eq "panel failed build: install.json lists panel as failed" "$(jq -r .components.panel.status "$H7/.local/share/nvvsr/install.json")" failed
t "panel failed build: core still installed" test -f "$H7/.local/share/nvvsr/lib/libnvvsr.so.0"
f "panel failed build: no launcher run" test -e "$H7/.local/share/applications/org.nvvsr.Control.desktop"

echo "== 13d. --with-chrome: fake nvvsr_webd (NVVSR_WEBD_BIN, no build), systemctl shim (logs argv; exits 1 while systemctl.fail exists)"
WB=$SCR/webdbin; mkdir -p "$WB"
printf '#!/bin/sh\necho fake nvvsr_webd\n' >"$WB/nvvsr_webd"; chmod 755 "$WB/nvvsr_webd"
SYSD=$SCR/sysdshim; mkdir -p "$SYSD"; SYSLOG=$SCR/systemctl.log
printf '#!/bin/sh\necho "$*" >>"%s"\n[ -e "%s" ] && exit 1\nexit 0\n' "$SYSLOG" "$SCR/systemctl.fail" >"$SYSD/systemctl"
chmod 755 "$SYSD/systemctl"
H8=$SCR/h8; P8=$H8/.local/share/nvvsr; UNIT8=$H8/.config/systemd/user/nvvsr-webd.service; mkdir -p "$H8"
snap "$H8" "$SCR/h8.before"
PATH=$SYSD:$PATH run "$SCR/d0.log" install --home "$H8" --no-mpv --no-vlc --skip-wheel --with-chrome --dry-run
eq "webd dry run: exit 0" "$RC" 0
dl() { grep -n -- "$1" "$SCR/d0.log" | head -n 1 | cut -d: -f1; }
b=$(dl "would build .*webd"); i=$(dl "would install .*bin/nvvsr_webd"); c=$(dl "would create .*systemd/user/nvvsr-webd.service")
r=$(dl "would systemctl --user daemon-reload"); e=$(dl "would systemctl --user enable --now nvvsr-webd.service")
if [ -n "$b$i$c$r$e" ] && [ "$b" -lt "$i" ] && [ "$i" -lt "$c" ] && [ "$c" -lt "$r" ] && [ "$r" -lt "$e" ]; then ok "webd dry run: build, binary, unit, daemon-reload, enable in that order"
else bad "webd dry run: plan order"; fi
snap "$H8" "$SCR/h8.dry"
eq "webd dry run: writes nothing" "$(diff "$SCR/h8.before" "$SCR/h8.dry" | wc -l)" 0
rm -f "$SYSLOG"
NVVSR_WEBD_BIN=$WB/nvvsr_webd PATH=$SYSD:$PATH run "$SCR/d1.log" install --home "$H8" --no-mpv --no-vlc --skip-wheel --with-chrome --yes
eq "webd install: exit 0" "$RC" 0
t "webd install: binary equals the fake daemon" cmp -s "$WB/nvvsr_webd" "$P8/bin/nvvsr_webd"
has "$UNIT8" "^ExecStart=$P8/bin/nvvsr_webd 41777 --origin https://www.youtube.com$" && ok "webd install: unit ExecStart uses the prefix" || bad "webd install: ExecStart"
has "$SYSLOG" "^--user daemon-reload$" && ok "webd install: systemctl --user daemon-reload called" || bad "webd install: daemon-reload"
has "$SYSLOG" "^--user enable --now nvvsr-webd.service$" && ok "webd install: systemctl --user enable --now called" || bad "webd install: enable"
jq -e '.components.webd.status == "installed" and (.components.webd.detail | contains("service_enabled=true"))' "$P8/install.json" >/dev/null \
  && ok "webd install: install.json has webd installed, service_enabled=true" || bad "webd install: install.json"
echo "== 13d (cont.). the same home with a failing systemctl: a warning, the component stays installed, exit 0"
touch "$SCR/systemctl.fail"
NVVSR_WEBD_BIN=$WB/nvvsr_webd PATH=$SYSD:$PATH run "$SCR/d2.log" install --home "$H8" --no-mpv --no-vlc --skip-wheel --with-chrome --yes
rm -f "$SCR/systemctl.fail"
eq "webd failing systemctl: exit 0" "$RC" 0
has "$SCR/d2.log" "webd: the service was not enabled" && has "$SCR/d2.log" "systemctl --user enable --now nvvsr-webd.service" \
  && ok "webd failing systemctl: warning with the two commands" || bad "webd failing systemctl: warning"
jq -e '.components.webd.status == "installed" and (.components.webd.detail | contains("service_enabled=false"))' "$P8/install.json" >/dev/null \
  && ok "webd failing systemctl: still installed, service_enabled=false" || bad "webd failing systemctl: install.json"
rm -f "$SYSLOG"
PATH=$SYSD:$PATH run "$SCR/d3.log" uninstall --home "$H8" --yes; eq "webd uninstall: exit 0" "$RC" 0
has "$SYSLOG" "^--user disable --now nvvsr-webd.service$" && ok "webd uninstall: systemctl --user disable --now called" || bad "webd uninstall: disable"
f "webd uninstall: unit and binary removed" test -e "$UNIT8" -o -e "$P8/bin/nvvsr_webd"

echo "== 14. the real home and the package manager"
snap_real >"$SCR/real-after.txt"
if diff "$SCR/real-before.txt" "$SCR/real-after.txt" >"$SCR/real.diff"; then ok "real home unchanged (~/.config/{mpv,vlc,vapoursynth,nvvsr,systemd}, ~/.local/share/nvvsr, ~/.local/bin)"
else bad "real home changed" "$(head -3 "$SCR/real.diff" | tr '\n' ' ')"; fi
if [ -e "$SCR/shim-called" ]; then bad "sudo/pacman/paru/yay was called" "$(cat "$SCR/shim-called")"; else ok "sudo/pacman/paru/yay never called"; fi

echo
echo "SUMMARY: $PASS passed, $FAIL failed"
exit "$FAIL"
