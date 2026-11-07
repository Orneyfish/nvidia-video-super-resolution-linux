#!/bin/bash
# Checks webgpu/extension/content.js end to end: real Chrome (scratch profile, native Wayland) inside a HEADLESS cage, driven by
# ext_drive.cpp, on a generated 640x360 clip served by server.cpp. The extension starts by itself (#nvvsr-force) and pretends the
# video box is 1280x720. Nothing appears on the desktop; GPU work only in "ok" mode.
#   webgpu/test/ext_check.sh ok     daemon with the real libraries: "[nvvsr] on", a canvas of 1280x720, frames received, no "failed"
#   webgpu/test/ext_check.sh fail   daemon with NVVSR_LIBDIR=/nonexistent, so the first frame gets "ERR create": the extension must
#                                   log "failed: ERR create", show the "SR failed" badge and remove its canvas (plain video)
# env: NVVSR_SCRATCH (default /tmp/nvvsr-webgpu), NVVSR_WEBD (daemon binary), NVVSR_LIBDIR (ok mode; default tools/libdir.sh),
#      EXT_DIR (extension to test; default webgpu/extension), CHROME, RENDER_NODE (default the Intel /dev/dri/renderD128).
# Everything runs in a private network namespace: the real port 41777 may be held by the user's nvvsr-webd.socket, and the bridge
# URL is fixed. server.cpp starts nvidia-smi (wakes the dGPU), so a fake nvidia-smi comes first in PATH.
set -eu
MODE=${1:?usage: ext_check.sh ok|fail}; [ "$MODE" = ok ] || [ "$MODE" = fail ] || { echo "usage: ext_check.sh ok|fail" >&2; exit 2; }
if [ -z "${EXT_CHECK_NS:-}" ]; then  # root in a fresh user+net namespace brings lo up, then back to our uid (Chrome refuses root)
  exec unshare -Urn env EXT_CHECK_NS=1 sh -c 'ip link set lo up && u=$1 g=$2 && shift 2 && exec unshare -U --map-user="$u" --map-group="$g" "$@"' \
    sh "$(id -u)" "$(id -g)" "$0" "$@"
fi
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$HERE")
SCRATCH=${NVVSR_SCRATCH:-/tmp/nvvsr-webgpu}; WEBD=${NVVSR_WEBD:-$ROOT/daemon/build/nvvsr_webd}; EXT=${EXT_DIR:-$ROOT/extension}
CHROME=${CHROME:-$(command -v google-chrome-stable)}
mkdir -p "$SCRATCH"
run=$(mktemp -d "$SCRATCH/ext-XXXXXX"); rt=$(mktemp -d /dev/shm/nvvsr-ext-XXXXXX); chmod 700 "$rt"
mkdir -p "$run/media" "$run/out" "$run/bin" "$run/home"
printf '#!/bin/sh\nexit 0\n' > "$run/bin/nvidia-smi"; chmod +x "$run/bin/nvidia-smi"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill -- "-$p" 2> /dev/null || kill "$p" 2> /dev/null || true; done; rm -rf "$rt"; }
trap cleanup EXIT

g++ -O2 -std=c++17 -pthread "$HERE/server.cpp" -o "$run/server"
g++ -O2 -std=c++17 "$HERE/ext_drive.cpp" -o "$run/ext_drive"
ffmpeg -v error -y -nostdin -f lavfi -i testsrc2=size=640x360:rate=30 -t 30 -c:v libx264 -preset veryfast -pix_fmt yuv420p -movflags +faststart "$run/media/clip.mp4"
echo '<!doctype html><meta charset="utf-8"><video src="clip.mp4" autoplay muted loop style="width:640px;height:360px"></video>' > "$run/media/page.html"

if [ "$MODE" = ok ]; then LIBDIR=${NVVSR_LIBDIR:-$("$ROOT/../tools/libdir.sh")}; else LIBDIR=/nonexistent; fi
NVVSR_LIBDIR=$LIBDIR HOME="$run/home" "$WEBD" 41777 > "$run/webd.log" 2>&1 & PIDS+=($!)
PATH="$run/bin:$PATH" "$run/server" --webgpu "$ROOT" --media "$run/media" --out "$run/out" --wayland "$rt/wayland-0" > "$run/server.out" 2> /dev/null & PIDS+=($!)
for _ in $(seq 100); do grep -q PORTS "$run/server.out" && break; sleep 0.1; done
read -r _ PA _ < "$run/server.out"

# page-side wait: ok = canvas drawn for 5 s; fail = "SR failed" badge seen (it lives 1.5 s)
if [ "$MODE" = ok ]; then
  probe="new Promise(r => { const t0 = Date.now(); let c0 = 0; const iv = setInterval(() => { const c = document.querySelector('canvas'); if (c && !c0) c0 = Date.now();
    if (c0 && Date.now() - c0 > 5000 || Date.now() - t0 > 30000) { clearInterval(iv); r(JSON.stringify({ canvas: !!c, size: c && [c.width, c.height] })); } }, 100); })"
else
  probe="new Promise(r => { const t0 = Date.now(); const iv = setInterval(() => { const b = [...document.querySelectorAll('div')].some(d => d.textContent === 'SR failed');
    if (b || Date.now() - t0 > 30000) { clearInterval(iv); r(JSON.stringify({ badge: b, canvas: !!document.querySelector('canvas') })); } }, 50); })"
fi
env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u HYPRLAND_INSTANCE_SIGNATURE -u XDG_SESSION_TYPE -u XDG_CURRENT_DESKTOP -u SWAYSOCK \
  HOME="$run/home" XDG_CONFIG_HOME="$run/home/.config" XDG_CACHE_HOME="$run/home/.cache" XDG_DATA_HOME="$run/home/.local/share" XDG_RUNTIME_DIR="$rt" \
  WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE="${RENDER_NODE:-/dev/dri/renderD128}" WLR_LIBINPUT_NO_DEVICES=1 \
  EXT_DRIVE_NOKEY=1 EXT_DRIVE_EVAL="$probe" \
  timeout 150 setsid cage -- "$run/ext_drive" "$CHROME" "$EXT" "http://127.0.0.1:$PA/media/page.html#nvvsr-debug&nvvsr-force&nvvsr-box=1280x720" "$run/out" 2 5 \
    --user-data-dir="$run/profile" --password-store=basic --ozone-platform=wayland --autoplay-policy=no-user-gesture-required --ignore-gpu-blocklist \
  > "$run/drive.out" 2> "$run/cage.err" || true

bad=0
ck() { if grep -q -e "$2" "$run/drive.out"; then echo "ok   $1"; else echo "FAIL $1"; bad=1; fi; }
nck() { if grep -q -e "$2" "$run/drive.out"; then echo "FAIL $1"; bad=1; else echo "ok   $1"; fi; }
ck "extension loaded" 'loadUnpacked: .*"id"'
if [ "$MODE" = ok ]; then
  ck "started" '"value":"on"'
  nck "no failure" '"value":"failed:"'
  ck "canvas 1280x720 drawn" 'probe: .*canvas\\":true,\\"size\\":\[1280,720\]'
  ck "frames received" 'sent=[0-9]* recv=[1-9]'
  ck "canvas still there at the end" 'fullscreen: .*"canvas":\[1280,720'
else
  ck "failed on ERR create" 'failed:.*"value":"ERR create'
  ck "SR failed badge, no canvas" 'probe: .*badge\\":true,\\"canvas\\":false'
  ck "no canvas at the end" 'fullscreen: .*"canvas":null'
fi
[ "$bad" = 0 ] && { echo "PASS ext_check $MODE"; rm -rf "$run"; exit 0; }
echo "FAIL ext_check $MODE; output kept in $run (drive.out, webd.log, cage.err)" >&2; exit 1
