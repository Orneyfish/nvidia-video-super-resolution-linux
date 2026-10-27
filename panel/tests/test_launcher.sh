#!/bin/bash
# Launcher installer test (bash port of class Launcher in tests/test_misc.py): install, idempotence, repair, removal,
# bad arguments, the nvcontrol lookup and quoting. A copy of the installer runs in a scratch tree with a fake nvcontrol.
#   usage: test_launcher.sh      (no GTK, no GPU, no window; writes only under a mktemp dir)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/nvc-launcher.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT
unset NVVSR_PREFIX

# a copy of the panel's installer and icon: the repo fallback (<repo>/build/panel/nvcontrol) points into the scratch tree
repo=$work/repo
mkdir -p "$repo/panel" && cp "$HERE/../install-launcher.sh" "$repo/panel/" && cp -r "$HERE/../icons" "$repo/panel/" || exit 2
LAUNCHER=$repo/panel/install-launcher.sh
mkdir -p "$work/fake" && printf '#!/bin/sh\nexit 0\n' > "$work/fake/nvcontrol" && chmod +x "$work/fake/nvcontrol"
fake=$work/fake/nvcontrol

fails=0 passes=0
ok() { passes=$((passes + 1)); echo "PASS: $1"; }
bad() { fails=$((fails + 1)); echo "FAIL: $1"; }
check() { if "${@:2}" >/dev/null 2>&1; then ok "$1"; else bad "$1"; fi; }

# the hostile environment of the Python test: the real data dir must never be used when --home is given
sh_env() { env HOME=/nonexistent-home XDG_DATA_HOME=/nonexistent-xdg sh "$LAUNCHER" "$@" 2>"$work/err"; }
fresh() {
  home=$(mktemp -d "$work/home.XXXXXX")
  desktop=$home/.local/share/applications/org.nvvsr.Control.desktop
  icon=$home/.local/share/icons/hicolor/scalable/apps/org.nvvsr.Control.svg
}
launch() { sh_env --home "$home" "$@"; }
run() { out=$(launch "$@"); rc=$?; }
nothing_written() { [ -z "$(find "$home" -mindepth 1 2>/dev/null)" ]; }
only_under_local() { [ -z "$(find "$home" -type f -o -type l 2>/dev/null | grep -vF "$home/.local/share/" || true)" ]; }

fresh
run --bin "$fake"
check "install with --bin exits 0 (rc=$rc)" test "$rc" -eq 0
check "the Exec line is the binary path" grep -Fxq -- "Exec=$fake" "$desktop"
check "the launcher has no python" test -z "$(grep -i python "$desktop")"
for line in "Icon=org.nvvsr.Control" "StartupWMClass=org.nvvsr.Control" "Terminal=false" \
  "Categories=System;Settings;HardwareSettings;" "Name=NV Control"; do
  check "the launcher has the line $line" grep -Fxq -- "$line" "$desktop"
done
check "the icon is installed" test -f "$icon"
check "install is correct: only the .local/share files were written" only_under_local
mtime=$(stat -c %y "$desktop" 2>/dev/null)
run --bin "$fake"
check "a second install is idempotent (already up to date)" grep -Fq "already up to date" <<<"$out"
check "a second install does not rewrite the launcher" test "$(stat -c %y "$desktop" 2>/dev/null)" = "$mtime"
run --uninstall --bin "$fake"
check "uninstall exits 0 (rc=$rc)" test "$rc" -eq 0
check "uninstall removes the launcher and the icon" test ! -e "$desktop" -a ! -e "$icon"
run --uninstall
check "uninstall twice is fine (rc=$rc)" test "$rc" -eq 0

fresh
run --bin "$fake"
printf '[Desktop Entry]\nType=Application\nName=old\nExec=true\n' > "$desktop"
run --bin "$fake"
check "a stale launcher is repaired" grep -Fq "installed" <<<"$out"
check "the repaired launcher has the new name" grep -Fxq "Name=NV Control" "$desktop"

fresh
sh_env --home "$home" --bogus; rc=$?
check "an unknown option exits 2 (rc=$rc)" test "$rc" -eq 2
sh_env --home; rc=$?
check "--home without a directory exits 2 (rc=$rc)" test "$rc" -eq 2
launch --bin; rc=$?
check "--bin without a path exits 2 (rc=$rc)" test "$rc" -eq 2

fresh
run
check "no executable found exits 1 (rc=$rc)" test "$rc" -eq 1
check "the message says how to build nvcontrol" grep -Fq "nvcontrol not found: build it with" "$work/err"
check "no executable found writes nothing" nothing_written

fresh
run --bin "$work/fake/missing"
check "a missing --bin exits 1 and writes nothing" test "$rc" -eq 1 -a -z "$(find "$home" -mindepth 1 2>/dev/null)"

fresh
dollar_dir="$work/we\$ird"; mkdir -p "$dollar_dir" && cp "$fake" "$dollar_dir/"
run --bin "$dollar_dir/nvcontrol"
check "a --bin path with a \$ is refused (rc=$rc)" test "$rc" -eq 1
check "the refused path writes nothing" nothing_written

fresh
spaced_dir="$work/my apps"; mkdir -p "$spaced_dir" && cp "$fake" "$spaced_dir/"
run --bin "$spaced_dir/nvcontrol"
check "a path with a space is double-quoted in Exec" grep -Fxq -- "Exec=\"$spaced_dir/nvcontrol\"" "$desktop"

fresh
prefix=$work/prefix; mkdir -p "$prefix/bin" && cp "$fake" "$prefix/bin/nvcontrol"
out=$(NVVSR_PREFIX=$prefix launch); rc=$?
check "NVVSR_PREFIX default is used when --bin is absent (rc=$rc)" test "$rc" -eq 0
check "the NVVSR_PREFIX Exec line is the prefix binary" grep -Fxq -- "Exec=$prefix/bin/nvcontrol" "$desktop"

fresh
mkdir -p "$repo/build/panel" && cp "$fake" "$repo/build/panel/nvcontrol"
run
check "the repo build is the default when NVVSR_PREFIX is unset (rc=$rc)" test "$rc" -eq 0
check "the repo default Exec line is the build binary" grep -Fxq -- "Exec=$repo/build/panel/nvcontrol" "$desktop"
rm -rf "$repo/build"

fresh
printf 'not executable\n' > "$work/notexec" && chmod 644 "$work/notexec"
run --bin "$work/notexec"
check "a non-executable --bin exits 1 and writes nothing" test "$rc" -eq 1 -a -z "$(find "$home" -mindepth 1 2>/dev/null)"

fresh
out=$(cd "$work" && launch --bin fake/nvcontrol 2>"$work/err"); rc=$?
check "a relative --bin is installed with an absolute Exec line (rc=$rc)" grep -Fxq -- "Exec=$fake" "$desktop"

echo "test_launcher: $passes passed, $fails failed"
[ "$fails" -eq 0 ]
