#!/bin/bash
# Which WebGPU adapter does Chrome get under native Wayland? usage: webgpu_probe.sh [extra chrome flags...]
# Throwaway profile, headless cage; the page puts the adapter info in its title, read via the DevTools port.
HERE=$(cd "$(dirname "$0")" && pwd); WORK=$(mktemp -d); rt=/dev/shm/nvvsr-spike-wg-$$; mkdir -m 700 "$rt"
env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u HYPRLAND_INSTANCE_SIGNATURE -u XDG_SESSION_TYPE -u XDG_CURRENT_DESKTOP \
  XDG_RUNTIME_DIR="$rt" WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE="${RENDER_NODE:-/dev/dri/renderD128}" WLR_LIBINPUT_NO_DEVICES=1 \
  timeout 40 cage -- google-chrome-stable --user-data-dir="$WORK/profile" --no-first-run --no-default-browser-check --password-store=basic \
    --ozone-platform=wayland --remote-debugging-port=9333 "$@" "file://$HERE/webgpu.html" > "$WORK/chrome.log" 2>&1 &
sleep 14
curl -s --max-time 5 http://127.0.0.1:9333/json/list | jq -r '.[] | select(.type=="page") | .title' | sed 's/&quot;/"/g; s/&lt;/</g; s/&gt;/>/g; s/&amp;/\&/g'
wait; rm -rf "$rt" "$WORK"
