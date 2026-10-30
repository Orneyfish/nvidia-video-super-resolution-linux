#!/bin/bash
# Which buffer sizes does a player hand to the Wayland compositor when it shows a 640x360 clip fullscreen on a 1280x720
# output? Runs the player inside a HEADLESS cage (nothing appears on the desktop) with WAYLAND_DEBUG=client and prints the
# buffer sizes it created (dmabuf / shm) and the viewport destinations it set.
#   usage: probe.sh LABEL SECONDS COMMAND...      (the command may use $CLIP)
set -u
label=$1 secs=$2; shift 2
work=$(mktemp -d "${TMPDIR:-/tmp}/wlprobe.XXXXXX"); rt=$(mktemp -d /dev/shm/wlprobe-rt.XXXXXX); chmod 700 "$rt"
trap 'rm -rf "$work" "$rt"' EXIT
CLIP=$work/clip.mp4
ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x360:rate=30:duration=$((secs + 4))" -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$CLIP"
mkdir -p "$work/home"
env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u HYPRLAND_INSTANCE_SIGNATURE -u XDG_SESSION_TYPE -u XDG_CURRENT_DESKTOP \
  HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config" XDG_CACHE_HOME="$work/home/.cache" XDG_DATA_HOME="$work/home/.local/share" \
  XDG_RUNTIME_DIR="$rt" WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE=/dev/dri/renderD128 WLR_LIBINPUT_NO_DEVICES=1 \
  CLIP="$CLIP" WAYLAND_DEBUG=client timeout "$secs" cage -- "$@" > "$work/out" 2> "$work/log"
[ -n "${DEBUG:-}" ] && grep -m8 -E "create_immed|create_buffer|wl_buffer#[0-9]+, [0-9]+, [0-9]+|params.*\.add" "$work/log" | cut -c1-170
echo "== $label ($(wc -l < "$work/log") protocol lines)"
echo "  buffers (WxH x count):"
{ grep -oE "create_immed\(new id wl_buffer#[0-9]+, [0-9]+, [0-9]+" "$work/log" | sed -E "s/.*, ([0-9]+), ([0-9]+)\$/\1x\2/";
  grep -oE "create_buffer\(new id wl_buffer#[0-9]+, [0-9]+, [0-9]+, [0-9]+" "$work/log" | sed -E 's/.*, ([0-9]+), ([0-9]+), [0-9]+$/\1x\2/'; } | sort | uniq -c | sort -rn | head -4 | sed 's/^/    /'
echo "  viewport destinations:"; grep -oE "set_destination\([0-9]+, [0-9]+\)" "$work/log" | sort | uniq -c | sort -rn | head -3 | sed 's/^/    /'
echo "  subsurfaces: $(grep -c 'get_subsurface' "$work/log")  commits: $(grep -c 'wl_surface#[0-9]*.commit' "$work/log")"
