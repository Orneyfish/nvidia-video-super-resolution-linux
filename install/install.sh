#!/usr/bin/env bash
# install.sh - user-level installer for NVIDIA RTX video super resolution (mpv, VLC).
#
#   install/install.sh check                 what is present / missing (prints sudo pacman -S lines, never runs them)
#   install/install.sh install [options]     build + install under the prefix, wire up mpv and VLC
#   install/install.sh uninstall [options]   remove exactly what install.json lists, restore the backups it made
#   install/install.sh status                what is installed, is it intact
#
# Nothing is written outside the user's home directory: no sudo, no pacman, nothing under /usr. Every file the
# installer creates or changes is listed in <prefix>/install.json (schema: docs/install-layout.md).
# Test hooks (environment): NVVSR_INSTALL_SKIP_GPU_CHECK=1 never touches the GPU; NVVSR_INSTALL_SKIP_MPV_BUILD=1
# makes --build-mpv a no-op; NVVSR_WHEEL_URL / NVVSR_WHEEL_SHA256 replace the pinned wheel URL and hash;
# NVVSR_INSTALL_PYLIBDIR replaces /usr/lib as the place where the vapoursynth.toml fallback looks for libpython;
# NVVSR_PANEL_BIN is the nvcontrol binary --with-panel installs instead of building it.
# NVVSR_WEBD_BIN is the nvvsr_webd binary --with-chrome installs instead of building it.
set -euo pipefail

SELF=$(readlink -f -- "${BASH_SOURCE[0]}")
INSTALL_DIR=${SELF%/*}
REPO=${INSTALL_DIR%/*}
MANIFEST_SH=$INSTALL_DIR/lib/manifest.sh
BUILD_MPV_SH=$INSTALL_DIR/build-mpv-vsr.sh

# Pinned wheel: the cp312-abi3 manylinux file listed in https://pypi.nvidia.com/nvidia-vfx/ for nvidia-vfx 0.2.0.0; the
# index's #sha256 fragment is the pin. A wheel is a zip: only nvvfx/libs and nvidia_vfx.libs are unpacked.
WHEEL_NAME=nvidia-vfx
WHEEL_VERSION=0.2.0.0
WHEEL_FILE=nvidia_vfx-0.2.0.0-cp312-abi3-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl
WHEEL_SHA256=${NVVSR_WHEEL_SHA256:-0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8}
WHEEL_URL=${NVVSR_WHEEL_URL:-https://pypi.nvidia.com/nvidia-vfx/$WHEEL_FILE}
MIN_DRIVER=570.190
KS=$'\037'                    # separator inside record keys ("type<KS>path")
TS=$(date -u +%Y%m%dT%H%M%S)  # one timestamp per run: the suffix of every backup it makes

CMD="" OPT_PREFIX="" OPT_HOME="" OPT_MPV_BIN=""
DRY_RUN=0 YES=0 NO_VLC=0 NO_MPV=0 SKIP_WHEEL=0 BUILD_MPV=0 WITH_PANEL=0 WITH_CHROME=0 PURGE_MPV=0
SKIP_GPU=${NVVSR_INSTALL_SKIP_GPU_CHECK:-0}

usage() {
  cat <<'EOF'
Usage: install/install.sh COMMAND [OPTIONS]

Commands
  check       report what is present or missing; prints `sudo pacman -S ...` lines (never runs them)
  install     build and install under the prefix, wire up mpv (mpv-sr) and VLC (vlc-sr)
  uninstall   remove exactly what install.json lists and restore the backups it made
  status      show what is installed and whether it is intact

Options
  --prefix DIR           install prefix (default: $XDG_DATA_HOME/nvvsr = ~/.local/share/nvvsr)
  --home DIR             act on DIR instead of $HOME (tests); XDG_* variables are then derived from DIR
  --dry-run              list what would change, change nothing
  --yes                  no prompts (without it every change is listed and confirmed on /dev/tty;
                         with no terminal and no --yes the installer refuses)
  --no-mpv               skip the VapourSynth plugin, the mpv script, mpv-sr and the vapoursynth.toml line
  --no-vlc               skip the VLC module and vlc-sr
  --skip-wheel           do not download the nvidia-vfx wheel; use <prefix>/vfx if it is there (offline, development)
  --mpv-bin PATH         private mpv (default: ~/.local/opt/mpv-vsr/bin/mpv)
  --build-mpv            build the private mpv with install/build-mpv-vsr.sh when it is missing or lacks vapoursynth
  --with-panel           also build nvcontrol (panel/, C++), install it under <prefix>/bin and run its launcher
  --with-chrome          also build nvvsr_webd (webgpu/daemon/, the Chrome upscaling daemon), install it under <prefix>/bin and
                         enable its systemd user socket nvvsr-webd.socket (the service starts on the first connection)
  --purge-mpv            uninstall: also delete the private mpv tree
  -h, --help             this text

Exit status: 0 ok, 1 failure, 2 usage error or missing requirements, 3 refused / not confirmed.
EOF
}

err() { printf 'error: %s\n' "$*" >&2; }
warn() { printf 'warning: %s\n' "$*" >&2; }
say() { printf '%s\n' "$*"; }
die() { err "$1"; exit "${2:-1}"; }

parse_args() {
  local val
  while [ $# -gt 0 ]; do
    val=""
    case $1 in --prefix|--home|--mpv-bin)
      if [ $# -lt 2 ]; then die "$1 needs a value" 2; fi
      val=$2 ;;
    esac
    case $1 in
      check|install|uninstall|status)
        if [ -n "$CMD" ]; then die "more than one command ($CMD, $1)" 2; fi
        CMD=$1 ;;
      --prefix) OPT_PREFIX=$val; shift ;;
      --home) OPT_HOME=$val; shift ;;
      --mpv-bin) OPT_MPV_BIN=$val; shift ;;
      --dry-run) DRY_RUN=1 ;;
      --yes) YES=1 ;;
      --no-vlc) NO_VLC=1 ;;
      --no-mpv) NO_MPV=1 ;;
      --skip-wheel) SKIP_WHEEL=1 ;;
      --build-mpv) BUILD_MPV=1 ;;
      --with-panel) WITH_PANEL=1 ;;
      --with-chrome) WITH_CHROME=1 ;;
      --purge-mpv) PURGE_MPV=1 ;;
      -h|--help) usage; exit 0 ;;
      *) err "unknown argument: $1"; usage >&2; exit 2 ;;
    esac
    shift
  done
  if [ -z "$CMD" ]; then usage >&2; exit 2; fi
}

# ------------------------------------------------------------------------------------------------ paths
HOME_DIR="" CONFIG_HOME="" DATA_HOME="" BIN_DIR="" PREFIX="" BUILD_DIR="" NVVSR_CONF_DIR="" VS_TOML=""
MPV_CONF_DIR="" VLC_PLUGIN_DIR="" MPV_BIN="" VFX="" LIB_DIR="" MPV_DIR="" MANIFEST="" JOBS=2

# A path ends up in scripts, RUNPATH, mpv's --script list, JSON and the libdir file: keep it to a safe alphabet.
check_path_chars() { # label path
  case $2 in /*) ;; *) die "$1 must be an absolute path: $2" 2 ;; esac
  if [[ $2 =~ [[:cntrl:]] ]] || [[ $2 == *[:,\"\\\|]* ]]; then
    die "$1 contains a character that is not allowed (control, colon, comma, quote, backslash, bar): $2" 2
  fi
}

resolve_env() {
  local h
  if [ -n "$OPT_HOME" ]; then
    [ -d "$OPT_HOME" ] || die "--home: $OPT_HOME is not a directory" 2
    h=$(readlink -f -- "$OPT_HOME")
    unset XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME XDG_STATE_HOME MPV_HOME
  else
    h=${HOME:-}
    [ -n "$h" ] && [ -d "$h" ] || die "HOME is not set to a directory" 2
    h=$(readlink -f -- "$h")
  fi
  HOME_DIR=$h
  export HOME=$HOME_DIR
  if [ "$(id -u)" = 0 ]; then die "do not run the installer as root (it installs into a home directory)" 2; fi
  case ${XDG_CONFIG_HOME:-} in /*) CONFIG_HOME=$XDG_CONFIG_HOME ;; *) CONFIG_HOME=$HOME_DIR/.config ;; esac
  case ${XDG_DATA_HOME:-} in /*) DATA_HOME=$XDG_DATA_HOME ;; *) DATA_HOME=$HOME_DIR/.local/share ;; esac
  case ${MPV_HOME:-} in /*) MPV_CONF_DIR=$MPV_HOME ;; *) MPV_CONF_DIR=$CONFIG_HOME/mpv ;; esac
  BIN_DIR=$HOME_DIR/.local/bin
  NVVSR_CONF_DIR=$CONFIG_HOME/nvvsr
  VS_TOML=$CONFIG_HOME/vapoursynth/vapoursynth.toml
  VLC_PLUGIN_DIR=$DATA_HOME/vlc/plugins
  PREFIX=${OPT_PREFIX:-$DATA_HOME/nvvsr}
  PREFIX=${PREFIX%/}
  MPV_BIN=${OPT_MPV_BIN:-$HOME_DIR/.local/opt/mpv-vsr/bin/mpv}
  for h in "$HOME_DIR" "$PREFIX" "$MPV_BIN" "$CONFIG_HOME" "$DATA_HOME"; do check_path_chars path "$h"; done
  case $PREFIX in
    /|/usr|/usr/*|/etc|/etc/*|/bin|/bin/*|/sbin|/sbin/*|/lib|/lib/*|/lib64|/lib64/*|/boot|/boot/*|/dev|/dev/*|/proc|/proc/*|/sys|/sys/*|/run|/run/*|"$HOME_DIR")
      die "refusing prefix $PREFIX (system directory or the home directory itself)" 2 ;;
  esac
  case $HOME_DIR in "$PREFIX"|"$PREFIX"/*) die "the prefix $PREFIX contains the home directory" 2 ;; esac
  BUILD_DIR=$PREFIX/build
  VFX=$PREFIX/vfx
  LIB_DIR=$PREFIX/lib
  MPV_DIR=$PREFIX/mpv
  MANIFEST=$PREFIX/install.json
  JOBS=$(nproc 2>/dev/null || echo 2)
}

# ------------------------------------------------------------------------------------------------ temp + cleanup
STATE="" EXEC_STARTED=0
TMPFILES=()
init_state() {
  STATE=$(mktemp -d "${TMPDIR:-/tmp}/nvvsr-install.XXXXXX")
  chmod 700 "$STATE"
}
on_exit() {
  local rc=$? f
  trap - EXIT
  if [ "$EXEC_STARTED" = 1 ]; then save_manifest || true; fi   # a failed run still leaves a manifest to uninstall by
  for f in "${TMPFILES[@]:-}"; do if [ -n "$f" ]; then rm -f -- "$f" 2>/dev/null || true; fi; done
  if [ -n "$STATE" ] && [ -d "$STATE" ]; then rm -rf -- "$STATE"; fi
  exit "$rc"
}
trap on_exit EXIT

# ------------------------------------------------------------------------------------------------ state: records
# DRY=1 is the plan pass: the same code runs, prints "would ..." and records nothing.
DRY=0 CHANGES=0 FAILED=0
declare -A META=() PATHS=() CSTAT=() CDET=()
declare -A RTYPE=() RCOMP=() RCRE=() RBAK=() RSHA=() REXTRA=()
RKEYS=() CORDER=()
declare -A PLANNED=()   # directories the plan pass already announced
BK=""

sha_of() { sha256sum -- "$1" | cut -d' ' -f1; }
have() { command -v "$1" >/dev/null 2>&1; }

rec_put() { # type path comp created backup sha extra
  if [ "$DRY" = 1 ]; then return 0; fi
  local k=$1$KS$2
  if [ -z "${RTYPE[$k]+x}" ]; then RKEYS+=("$k"); fi
  RTYPE[$k]=$1; RCOMP[$k]=$3; RCRE[$k]=$4; RBAK[$k]=$5; RSHA[$k]=$6; REXTRA[$k]=$7
}
rec_has() { [ -n "${RTYPE[$1$KS$2]+x}" ]; }
set_comp() { # name status detail
  if [ "$DRY" = 1 ]; then return 0; fi
  if [ -z "${CSTAT[$1]+x}" ]; then CORDER+=("$1"); fi
  CSTAT[$1]=$2; CDET[$1]=${3//[$'\t\n']/ }
}
skip_comp() { # "not requested" never overwrites what an earlier run installed
  if [ -z "${CSTAT[$1]+x}" ]; then set_comp "$1" skipped "$2"; fi
}
set_path() { if [ "$DRY" = 0 ]; then PATHS[$1]=$2; fi; }

act() { # verb target
  CHANGES=$((CHANGES + 1))
  if [ "$DRY" = 1 ]; then printf '  would %-8s %s\n' "$1" "$2"; else printf '  %-8s %s\n' "$1" "$2"; fi
}

emit_state() { # TSV for manifest.sh ("-" stands for an empty field)
  local k key
  tsv() { local IFS=$'\t'; printf '%s\n' "$*"; }
  for key in version git_commit git_dirty repo prefix home config_home data_home nvvsr_libdir \
             wheel_version wheel_name wheel_sha256 wheel_installed mpv_path mpv_vapoursynth mpv_built; do
    if [ -n "${META[$key]+x}" ]; then tsv meta "$key" "${META[$key]:--}"; fi
  done
  for key in "${!PATHS[@]}"; do tsv path "$key" "${PATHS[$key]:--}"; done
  for key in "${CORDER[@]:-}"; do
    if [ -n "$key" ]; then tsv comp "$key" "${CSTAT[$key]}" "${CDET[$key]:--}"; fi
  done
  for k in "${RKEYS[@]:-}"; do
    if [ -n "$k" ]; then
      tsv entry "${RTYPE[$k]}" "${k#*"$KS"}" "${RCOMP[$k]:--}" "${RCRE[$k]:-0}" "${RBAK[$k]:--}" "${RSHA[$k]:--}" "${REXTRA[$k]:--}"
    fi
  done
}

save_manifest() {
  if [ "$DRY" = 1 ] || [ ! -d "$PREFIX" ]; then return 0; fi
  emit_state | "$MANIFEST_SH" to-json "$MANIFEST"
}

load_manifest() { # FILE : fills META/PATHS/components/records from an existing install.json
  local kind a b c d e g h k
  [ -f "$1" ] || return 0
  "$MANIFEST_SH" validate "$1" >/dev/null || { warn "$1 is not a valid install.json; ignoring its contents"; return 1; }
  while IFS=$'\t' read -r kind a b c d e g h; do
    [ "$b" != - ] || b=""; [ "$c" != - ] || c=""; [ "$e" != - ] || e=""; [ "$g" != - ] || g=""; [ "$h" != - ] || h=""
    case $kind in
      meta) META[$a]=$b ;;
      path) PATHS[$a]=$b ;;
      comp) if [ -z "${CSTAT[$a]+x}" ]; then CORDER+=("$a"); fi
            CSTAT[$a]=$b; CDET[$a]=$c ;;
      entry) # a type, b path, c component, d created, e backup, g sha256, h extra
        k=$a$KS$b
        if [ -z "${RTYPE[$k]+x}" ]; then RKEYS+=("$k"); fi
        RTYPE[$k]=$a; RCOMP[$k]=$c; RCRE[$k]=$d; RBAK[$k]=$e; RSHA[$k]=$g; REXTRA[$k]=$h ;;
    esac
  done < <("$MANIFEST_SH" to-tsv "$1")
}

# ------------------------------------------------------------------------------------------------ primitives
make_backup() { # path -> BK : timestamped copy next to it (cp -a keeps mode, mtime and symlinks)
  local n=1
  BK=$1.bak-nvvsr-$TS
  while [ -e "$BK" ] || [ -L "$BK" ]; do BK=$1.bak-nvvsr-$TS-$n; n=$((n + 1)); done
  act backup "$1 -> $BK"
  if [ "$DRY" = 0 ]; then cp -a -- "$1" "$BK" || die "cannot back up $1"; fi
}

ensure_dir() { # dir comp : mkdir -p, recording every directory that did not exist
  local p=$1 missing=() i
  while [ ! -e "$p" ] && [ ! -L "$p" ]; do missing+=("$p"); p=$(dirname "$p"); done
  if [ ! -d "$p" ]; then die "$p exists and is not a directory"; fi
  for ((i = ${#missing[@]} - 1; i >= 0; i--)); do
    if [ "$DRY" = 1 ]; then
      if [ -n "${PLANNED[${missing[$i]}]+x}" ]; then continue; fi
      PLANNED[${missing[$i]}]=1
    fi
    act mkdir "${missing[$i]}"
    if [ "$DRY" = 0 ]; then
      mkdir -m 755 -- "${missing[$i]}" || die "cannot create ${missing[$i]}"
      rec_put dir "${missing[$i]}" "$2" 1 "" "" ""
    fi
  done
}

write_atomic() { # src dest mode : temp file next to dest, then rename
  local tmp
  tmp=$(mktemp "$2.nvvsr-tmp.XXXXXX") || die "cannot write next to $2"
  TMPFILES+=("$tmp")
  cp -- "$1" "$tmp"
  chmod "$3" "$tmp"
  mv -f -- "$tmp" "$2"
}

# Installs SRC as DEST. A file we did not create is backed up first; one the user changed after our last write is
# kept next to it as a backup; "create-only" never touches an existing file.
install_file() { # src dest mode comp [create-only]
  local src=$1 dest=$2 mode=$3 comp=$4 flag=${5:-}
  local k=file$KS$dest want cur="" created=1 backup="" have_rec=0
  if rec_has file "$dest"; then have_rec=1; fi
  if [ ! -f "$src" ]; then
    if [ "$DRY" = 1 ]; then act install "$dest (after the build)"; return 0; fi
    err "missing source file $src"; return 1
  fi
  want=$(sha_of "$src")
  if [ -e "$dest" ] || [ -L "$dest" ]; then
    if [ -f "$dest" ] && [ ! -L "$dest" ]; then
      cur=$(sha_of "$dest")
      if [ "$cur" = "$want" ]; then return 0; fi
    fi
    if [ "$flag" = create-only ]; then return 0; fi
    if [ "$have_rec" = 1 ]; then
      created=${RCRE[$k]}; backup=${RBAK[$k]}
      if [ "$cur" != "${RSHA[$k]}" ]; then make_backup "$dest"; fi   # the user changed our file: keep their copy
    else
      make_backup "$dest"; backup=$BK; created=0
    fi
    act replace "$dest"
  else
    ensure_dir "$(dirname "$dest")" "$comp"
    if [ "$have_rec" = 1 ]; then created=${RCRE[$k]}; backup=${RBAK[$k]}; fi
    act create "$dest"
  fi
  if [ "$DRY" = 0 ]; then
    write_atomic "$src" "$dest" "$mode"
    rec_put file "$dest" "$comp" "$created" "$backup" "$want" ""
  fi
}

install_symlink() { # target dest comp
  local target=$1 dest=$2 k=symlink$KS$2 created=1 backup="" tmp
  if [ -L "$dest" ] && [ "$(readlink -- "$dest")" = "$target" ]; then return 0; fi
  if [ -e "$dest" ] || [ -L "$dest" ]; then
    if rec_has symlink "$dest"; then created=${RCRE[$k]}; backup=${RBAK[$k]}
    else make_backup "$dest"; backup=$BK; created=0; fi
    act replace "$dest -> $target"
  else
    ensure_dir "$(dirname "$dest")" "$3"
    act symlink "$dest -> $target"
  fi
  if [ "$DRY" = 0 ]; then
    tmp=$dest.nvvsr-tmp.$$
    ln -s -- "$target" "$tmp"
    mv -T -f -- "$tmp" "$dest"
    rec_put symlink "$dest" "$3" "$created" "$backup" "" "$target"
  fi
}

# Generated content goes to $STATE/gen-NAME first (a pipeline runs in a subshell, so no variable is returned).
gen_file() { cat >"$STATE/gen-$1"; }

NB_DEPS=()
build_component() { # name srcdir realoutput cmake-args... (NB_DEPS: files whose change forces a rebuild)
  local name=$1 src=$2 out=$3 bd log
  shift 3
  bd=$BUILD_DIR/$name; log=$BUILD_DIR/logs/$name.log
  # up to date: the output exists and nothing it is built from is newer
  if [ -f "$out" ] && [ -z "$(find -L "${NB_DEPS[@]}" -type f -newer "$out" -print -quit 2>/dev/null || true)" ]; then return 0; fi
  act build "$name ($bd)"
  if [ "$DRY" = 1 ]; then return 0; fi
  if [ ! -d "$BUILD_DIR" ]; then rec_put tree "$BUILD_DIR" build 1 "" "" ""; fi
  mkdir -p -- "$BUILD_DIR/logs"
  if ! { cmake -S "$src" -B "$bd" -DCMAKE_BUILD_TYPE=Release "$@" && cmake --build "$bd" -j "$JOBS"; } >"$log" 2>&1; then
    err "the $name build failed; last lines of $log:"
    tail -n 25 "$log" >&2
    return 1
  fi
}

# ------------------------------------------------------------------------------------------------ facts
core_version() { sed -n 's/^project(nvvsr VERSION \([0-9][0-9.]*\).*/\1/p' "$REPO/core/CMakeLists.txt" | head -1; }

ver_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; } # A >= B (dotted versions)

driver_version() { # never called in GPU-free mode
  local v=""
  if [ -r /proc/driver/nvidia/version ]; then
    v=$(sed -n 1p /proc/driver/nvidia/version | grep -oE '(^| )[0-9]{3}\.[0-9]+(\.[0-9]+)?( |$)' | head -1 | tr -d ' ' || true)
  fi
  if [ -z "$v" ] && have nvidia-smi; then v=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null | head -1 || true); fi
  printf '%s' "$v"
}

# The wheel's two library directories, colon separated (the value of <config>/nvvsr/libdir)
wheel_libdir() { printf '%s:%s' "$VFX/nvvfx/libs" "$VFX/nvidia_vfx.libs"; }
wheel_present() { [ -f "$VFX/nvvfx/libs/libnvVFXVideoSuperRes.so" ] && [ -d "$VFX/nvidia_vfx.libs" ]; }

mpv_has_vapoursynth() { # BIN : GPU-free, only prints the filter list
  local out
  [ -x "$1" ] || return 1
  out=$(env -u DISPLAY -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS HOME="$STATE" XDG_CONFIG_HOME="$STATE" \
        XDG_CACHE_HOME="$STATE" XDG_STATE_HOME="$STATE" XDG_RUNTIME_DIR="$STATE" \
        timeout 30 "$1" --no-config --vf=help 2>&1 || true)
  printf '%s\n' "$out" | grep -Eq '^[[:space:]]+vapoursynth[[:space:]]'
}

find_vsscript() {
  local p=""
  if have ldconfig; then p=$(ldconfig -p 2>/dev/null | sed -n 's/^[[:space:]]*libvsscript\.so[^=]*=>[[:space:]]*//p' | head -1 || true); fi
  printf '%s' "${p:-/usr/lib/libvsscript.so}"
}

# ------------------------------------------------------------------------------------------------ check
declare -A NEED_PKG=()
NEED_NOTES=()
HARD_MISSING=0 SOFT_MISSING=0
row() { printf '  %-8s %-18s %s\n' "$1" "$2" "$3"; }
r_ok() { row ok "$1" "$2"; }
r_miss() { # name detail pkgs hard(1|0)
  local tag=missing p
  if [ "${4:-1}" = 0 ]; then tag=optional; SOFT_MISSING=$((SOFT_MISSING + 1)); else HARD_MISSING=$((HARD_MISSING + 1)); fi
  row "$tag" "$1" "$2"
  for p in $3; do NEED_PKG[$p]=1; done
}
check_tool() { if have "$2"; then r_ok "$1" "$(command -v "$2")"; else r_miss "$1" "not found" "$3" "${4:-1}"; fi; } # name cmd pkg hard

run_checks() {
  local dv m
  say "Requirements (components: core, wheel$([ "$NO_MPV" = 0 ] && echo ', mpv')$([ "$NO_VLC" = 0 ] && echo ', vlc'))"
  if [ "$SKIP_GPU" = 1 ]; then
    row note nvidia-driver "skipped (GPU-free mode)"
  else
    dv=$(driver_version)
    if [ -z "$dv" ]; then
      row missing nvidia-driver "no NVIDIA driver found; need >= $MIN_DRIVER"; SOFT_MISSING=$((SOFT_MISSING + 1))
      NEED_NOTES+=("install or update the NVIDIA driver package (nvidia-open / nvidia-utils) to >= $MIN_DRIVER")
    elif ver_ge "$dv" "$MIN_DRIVER"; then r_ok nvidia-driver "$dv (>= $MIN_DRIVER)"
    else
      row missing nvidia-driver "$dv is older than $MIN_DRIVER"; SOFT_MISSING=$((SOFT_MISSING + 1))
      NEED_NOTES+=("update the NVIDIA driver to >= $MIN_DRIVER")
    fi
  fi
  check_tool curl curl curl 1
  check_tool unzip unzip unzip 1
  check_tool sha256sum sha256sum coreutils 1
  if have cmake; then
    m=$(cmake --version | sed -n '1s/[^0-9]*\([0-9][0-9.]*\).*/\1/p')
    if ver_ge "$m" 3.20; then r_ok cmake "$m"; else r_miss cmake "$m is older than 3.20" cmake 1; fi
  else r_miss cmake "not found" cmake 1; fi
  if have c++ || have g++; then r_ok c++-compiler "$(command -v c++ || command -v g++)"; else r_miss c++-compiler "not found" gcc 1; fi
  if have cc || have gcc; then r_ok c-compiler "$(command -v cc || command -v gcc)"; else r_miss c-compiler "not found" gcc 1; fi
  if have make || have ninja; then r_ok make/ninja "$(command -v make || command -v ninja)"; else r_miss make/ninja "not found" make 1; fi
  check_tool pkg-config pkg-config pkgconf 1
  check_tool jq jq jq 1
  if [ "$NO_MPV" = 0 ]; then
    if have pkg-config && pkg-config --exists vapoursynth 2>/dev/null; then
      m=$(pkg-config --variable=includedir vapoursynth 2>/dev/null || echo /usr/include)
      if [ -f "$m/vapoursynth/VapourSynth4.h" ] || [ -f "$m/VapourSynth4.h" ]; then r_ok vapoursynth "R$(pkg-config --modversion vapoursynth), VapourSynth4.h"
      else r_miss vapoursynth "VapourSynth4.h not found under $m" vapoursynth 1; fi
    else r_miss vapoursynth "pkg-config vapoursynth failed" vapoursynth 1; fi
    m=$(find_vsscript)
    if [ -e "$m" ]; then r_ok libvsscript "$m"; else r_miss libvsscript "not found" vapoursynth 1; fi
    if mpv_has_vapoursynth "$MPV_BIN"; then r_ok private-mpv "$MPV_BIN (--vf=help lists vapoursynth)"
    else
      r_miss private-mpv "$MPV_BIN missing or without vapoursynth; install --build-mpv builds it" "" 0
      if m=$("$BUILD_MPV_SH" --check-deps 2>&1); then r_ok mpv-build-deps "tools and pkg-config modules for install/build-mpv-vsr.sh found"
      else
        r_miss mpv-build-deps "needed only to build the private mpv:" "" "$BUILD_MPV"
        printf '%s\n' "$m" | sed 's/^/             /'
      fi
    fi
  fi
  if [ "$NO_VLC" = 0 ]; then
    if have pkg-config && pkg-config --exists 'vlc-plugin >= 3.0' 2>/dev/null; then r_ok vlc-headers "vlc-plugin $(pkg-config --modversion vlc-plugin)"
    else r_miss vlc-headers "pkg-config vlc-plugin >= 3.0 failed" vlc 1; fi
    check_tool vlc vlc vlc 1
  fi
  check_tool ffmpeg ffmpeg ffmpeg 0
  check_tool git git git 0
}

print_pkg_hints() {
  local p n list=()
  for p in "${!NEED_PKG[@]}"; do list+=("$p"); done
  if [ "${#list[@]}" -gt 0 ]; then
    say ""
    if have pacman; then
      say "Install the missing packages yourself (this script never runs sudo or pacman):"
      say "  sudo pacman -S --needed $(printf '%s\n' "${list[@]}" | sort | tr '\n' ' ')"
    else
      say "Missing packages (Arch names; no pacman here): $(printf '%s\n' "${list[@]}" | sort | tr '\n' ' ')"
    fi
  fi
  for n in "${NEED_NOTES[@]:-}"; do if [ -n "$n" ]; then say "  note: $n"; fi; done
}

cmd_check() {
  resolve_env
  init_state
  say "Prefix $PREFIX, home $HOME_DIR; installed: $([ -f "$MANIFEST" ] && echo yes || echo no)"
  run_checks
  print_pkg_hints
  if [ "$HARD_MISSING" -gt 0 ]; then say "$HARD_MISSING required item(s) missing."; return 1; fi
  if [ "$SOFT_MISSING" -gt 0 ]; then say "All required items present; $SOFT_MISSING optional/GPU item(s) missing."; return 0; fi
  say "Everything needed is present."
}

# ------------------------------------------------------------------------------------------------ install steps
NVVSR_LIBDIR_VALUE="" MPV_OK=0 MPV_NOTE=""
WANT_MPV=1 WANT_VLC=1

step_failed() { # component detail
  set_comp "$1" failed "$2"
  err "$1: $2"
  FAILED=$((FAILED + 1))
}

step_core() {
  local ver real name
  ver=$(core_version)
  NB_DEPS=("$REPO/core/src" "$REPO/core/include" "$REPO/core/CMakeLists.txt" "$REPO/core/third_party")
  real=$(readlink -f -- "$BUILD_DIR/core/libnvvsr.so" 2>/dev/null || true)
  if [ -z "$real" ] || [ ! -f "$real" ]; then real=$BUILD_DIR/core/libnvvsr.so.$ver; fi
  if ! build_component core "$REPO/core" "$real" -DNVVSR_BUILD_TESTS=OFF; then step_failed core "build failed (see $BUILD_DIR/logs/core.log)"; return 1; fi
  name=$(basename -- "$real")
  install_file "$real" "$LIB_DIR/$name" 755 core || { step_failed core "cannot install $name"; return 1; }
  install_symlink "$name" "$LIB_DIR/libnvvsr.so.0" core
  install_symlink "libnvvsr.so.0" "$LIB_DIR/libnvvsr.so" core
  install_file "$BUILD_DIR/core/nvvsr_probe" "$PREFIX/bin/nvvsr_probe" 755 core || { step_failed core "cannot install nvvsr_probe"; return 1; }
  set_comp core installed "libnvvsr $ver in $LIB_DIR"
  set_path lib "$LIB_DIR"
  set_path libnvvsr "$LIB_DIR/libnvvsr.so.0"
  set_path nvvsr_probe "$PREFIX/bin/nvvsr_probe"
}

# The plugins find libnvvsr.so.0 through RUNPATH: $PREFIX/lib (absolute, so a copy in VLC's directory works too) and $ORIGIN.
step_plugin() { # name(=component) srcdir lib-file cmake-tests-option
  NB_DEPS=("$REPO/$2/src" "$REPO/$2/CMakeLists.txt" "$REPO/core/include")
  if ! build_component "$1" "$REPO/$2" "$BUILD_DIR/$1/$3" "-DNVVSR_CORE_BUILD_DIR=$LIB_DIR" "$4=OFF"; then
    step_failed "$1" "build failed (see $BUILD_DIR/logs/$1.log)"; return 1
  fi
  install_file "$BUILD_DIR/$1/$3" "$LIB_DIR/$3" 755 "$1" || { step_failed "$1" "cannot install $3"; return 1; }
  set_comp "$1" installed "$3 in $LIB_DIR"
  set_path "${1}_plugin" "$LIB_DIR/$3"
}

step_wheel() {
  local whl=$STATE/$WHEEL_FILE got ver
  if [ "$SKIP_WHEEL" = 1 ]; then
    if wheel_present; then NVVSR_LIBDIR_VALUE=$(wheel_libdir); set_comp wheel installed "existing $VFX (--skip-wheel)"
    else set_comp wheel skipped "--skip-wheel and no wheel libraries in $VFX"; fi
    return 0
  fi
  if ! wheel_present; then
    act wheel "$WHEEL_NAME==$WHEEL_VERSION from $WHEEL_URL, sha256 $WHEEL_SHA256 (about 440 MB)"
    if [ "$DRY" = 0 ]; then
      if ! curl -fL --show-error -o "$whl" "$WHEEL_URL"; then
        step_failed wheel "download failed (offline, or $WHEEL_URL unreachable)"; return 1
      fi
      got=$(sha_of "$whl")
      if [ "$got" != "$WHEEL_SHA256" ]; then
        rm -f -- "$whl"; step_failed wheel "sha256 is $got, pinned $WHEEL_SHA256; the download was deleted"; return 1
      fi
      # -o: overwrite without prompting (a partial tree from an earlier run must not hang on a question)
      if ! unzip -qo "$whl" 'nvvfx/libs/*' 'nvidia_vfx.libs/*' -d "$VFX"; then
        rm -rf -- "$VFX"; step_failed wheel "unzip failed: the download is not a valid wheel"; return 1
      fi
      ver=$(unzip -p "$whl" '*.dist-info/METADATA' 2>/dev/null | sed -n 's/^Version: //p' | head -1 | tr -d '\r' || true)
      if [ "$ver" != "$WHEEL_VERSION" ] || [ ! -f "$VFX/nvvfx/libs/libnvVFXVideoSuperRes.so" ]; then
        rm -rf -- "$VFX"; step_failed wheel "wheel has version '${ver:-none}' and no library tree; expected $WHEEL_VERSION"; return 1
      fi
      META[wheel_version]=$ver
    fi
  fi
  NVVSR_LIBDIR_VALUE=$(wheel_libdir)
  if [ "$DRY" = 1 ]; then return 0; fi   # plan output only: the directories exist once the wheel is unpacked
  rec_put tree "$VFX" wheel 1 "" "" "nvvfx/libs"
  META[wheel_installed]=1
  set_comp wheel installed "$WHEEL_NAME ${META[wheel_version]:-$WHEEL_VERSION}, sha256 $WHEEL_SHA256"
  set_path vfx "$VFX"
}

step_libdir_file() {
  if [ -z "$NVVSR_LIBDIR_VALUE" ]; then return 0; fi
  printf '%s\n' "$NVVSR_LIBDIR_VALUE" | gen_file libdir
  install_file "$STATE/gen-libdir" "$NVVSR_CONF_DIR/libdir" 644 core
  set_path libdir_file "$NVVSR_CONF_DIR/libdir"
}

step_mpv_binary() { # sets MPV_OK; builds the private mpv when asked
  local tree=${MPV_BIN%/bin/mpv}
  MPV_OK=0
  META[mpv_vapoursynth]=0
  if mpv_has_vapoursynth "$MPV_BIN"; then MPV_OK=1; META[mpv_vapoursynth]=1; MPV_NOTE="private mpv $MPV_BIN"; return 0; fi
  MPV_NOTE="private mpv missing or without vapoursynth: $MPV_BIN (--build-mpv, or install/build-mpv-vsr.sh)"
  if [ "$BUILD_MPV" = 0 ]; then warn "$MPV_NOTE"; return 0; fi
  if [ "${NVVSR_INSTALL_SKIP_MPV_BUILD:-0}" = 1 ]; then warn "$MPV_NOTE; build skipped (NVVSR_INSTALL_SKIP_MPV_BUILD=1)"; return 0; fi
  if [ "$tree" = "$MPV_BIN" ]; then step_failed mpv "--mpv-bin must end in /bin/mpv to be built"; return 1; fi
  act build-mpv "$tree (mpv 0.41.0 from a pinned tarball, 10+ minutes; install/build-mpv-vsr.sh)"
  if [ "$DRY" = 0 ]; then
    "$BUILD_MPV_SH" --prefix "$tree" --yes || { step_failed mpv "build-mpv-vsr.sh failed"; return 1; }
    mpv_has_vapoursynth "$MPV_BIN" || { step_failed mpv "the new mpv has no vapoursynth filter"; return 1; }
    MPV_OK=1; META[mpv_vapoursynth]=1; META[mpv_built]=1; MPV_NOTE="private mpv built into $tree"
  fi
}

# mpv resolves libvsscript.so through $XDG_CONFIG_HOME/vapoursynth/vapoursynth.toml. VapourSynth's own `config` writes
# only the python-package key, so the entry for /usr/lib/libvsscript.so is appended (exe, libpython and mtime copied from
# that entry; without one, from the system). An existing entry for the key is never changed.
step_vsscript_line() {
  local f=$VS_TOML key line exe="" libpy="" mt="" trio tmp
  key=$(find_vsscript)
  if [ ! -e "$key" ]; then warn "libvsscript.so not found; vapoursynth.toml not touched"; return 0; fi
  check_path_chars "libvsscript path" "$key"
  if [ -f "$f" ]; then
    if grep -qE "^[[:space:]]*\"$(printf '%s' "$key" | sed 's/[][\.*^$]/\\&/g')\"[[:space:]]*=" "$f"; then return 0; fi
    trio=$(sed -n 's/^"[^"]*"[[:space:]]*=[[:space:]]*\["\([^"]\+\)","\([^"]\+\)","\([0-9]\+\)"\].*$/\1\t\2\t\3/p' "$f" | head -1 || true)
    if [ -n "$trio" ]; then IFS=$'\t' read -r exe libpy mt <<<"$trio"; fi
  fi
  if [ -z "$exe" ] || [ ! -f "$libpy" ]; then   # no usable entry to copy: the values come from the system
    exe=$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)
    if [ -n "$exe" ]; then exe=$(readlink -f -- "$exe"); fi
    libpy=$(ls -1 "${NVVSR_INSTALL_PYLIBDIR:-/usr/lib}"/libpython3*.so.1.0 2>/dev/null | sort -V | tail -n 1 || true)
    mt=""
    if [ -n "$libpy" ]; then mt=$(stat -L -c %Y -- "$libpy" 2>/dev/null || true); fi
  fi
  if [ -z "$exe" ] || [ -z "$libpy" ] || [ -z "$mt" ] || [ ! -x "$exe" ] || [ ! -f "$libpy" ]; then
    warn "cannot work out python and libpython for vapoursynth.toml; run \`vapoursynth config\` and re-run the installer"
    return 0
  fi
  check_path_chars python "$exe"; check_path_chars libpython "$libpy"
  line=$(printf '"%s" = ["%s","%s","%s"]' "$key" "$exe" "$libpy" "$mt")
  set_path vapoursynth_toml "$f"
  if [ -f "$f" ]; then
    make_backup "$f"
    act append "$f: $line"
    if [ "$DRY" = 0 ]; then
      tmp=$(mktemp "$f.nvvsr-tmp.XXXXXX"); TMPFILES+=("$tmp")
      cp -p -- "$f" "$tmp"
      if [ -n "$(tail -c1 -- "$f")" ]; then printf '\n' >>"$tmp"; fi   # the file did not end in a newline
      printf '%s\n' "$line" >>"$tmp"
      mv -f -- "$tmp" "$f"
      rec_put line "$f" mpv 0 "$BK" "$(sha_of "$f")" "$line"
    fi
  else
    ensure_dir "$(dirname "$f")" mpv
    act create "$f: $line"
    if [ "$DRY" = 0 ]; then
      printf '%s\n' "$line" >"$STATE/gen-toml"
      write_atomic "$STATE/gen-toml" "$f" 644
      rec_put line "$f" mpv 1 "" "$(sha_of "$f")" "$line"
    fi
  fi
}

step_mpv() {
  local q_mpv q_script
  step_mpv_binary || return 1
  ensure_dir "$MPV_DIR/script-opts" mpv
  install_file "$REPO/mpv/vsr.lua" "$MPV_DIR/vsr.lua" 644 mpv &&
  install_file "$REPO/mpv/vsr.vpy" "$MPV_DIR/vsr.vpy" 644 mpv &&
  install_file "$REPO/mpv/script-opts/vsr.conf" "$MPV_DIR/script-opts/vsr.conf" 644 mpv &&
  install_file "$REPO/mpv/script-opts/vsr.conf" "$MPV_CONF_DIR/script-opts/vsr.conf" 644 mpv create-only ||
    { step_failed mpv "cannot install the mpv files"; return 1; }
  q_mpv=$(printf '%q' "$MPV_BIN"); q_script=$(printf '%q' "$MPV_DIR/vsr.lua")
  gen_file mpv-sr <<EOF
#!/usr/bin/env bash
# mpv-sr - mpv with NVIDIA RTX video super resolution. Written by the NVVSR installer (install/install.sh);
# re-run the installer instead of editing. It does not replace or shadow the system mpv.
MPV=$q_mpv
if [ ! -x "\$MPV" ]; then
  echo "mpv-sr: private mpv not found at \$MPV (build it with install/build-mpv-vsr.sh)" >&2
  exit 127
fi
exec "\$MPV" --script=$q_script "\$@"
EOF
  install_file "$STATE/gen-mpv-sr" "$BIN_DIR/mpv-sr" 755 mpv || { step_failed mpv "cannot install mpv-sr"; return 1; }
  set_path mpv_dir "$MPV_DIR"; set_path mpv_script "$MPV_DIR/vsr.lua"; set_path mpv_wrapper "$BIN_DIR/mpv-sr"
  set_path vsr_conf "$MPV_CONF_DIR/script-opts/vsr.conf"; set_path private_mpv "$MPV_BIN"
  META[mpv_path]=$MPV_BIN
  step_vsscript_line
  if [ "$MPV_OK" = 1 ]; then set_comp mpv installed "$MPV_NOTE"; else set_comp mpv partial "$MPV_NOTE"; fi
}

# VLC 3.0.24 scans only its system plugin directory and $VLC_PLUGIN_PATH (verified: a module in
# ~/.local/share/vlc/plugins is NOT found without it), so vlc-sr sets the variable and starts the system vlc.
step_vlc() {
  local plugin=$VLC_PLUGIN_DIR/video_output/libnvvsr_plugin.so q_vlc q_dir
  install_file "$LIB_DIR/libnvvsr_plugin.so" "$plugin" 755 vlc || { step_failed vlc "cannot install the VLC module"; return 1; }
  q_vlc=$(printf '%q' "$(command -v vlc 2>/dev/null || echo vlc)"); q_dir=$(printf '%q' "$VLC_PLUGIN_DIR")
  gen_file vlc-sr <<EOF
#!/usr/bin/env bash
# vlc-sr - VLC 3 with the NVIDIA RTX video super resolution module. Written by the NVVSR installer.
# VLC 3.0 has no per-user plugin directory, so the module directory is added through VLC_PLUGIN_PATH.
# It does not replace or shadow the system vlc.
export VLC_PLUGIN_PATH=$q_dir\${VLC_PLUGIN_PATH:+:\$VLC_PLUGIN_PATH}
exec $q_vlc "\$@"
EOF
  install_file "$STATE/gen-vlc-sr" "$BIN_DIR/vlc-sr" 755 vlc || { step_failed vlc "cannot install vlc-sr"; return 1; }
  set_path vlc_plugin "$plugin"; set_path vlc_plugin_dir "$VLC_PLUGIN_DIR"; set_path vlc_wrapper "$BIN_DIR/vlc-sr"
  set_comp vlc installed "module in $VLC_PLUGIN_DIR/video_output, start VLC with vlc-sr"
}

# nvcontrol (panel/, C++) goes to <prefix>/bin; panel/install-launcher.sh adds its desktop entry to the home and runs
# from the installed binary. NVVSR_PANEL_BIN names an existing executable to install instead of building (tests).
PANEL_BUILD=$REPO/build/panel
PANEL_LAUNCHER=$REPO/panel/install-launcher.sh
step_panel() {
  local src target=$PREFIX/bin/nvcontrol log=$STATE/panel-build.log
  if [ ! -f "$PANEL_LAUNCHER" ]; then say "  panel: $PANEL_LAUNCHER not present, skipped"; return 0; fi
  if [ -n "${NVVSR_PANEL_BIN:-}" ] && [ -x "$NVVSR_PANEL_BIN" ]; then
    src=$NVVSR_PANEL_BIN
  else
    src=$PANEL_BUILD/nvcontrol
    act build "panel ($PANEL_BUILD)"
    if [ "$DRY" = 0 ] && ! { cmake -S "$REPO/panel" -B "$PANEL_BUILD" -DCMAKE_BUILD_TYPE=Release &&
                              cmake --build "$PANEL_BUILD" -j "$JOBS"; } >"$log" 2>&1; then
      tail -n 25 "$log" >&2
      step_failed panel "build failed (last lines above)"; return 1
    fi
  fi
  install_file "$src" "$target" 755 panel || { step_failed panel "cannot install $target"; return 1; }
  set_path panel "$target"
  act launch "$PANEL_LAUNCHER --bin $target --home $HOME_DIR"
  if [ "$DRY" = 0 ]; then
    if bash "$PANEL_LAUNCHER" --bin "$target" --home "$HOME_DIR"; then set_comp panel installed "nvcontrol $target"
    else step_failed panel "launcher failed: $PANEL_LAUNCHER"; return 1; fi
  fi
}

# nvvsr_webd (webgpu/daemon/, the Chrome upscaling daemon) goes to <prefix>/bin, its systemd user unit to the config home.
# NVVSR_WEBD_BIN names an existing executable to install instead of building (tests).
WEBD_BUILD=$REPO/build/webd
WEBD_UNIT_NAME=nvvsr-webd.service WEBD_SOCKET_NAME=nvvsr-webd.socket
step_webd() {
  local src target=$PREFIX/bin/nvvsr_webd unit=$CONFIG_HOME/systemd/user/nvvsr-webd.service log=$STATE/webd-build.log
  local sock=$CONFIG_HOME/systemd/user/nvvsr-webd.socket enabled=true
  if [ -n "${NVVSR_WEBD_BIN:-}" ] && [ -x "$NVVSR_WEBD_BIN" ]; then
    src=$NVVSR_WEBD_BIN
  else
    src=$WEBD_BUILD/nvvsr_webd
    act build "webd ($WEBD_BUILD)"
    if [ "$DRY" = 0 ] && ! { cmake -S "$REPO/webgpu/daemon" -B "$WEBD_BUILD" -DCMAKE_BUILD_TYPE=Release -DNVVSR_CORE_BUILD_DIR="$LIB_DIR" &&
                              cmake --build "$WEBD_BUILD" -j "$JOBS"; } >"$log" 2>&1; then
      tail -n 25 "$log" >&2
      step_failed webd "build failed (last lines above)"; return 1
    fi
  fi
  install_file "$src" "$target" 755 webd || { step_failed webd "cannot install $target"; return 1; }
  sed "s|%h/.local/share/nvvsr/bin/nvvsr_webd|$target|" "$REPO/webgpu/daemon/nvvsr-webd.service" >"$STATE/gen-webd-unit"
  install_file "$STATE/gen-webd-unit" "$unit" 644 webd || { step_failed webd "cannot install $unit"; return 1; }
  install_file "$REPO/webgpu/daemon/nvvsr-webd.socket" "$sock" 644 webd || { step_failed webd "cannot install $sock"; return 1; }
  set_path webd "$target"
  set_path webd_unit "$unit"
  set_path webd_socket "$sock"
  # an old install enabled the service, which holds port 41777 all session: disable it so the socket can bind
  act systemctl "--user daemon-reload"
  act systemctl "--user disable --now $WEBD_UNIT_NAME"
  act systemctl "--user enable --now $WEBD_SOCKET_NAME"
  if [ "$DRY" = 0 ]; then
    if ! { have systemctl && systemctl --user daemon-reload && { systemctl --user disable --now "$WEBD_UNIT_NAME" >/dev/null 2>&1 || true; } &&
           systemctl --user enable --now "$WEBD_SOCKET_NAME"; } >/dev/null 2>&1; then
      enabled=false
      warn "webd: the socket was not enabled (no systemd user manager?). Run these commands:"
      say "  systemctl --user daemon-reload"
      say "  systemctl --user disable --now $WEBD_UNIT_NAME"
      say "  systemctl --user enable --now $WEBD_SOCKET_NAME"
    fi
    set_comp webd installed "nvvsr_webd $target; unit $unit; socket $sock; service_enabled=$enabled"
  fi
}

init_meta() {
  META[version]=$(core_version)
  META[git_commit]=$(GIT_OPTIONAL_LOCKS=0 git -C "$REPO" rev-parse HEAD 2>/dev/null || true)
  META[git_dirty]=0
  if ! GIT_OPTIONAL_LOCKS=0 git -C "$REPO" diff --quiet HEAD -- 2>/dev/null; then META[git_dirty]=1; fi
  META[repo]=$REPO; META[prefix]=$PREFIX; META[home]=$HOME_DIR; META[config_home]=$CONFIG_HOME; META[data_home]=$DATA_HOME
  META[wheel_name]=$WHEEL_NAME; META[wheel_sha256]=$WHEEL_SHA256   # wheel_version: read from the wheel by step_wheel
  set_path install_json "$MANIFEST"
}

run_install_steps() {
  local vs_ok=0 vlc_ok=0
  FAILED=0
  ensure_dir "$PREFIX" core
  ensure_dir "$LIB_DIR" core
  step_core || return 1
  if [ "$WANT_MPV" = 1 ] && step_plugin vapoursynth vapoursynth libvsnvvsr.so -DNVVSR_VS_BUILD_TESTS; then vs_ok=1; fi
  if [ "$WANT_VLC" = 1 ] && step_plugin vlc vlc libnvvsr_plugin.so -DNVVSR_VLC_BUILD_TESTS; then vlc_ok=1; fi
  step_wheel || true
  if [ "$DRY" = 0 ]; then META[nvvsr_libdir]=$NVVSR_LIBDIR_VALUE; fi
  step_libdir_file
  if [ "$WANT_MPV" = 1 ]; then if [ "$vs_ok" = 1 ]; then step_mpv || true; fi; else skip_comp mpv "--no-mpv"; fi
  if [ "$WANT_VLC" = 1 ]; then if [ "$vlc_ok" = 1 ]; then step_vlc || true; fi; else skip_comp vlc "--no-vlc"; fi
  if [ "$WITH_PANEL" = 1 ]; then step_panel || true; else skip_comp panel "not requested"; fi
  if [ "$WITH_CHROME" = 1 ]; then step_webd || true; else skip_comp webd "not requested"; fi
  save_manifest
}

# ------------------------------------------------------------------------------------------------ install command
# The plugins must find libnvvsr.so.0 in the prefix, whatever directory they are copied to.
verify_elf() { # file
  local res
  res=$(ldd "$1" 2>/dev/null | sed -n 's/^[[:space:]]*libnvvsr\.so\.0 => \([^ ]*\).*/\1/p' | head -1 || true)
  if [ "$res" = "$LIB_DIR/libnvvsr.so.0" ]; then return 0; fi
  err "$1 resolves libnvvsr.so.0 to '${res:-nothing}', expected $LIB_DIR/libnvvsr.so.0"
  FAILED=$((FAILED + 1))
}

# <prefix>/bin/nvvsr_probe: loads the NVIDIA libraries and initialises CUDA, creates no context. Touches the GPU.
gpu_probe() {
  local out
  # LD_LIBRARY_PATH: the probe loads the installed libnvvsr.so.0, not the build tree copy in its RUNPATH
  if out=$(LD_LIBRARY_PATH=$LIB_DIR NVVSR_LIBDIR=$NVVSR_LIBDIR_VALUE "$PREFIX/bin/nvvsr_probe" 2>&1); then
    say "GPU probe: super resolution is usable"
  else
    warn "GPU probe failed: ${out:-no answer}"
  fi
}

confirm() { # prompt
  local a=""
  if [ "$YES" = 1 ]; then return 0; fi
  if ! { : </dev/tty; } 2>/dev/null; then
    err "refusing to change $HOME_DIR without a terminal to confirm on; re-run with --yes (or --dry-run to only see the plan)"
    exit 3
  fi
  printf '%s [y/N] ' "$1" >/dev/tty
  read -r a </dev/tty || a=n
  case $a in y|Y|yes|YES) return 0 ;; *) say "Not confirmed. Nothing was changed."; exit 3 ;; esac
}

cmd_install() {
  local c
  resolve_env
  init_state
  WANT_MPV=$((1 - NO_MPV)); WANT_VLC=$((1 - NO_VLC))
  say "NVIDIA video super resolution: install into $PREFIX (home $HOME_DIR)"
  if [ "$SKIP_GPU" = 1 ]; then say "GPU-free mode: no driver query, no GPU probe."; fi
  run_checks >"$STATE/preflight.txt"
  if [ "$HARD_MISSING" -gt 0 ]; then
    grep -E '^  missing' "$STATE/preflight.txt" || true
    print_pkg_hints
    die "required tools are missing (see above); nothing was changed" 2
  fi
  load_manifest "$MANIFEST" || true
  DRY=1; CHANGES=0
  say "Plan:"
  run_install_steps || true
  if [ "$CHANGES" = 0 ]; then say "  nothing to change (all up to date)"; return 0; fi
  if [ "$DRY_RUN" = 1 ]; then say "Dry run: $CHANGES change(s) would be made; nothing was written."; return 0; fi
  say "$CHANGES change(s) in $HOME_DIR, all recorded in $MANIFEST (install.sh uninstall undoes them)."
  confirm "Apply these changes?"
  DRY=0; CHANGES=0; EXEC_STARTED=1
  say "Installing:"
  init_meta
  run_install_steps || FAILED=$((FAILED + 1))
  if [ "$FAILED" = 0 ]; then
    if [ "$WANT_MPV" = 1 ]; then verify_elf "$LIB_DIR/libvsnvvsr.so"; fi
    if [ "$WANT_VLC" = 1 ]; then verify_elf "$LIB_DIR/libnvvsr_plugin.so"; verify_elf "$VLC_PLUGIN_DIR/video_output/libnvvsr_plugin.so"; fi
    if [ "$SKIP_GPU" = 0 ] && [ -n "$NVVSR_LIBDIR_VALUE" ]; then gpu_probe; fi
  fi
  say ""
  if [ "$FAILED" -gt 0 ]; then
    say "Finished with problems (see above). install.json lists what was done; install.sh uninstall removes it."
    return 1
  fi
  say "Done: $CHANGES change(s)."
  for c in "${CORDER[@]:-}"; do if [ -n "$c" ]; then printf '  %-16s %-9s %s\n' "$c" "${CSTAT[$c]}" "${CDET[$c]}"; fi; done
  if [ "$WANT_MPV" = 1 ]; then say "mpv:  mpv-sr video.mkv   (fullscreen with f: super resolution switches on by itself)"; fi
  if [ "$WANT_VLC" = 1 ]; then say "VLC:  vlc-sr video.mkv   (VLC 3.0 needs VLC_PLUGIN_PATH, which vlc-sr sets)"; fi
  case ":$PATH:" in *":$BIN_DIR:"*) ;; *) say "Add $BIN_DIR to your PATH to use mpv-sr / vlc-sr by name." ;; esac
}

# ------------------------------------------------------------------------------------------------ status
cmd_status() {
  local c k p bad=0 ok=0 key
  resolve_env
  init_state
  if [ ! -f "$MANIFEST" ]; then say "Not installed: no $MANIFEST"; return 1; fi
  if ! "$MANIFEST_SH" validate "$MANIFEST"; then err "$MANIFEST is not a valid install.json"; return 2; fi
  load_manifest "$MANIFEST"
  say "Installed: version ${META[version]:-?}, commit ${META[git_commit]:-unknown}, prefix ${META[prefix]:-$PREFIX}"
  say "Libdir:    ${META[nvvsr_libdir]:-none}"
  for c in "${CORDER[@]:-}"; do if [ -n "$c" ]; then printf '  %-16s %-9s %s\n' "$c" "${CSTAT[$c]}" "${CDET[$c]}"; fi; done
  for k in "${RKEYS[@]:-}"; do
    if [ -z "$k" ]; then continue; fi
    p=${k#*"$KS"}
    case ${RTYPE[$k]} in
      file) if [ ! -f "$p" ]; then printf '  MISSING  %s\n' "$p"; bad=$((bad + 1))
            elif [ "$(sha_of "$p")" != "${RSHA[$k]}" ]; then printf '  CHANGED  %s (modified since install)\n' "$p"
            else ok=$((ok + 1)); fi ;;
      symlink) if [ -L "$p" ]; then ok=$((ok + 1)); else printf '  MISSING  %s\n' "$p"; bad=$((bad + 1)); fi ;;
      dir|tree) if [ -d "$p" ]; then ok=$((ok + 1)); else printf '  MISSING  %s\n' "$p"; bad=$((bad + 1)); fi ;;
      line) if [ -f "$p" ] && grep -qxF -- "${REXTRA[$k]}" "$p"; then ok=$((ok + 1)); else printf '  MISSING  line in %s\n' "$p"; bad=$((bad + 1)); fi ;;
    esac
  done
  say "  $ok recorded item(s) intact"
  key=$(find_vsscript)
  if [ "${CSTAT[mpv]:-skipped}" != skipped ] && ! grep -qF -- "\"$key\"" "$VS_TOML" 2>/dev/null; then
    printf '  MISSING  %s entry in %s (mpv cannot start VapourSynth)\n' "$key" "$VS_TOML"; bad=$((bad + 1))
  fi
  if [ "$bad" -gt 0 ]; then say "$bad problem(s) found."; return 2; fi
  say "Everything is intact."
}

# ------------------------------------------------------------------------------------------------ uninstall
KEPT=()
under() { case $2 in "$1"/*) return 0 ;; *) return 1 ;; esac; }
keep() { KEPT+=("$1"); printf '  keep     %s\n' "$1"; }

restore_or_remove() { # path backup : put the original back byte for byte, else delete our file
  if [ -n "$2" ] && { [ -e "$2" ] || [ -L "$2" ]; }; then
    act restore "$1 <- $2"
    if [ "$DRY" = 0 ]; then mv -f -- "$2" "$1"; fi
  else
    act remove "$1"
    if [ "$DRY" = 0 ]; then rm -f -- "$1"; fi
  fi
}

un_entry() { # key : files and symlinks we wrote are restored/removed unless the user changed them since
  local k=$1 p=${1#*"$KS"} cur=""
  case ${RTYPE[$k]} in
    file)
      if [ -f "$p" ] && [ ! -L "$p" ]; then cur=$(sha_of "$p"); fi
      if [ -n "$cur" ] && [ "$cur" != "${RSHA[$k]}" ]; then
        keep "$p (you changed it after the install${RBAK[$k]:+; your original is at ${RBAK[$k]}})"; return 0
      fi
      if [ -e "$p" ] || [ -L "$p" ] || [ -n "${RBAK[$k]}" ]; then restore_or_remove "$p" "${RBAK[$k]}"; fi ;;
    symlink)
      if [ -L "$p" ] && [ "$(readlink -- "$p")" = "${REXTRA[$k]}" ]; then restore_or_remove "$p" "${RBAK[$k]}"
      elif [ -e "$p" ] || [ -L "$p" ]; then keep "$p (no longer the symlink we made)"; fi ;;
    line)
      [ -f "$p" ] || return 0
      if [ "$(sha_of "$p")" = "${RSHA[$k]}" ]; then restore_or_remove "$p" "${RBAK[$k]}"; return 0; fi
      act unline "$p: ${REXTRA[$k]}"
      if [ "$DRY" = 0 ]; then
        "$MANIFEST_SH" remove-line "$p" "${REXTRA[$k]}" >/dev/null
        if [ "${RCRE[$k]}" = 1 ] && [ -z "$(tr -d '[:space:]' <"$p")" ]; then rm -f -- "$p"; fi
      fi ;;
    tree)
      [ -e "$p" ] || return 0
      if ! under "$PREFIX" "$p" || [[ $p == *..* ]]; then keep "$p (outside the prefix)"; return 0; fi
      if [ -n "${REXTRA[$k]}" ] && [ ! -e "$p/${REXTRA[$k]}" ]; then keep "$p (no ${REXTRA[$k]}: not what we created)"; return 0; fi
      act rmtree "$p"
      if [ "$DRY" = 0 ]; then rm -rf -- "$p"; fi ;;
  esac
}

un_dir() {
  local p=${1#*"$KS"}
  [ -d "$p" ] || return 0
  if [ "$DRY" = 1 ]; then act rmdir "$p (if empty)"; return 0; fi
  # VLC leaves its own plugin cache in a plugin directory; VLC rebuilds it
  if [ "$(basename -- "$p")" = plugins ] && [ -f "$p/plugins.dat" ] && [ -z "$(find "$p" -mindepth 1 ! -name plugins.dat -print -quit)" ]; then rm -f -- "$p/plugins.dat"; fi
  if rmdir -- "$p" 2>/dev/null; then printf '  rmdir    %s\n' "$p"; else keep "$p (not empty)"; fi
}

run_uninstall() {
  local i k mp tree
  KEPT=()
  if [ -n "${CSTAT[panel]:-}" ] && [ "${CSTAT[panel]}" != skipped ]; then   # the launcher is not in install.json: it removes itself
    act launch "$PANEL_LAUNCHER --uninstall --home $HOME_DIR"
    if [ "$DRY" = 0 ]; then bash "$PANEL_LAUNCHER" --uninstall --home "$HOME_DIR" || warn "panel launcher could not be removed"; fi
  fi
  if [ -n "${CSTAT[webd]:-}" ] && [ "${CSTAT[webd]}" != skipped ]; then   # the unit is removed with the manifest; stop it first
    act systemctl "--user disable --now $WEBD_SOCKET_NAME $WEBD_UNIT_NAME"
    act systemctl "--user daemon-reload"
    if [ "$DRY" = 0 ] && have systemctl; then
      systemctl --user disable --now "$WEBD_SOCKET_NAME" "$WEBD_UNIT_NAME" >/dev/null 2>&1 || true
      systemctl --user daemon-reload >/dev/null 2>&1 || true
    fi
  fi
  for ((i = ${#RKEYS[@]} - 1; i >= 0; i--)); do
    k=${RKEYS[$i]}
    if [ "${RTYPE[$k]}" != dir ]; then un_entry "$k"; fi
  done
  act remove "$MANIFEST"   # before the directories, so the prefix itself can go when we created it
  if [ "$DRY" = 0 ]; then rm -f -- "$MANIFEST"; fi
  for ((i = ${#RKEYS[@]} - 1; i >= 0; i--)); do
    k=${RKEYS[$i]}
    if [ "${RTYPE[$k]}" = dir ]; then un_dir "$k"; fi
  done
  if [ "$PURGE_MPV" = 1 ]; then
    mp=${META[mpv_path]:-$MPV_BIN}; tree=${mp%/bin/mpv}
    if [ "$tree" != "$mp" ] && [ "$(basename -- "$tree")" = mpv-vsr ] && [ -x "$mp" ]; then
      act rmtree "$tree (private mpv, --purge-mpv)"
      if [ "$DRY" = 0 ]; then rm -rf -- "$tree"; fi
    else warn "--purge-mpv: $mp is not a .../mpv-vsr/bin/mpv; not removed"; fi
  fi
}

cmd_uninstall() {
  local k p mhome
  resolve_env
  init_state
  if [ ! -f "$MANIFEST" ]; then say "Nothing to uninstall: no $MANIFEST"; return 0; fi
  load_manifest "$MANIFEST" || die "cannot read $MANIFEST"
  mhome=${META[home]:-}
  for k in "${RKEYS[@]:-}"; do   # a manifest may only remove things below the home it was written for
    if [ -z "$k" ]; then continue; fi
    p=${k#*"$KS"}
    if [ -n "$mhome" ] && ! under "$mhome" "$p"; then die "$MANIFEST lists $p, outside its home $mhome; refusing"; fi
    if [[ $p == *..* ]]; then die "$MANIFEST lists a path with '..': $p"; fi
  done
  say "Uninstall from $PREFIX (home $HOME_DIR)"
  DRY=1; CHANGES=0
  say "Plan:"
  run_uninstall
  if [ "$DRY_RUN" = 1 ]; then say "Dry run: nothing was changed."; return 0; fi
  confirm "Remove these items?"
  DRY=0; CHANGES=0
  say "Removing:"
  run_uninstall
  if [ "${#KEPT[@]}" -gt 0 ]; then say "Kept (yours, or not empty): ${#KEPT[@]} item(s), listed above."; fi
  say "Uninstalled. The private mpv ($MPV_BIN) and your own configuration were left alone."
}

main() {
  parse_args "$@"
  case $CMD in
    check) cmd_check ;;
    install) cmd_install ;;
    uninstall) cmd_uninstall ;;
    status) cmd_status ;;
  esac
}
main "$@"
