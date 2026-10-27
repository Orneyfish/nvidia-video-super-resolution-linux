#!/usr/bin/env bash
# build-mpv-vsr.sh - build the private mpv with `--vf=vapoursynth` into ~/.local/opt/mpv-vsr (no sudo).
#
# Why: Arch's mpv 0.41.0 is built without the VapourSynth filter. mpv's meson looks for a pkg-config module
# `vapoursynth-script` that VapourSynth R80+ no longer ships (the library is libvapoursynth-script, `libvsscript`
# is gone), so we feed it a one-line shim .pc and keep Arch's other build options. The system mpv is not touched.
#
#   install/build-mpv-vsr.sh [--prefix DIR] [--jobs N] [--tarball FILE] [--keep-build] [--dry-run] [--yes]
#   install/build-mpv-vsr.sh --check-deps       list missing pkg-config modules / tools and the pacman line
#
# Recipe (tools/phase0-results.md has the history):
#   * https://github.com/mpv-player/mpv/archive/refs/tags/v0.41.0.tar.gz, sha256 pinned below (checked before extracting)
#   * <tmp>/pcshim/vapoursynth-script.pc : Requires: vapoursynth, Libs: -lvapoursynth-script
#   * meson setup with Arch's flags + -Dvapoursynth=enabled, --wrap-mode=nodownload (no hidden network access)
#   * staged install, then one rename into place; an existing prefix is kept as <prefix>.bak-nvvsr-<time>
# The extracted source is treated as data: nothing in it is run except meson/ninja and the build rules they read.
set -euo pipefail

MPV_VERSION=0.41.0
MPV_URL=https://github.com/mpv-player/mpv/archive/refs/tags/v${MPV_VERSION}.tar.gz
MPV_SHA256=ee21092a5ee427353392360929dc64645c54479aefdb5babc5cfbb5fad626209
MESON_FLAGS=(
  -Dlibmpv=true -Dgl-x11=enabled -Dcaca=disabled -Dcdda=enabled -Ddrm=enabled -Ddvbin=enabled -Ddvdnav=enabled
  -Dlibarchive=enabled -Dopenal=enabled -Dsdl2-audio=enabled -Dsdl2-video=enabled -Dsdl2-gamepad=enabled
  -Dvapoursynth=enabled -Dbuildtype=release
)
# pkg-config module -> Arch package(s)
MODULES=(
  "libavcodec:ffmpeg" "libavfilter:ffmpeg" "libavformat:ffmpeg" "libavutil:ffmpeg" "libswresample:ffmpeg" "libswscale:ffmpeg"
  "libplacebo:libplacebo" "libass:libass" "vapoursynth:vapoursynth" "luajit:luajit" "libdrm:libdrm" "dvdnav:libdvdnav"
  "libarchive:libarchive" "openal:openal" "sdl2:sdl2-compat" "libcdio_paranoia:libcdio-paranoia" "x11:libx11"
  "gl:libglvnd" "egl:libglvnd" "vulkan:vulkan-icd-loader vulkan-headers" "wayland-client:wayland"
  "wayland-protocols:wayland-protocols" "xkbcommon:libxkbcommon" "zlib:zlib"
)

PREFIX="${HOME:-/nonexistent}/.local/opt/mpv-vsr"
JOBS=$(nproc 2>/dev/null || echo 2)
TARBALL="" KEEP=0 DRY=0 YES=0 CHECK_ONLY=0

usage() {
  sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
}
err() { printf 'build-mpv-vsr: %s\n' "$*" >&2; }
die() { err "$1"; exit "${2:-1}"; }

while [ $# -gt 0 ]; do
  case $1 in
    --prefix) [ $# -ge 2 ] || die "--prefix needs a directory" 2; PREFIX=$2; shift ;;
    --prefix=*) PREFIX=${1#*=} ;;
    --jobs) [ $# -ge 2 ] || die "--jobs needs a number" 2; JOBS=$2; shift ;;
    --tarball) [ $# -ge 2 ] || die "--tarball needs a file" 2; TARBALL=$2; shift ;;
    --keep-build) KEEP=1 ;;
    --dry-run|-n) DRY=1 ;;
    --yes|-y) YES=1 ;;
    --check-deps) CHECK_ONLY=1 ;;
    -h|--help) usage; exit 0 ;;
    *) err "unknown argument: $1"; exit 2 ;;
  esac
  shift
done

case $PREFIX in
  /*) ;;
  *) die "--prefix must be an absolute path: $PREFIX" 2 ;;
esac
case $PREFIX in
  /|/usr|/usr/*|/etc|/etc/*|/bin|/bin/*|/lib|/lib/*|/opt|/var|/var/*|/boot|/boot/*)
    die "refusing prefix $PREFIX: this script installs into a user directory only" 2 ;;
esac
case $JOBS in
  ''|*[!0-9]*|0) die "--jobs must be a positive number" 2 ;;
esac
if [[ $PREFIX == *[[:space:]]* ]] || [[ $PREFIX == *[\'\"\\\$\`]* ]]; then die "the prefix must not contain spaces or shell characters" 2; fi
if [ "$(id -u)" = 0 ]; then die "do not run this as root" 2; fi

have() { command -v "$1" >/dev/null 2>&1; }

check_deps() { # sets MISSING_TOOLS (names) and PACKAGES (list), returns 1 when anything is missing
  local m pk spec missing_mods=() tools=() pkgs=() p
  for m in meson ninja pkg-config tar sha256sum; do
    if ! have "$m"; then
      tools+=("$m")
      case $m in meson) pkgs+=(meson) ;; ninja) pkgs+=(ninja) ;; pkg-config) pkgs+=(pkgconf) ;; *) pkgs+=(coreutils tar) ;; esac
    fi
  done
  if [ -z "$TARBALL" ] && ! have curl && ! have wget; then tools+=("curl or wget"); pkgs+=(curl); fi
  if ! have cc && ! have gcc; then tools+=("a C compiler"); pkgs+=(gcc); fi
  if have pkg-config; then
    for spec in "${MODULES[@]}"; do
      m=${spec%%:*}; pk=${spec#*:}
      if ! pkg-config --exists "$m" 2>/dev/null; then
        if [ "$m" = luajit ] && pkg-config --exists lua5.1 2>/dev/null; then continue; fi
        missing_mods+=("$m"); for p in $pk; do pkgs+=("$p"); done
      fi
    done
  fi
  if [ "${#tools[@]}" -eq 0 ] && [ "${#missing_mods[@]}" -eq 0 ]; then
    echo "all build tools and pkg-config modules found"
    return 0
  fi
  [ "${#tools[@]}" -eq 0 ] || echo "missing tools: ${tools[*]}"
  [ "${#missing_mods[@]}" -eq 0 ] || echo "missing pkg-config modules: ${missing_mods[*]}"
  echo "sudo pacman -S --needed $(printf '%s\n' "${pkgs[@]}" | sort -u | tr '\n' ' ')  # run it yourself; this script never does"
  return 1
}

plan() {
  cat <<EOF
mpv $MPV_VERSION with VapourSynth -> $PREFIX
  source   : ${TARBALL:-$MPV_URL}
  sha256   : $MPV_SHA256 (checked before extracting)
  shim     : <tmp>/pcshim/vapoursynth-script.pc  (Requires: vapoursynth, Libs: -lvapoursynth-script)
  meson    : setup --prefix=$PREFIX --wrap-mode=nodownload ${MESON_FLAGS[*]}
  build    : meson compile -j $JOBS in a mktemp directory, removed afterwards (unless --keep-build)
  install  : staged, then moved into place; an existing $PREFIX is kept as $PREFIX.bak-nvvsr-<time>
  check    : $PREFIX/bin/mpv --no-config --vf=help must list the vapoursynth filter
EOF
}

if [ "$CHECK_ONLY" = 1 ]; then
  check_deps
  exit $?
fi

plan
if [ "$DRY" = 1 ]; then
  echo "(dry run: nothing downloaded, nothing written)"
  if ! check_deps; then echo "(dependencies above are missing; a real run would stop here)"; fi
  exit 0
fi

if ! check_deps; then die "install the missing items first (see above)" 2; fi
if [ "$YES" = 0 ]; then
  if ! { : </dev/tty; } 2>/dev/null; then die "no terminal to confirm on; re-run with --yes" 3; fi
  printf 'Build mpv (10+ minutes of CPU, a ~2 MB download) into %s? [y/N] ' "$PREFIX" >/dev/tty
  read -r answer </dev/tty || answer=n
  case $answer in y|Y|yes|YES) ;; *) echo "Not confirmed."; exit 3 ;; esac
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/nvvsr-mpv-build.XXXXXX")
chmod 700 "$WORK"
cleanup() {
  local rc=$?
  trap - EXIT
  if [ "$KEEP" = 1 ]; then echo "build directory kept: $WORK"; else rm -rf -- "$WORK"; fi
  exit "$rc"
}
trap cleanup EXIT

# 1. the tarball: downloaded over https (or given), verified BEFORE it is extracted
if [ -n "$TARBALL" ]; then
  [ -f "$TARBALL" ] || die "--tarball: $TARBALL does not exist"
  cp -- "$TARBALL" "$WORK/mpv.tar.gz"
else
  echo "downloading $MPV_URL"
  if have curl; then curl --fail --location --silent --show-error --proto '=https' --tlsv1.2 -o "$WORK/mpv.tar.gz" "$MPV_URL"
  else wget --quiet --https-only -O "$WORK/mpv.tar.gz" "$MPV_URL"; fi
fi
got=$(sha256sum -- "$WORK/mpv.tar.gz" | cut -d' ' -f1)
if [ "$got" != "$MPV_SHA256" ]; then die "sha256 mismatch: got $got, expected $MPV_SHA256; nothing was extracted"; fi
echo "sha256 ok: $got"

# 2. extract as data: refuse absolute or parent-relative member names, never keep owner or setuid bits
if tar -tzf "$WORK/mpv.tar.gz" | grep -Eq '(^/|(^|/)\.\.(/|$))'; then die "the archive has unsafe member names; refusing"; fi
mkdir "$WORK/src"
tar -xzf "$WORK/mpv.tar.gz" -C "$WORK/src" --strip-components=1 --no-same-owner --no-same-permissions
[ -f "$WORK/src/meson.build" ] || die "the archive does not contain meson.build"

# 3. the pkg-config shim for VapourSynth R80+
mkdir "$WORK/pcshim"
vsver=$(pkg-config --modversion vapoursynth)
cat >"$WORK/pcshim/vapoursynth-script.pc" <<EOF
Name: vapoursynth-script
Description: shim: VapourSynth R80+ ships libvapoursynth-script but no vapoursynth-script.pc
Version: $vsver
Requires: vapoursynth
Libs: -lvapoursynth-script
EOF
export PKG_CONFIG_PATH="$WORK/pcshim${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --exists vapoursynth-script || die "the vapoursynth-script shim does not resolve"

# 4. configure, build, stage
echo "configuring (log: $WORK/meson.log)"
(cd "$WORK" && meson setup build src --prefix="$PREFIX" --wrap-mode=nodownload "${MESON_FLAGS[@]}" >"$WORK/meson.log" 2>&1) ||
  { tail -n 30 "$WORK/meson.log" >&2; die "meson setup failed (log kept with --keep-build)"; }
echo "building with $JOBS jobs"
(cd "$WORK" && meson compile -C build -j "$JOBS" >>"$WORK/meson.log" 2>&1) ||
  { tail -n 30 "$WORK/meson.log" >&2; die "the build failed"; }
(cd "$WORK" && meson install -C build --no-rebuild --destdir "$WORK/stage" >>"$WORK/meson.log" 2>&1) ||
  { tail -n 30 "$WORK/meson.log" >&2; die "meson install into the staging directory failed"; }
[ -x "$WORK/stage$PREFIX/bin/mpv" ] || die "the staged tree has no bin/mpv"

# 5. check the staged binary before anything is replaced (headless: no GPU, no window)
staged_mpv="$WORK/stage$PREFIX/bin/mpv"
mkdir "$WORK/home"
if ! env -u DISPLAY -u WAYLAND_DISPLAY HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home" \
     "$staged_mpv" --no-config --vf=help 2>&1 | grep -Eq '^[[:space:]]+vapoursynth[[:space:]]'; then
  die "the built mpv does not list the vapoursynth filter"
fi

# 6. move into place: copy next to the target (same filesystem), then rename
mkdir -p -- "$(dirname -- "$PREFIX")"
new="$PREFIX.new.$$"
cp -a -- "$WORK/stage$PREFIX" "$new"
if [ -e "$PREFIX" ]; then
  old="$PREFIX.bak-nvvsr-$(date -u +%Y%m%dT%H%M%S)"
  mv -- "$PREFIX" "$old"
  echo "previous tree kept as $old (delete it when you are happy)"
fi
mv -- "$new" "$PREFIX"
echo "done: $PREFIX/bin/mpv ($("$PREFIX/bin/mpv" --version 2>/dev/null | head -1 || true))"
