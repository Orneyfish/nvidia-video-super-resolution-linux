#!/bin/bash
# Runs a command inside a HEADLESS cage compositor so a GTK window can never appear on the real desktop.
#   usage: run_cage.sh [--home DIR] -- COMMAND [ARG...]
# The command gets a scratch HOME (or DIR with --home; DIR is kept, scratch is removed) and scratch XDG dirs, and
# none of the real session's display, bus or Hyprland variables. Exits with the command's status (2: usage, 3: cage
# did not run the command).
set -u

home=""
while [ $# -gt 0 ]; do
  case $1 in
    --home) [ $# -ge 2 ] || { echo "run_cage.sh: --home needs a directory" >&2; exit 2; }; home=$2; shift 2 ;;
    --) shift; break ;;
    *) echo "usage: run_cage.sh [--home DIR] -- COMMAND [ARG...]" >&2; exit 2 ;;
  esac
done
[ $# -ge 1 ] || { echo "usage: run_cage.sh [--home DIR] -- COMMAND [ARG...]" >&2; exit 2; }
command -v cage >/dev/null || { echo "run_cage.sh: cage is not installed" >&2; exit 2; }

# XDG_RUNTIME_DIR must be mode 700 and on a tmpfs (the compositor socket lives there)
run=$(mktemp -d /dev/shm/nvc-cage.XXXXXX) || exit 2
scratch=$(mktemp -d "${TMPDIR:-/tmp}/nvc-cage-home.XXXXXX") || { rmdir "$run"; exit 2; }
cleanup() { rm -rf "$run" "$scratch"; }
trap cleanup EXIT
trap 'exit 130' INT TERM

[ -n "$home" ] || home=$scratch/home
mkdir -p "$home/.config" "$home/.local/share" "$home/.local/state" "$home/.cache" || exit 2
# cage does not hand back the client's exit status reliably: the inner shell writes it to a file
rcfile=$scratch/rc

env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u HYPRLAND_INSTANCE_SIGNATURE -u XDG_SESSION_TYPE \
  XDG_RUNTIME_DIR="$run" HOME="$home" XDG_CONFIG_HOME="$home/.config" XDG_DATA_HOME="$home/.local/share" \
  XDG_STATE_HOME="$home/.local/state" XDG_CACHE_HOME="$home/.cache" \
  WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
  GDK_BACKEND=wayland GSK_RENDERER=cairo LIBGL_ALWAYS_SOFTWARE=1 GTK_A11Y=none GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1 \
  cage -- bash -c 'rc=$1; shift; "$@"; echo $? > "$rc"' _ "$rcfile" "$@"
cage_rc=$?

if [ -f "$rcfile" ]; then
  exit "$(cat "$rcfile")"
fi
echo "run_cage.sh: cage did not run the command (cage exit status $cage_rc)" >&2
exit 3
