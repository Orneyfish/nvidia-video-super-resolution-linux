#!/bin/bash
# What does Chrome hand to the Wayland compositor for a playing video?
# usage: capture.sh CLIP.mp4 OUT.log [seconds] [extra chrome flags...]
# Runs Chrome (throwaway profile, native Wayland) inside a HEADLESS cage compositor on the Intel render node, so no
# window appears on the desktop, with WAYLAND_DEBUG=1. Then count get_subsurface / create_immed (dmabuf buffer sizes).
set -u
CLIP=$(readlink -f "$1"); OUT=$2; SECS=${3:-25}; shift 3 2>/dev/null || shift $#
HERE=$(cd "$(dirname "$0")" && pwd); WORK=$(mktemp -d); rt=/dev/shm/nvvsr-spike-$$; mkdir -m 700 "$rt"
sed "s|CLIP_URL|file://$CLIP|" "$HERE/video.html" > "$WORK/page.html"
env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u HYPRLAND_INSTANCE_SIGNATURE -u XDG_SESSION_TYPE -u XDG_CURRENT_DESKTOP \
  XDG_RUNTIME_DIR="$rt" WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE="${RENDER_NODE:-/dev/dri/renderD128}" WLR_LIBINPUT_NO_DEVICES=1 \
  timeout "$SECS" cage -- env WAYLAND_DEBUG=1 google-chrome-stable --user-data-dir="$WORK/profile" --no-first-run \
    --no-default-browser-check --password-store=basic --ozone-platform=wayland --autoplay-policy=no-user-gesture-required \
    "$@" "file://$WORK/page.html" > "$OUT" 2>&1
echo "subsurfaces: $(grep -c get_subsurface "$OUT")"
echo "dmabuf buffer sizes:"; grep -E 'create_immed' "$OUT" | sed -E 's/.*create_immed\(new id wl_buffer#[0-9]+, ([0-9]+), ([0-9]+).*/\1x\2/' | sort | uniq -c | sort -rn
rm -rf "$rt" "$WORK"
