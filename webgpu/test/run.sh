#!/bin/bash
# Headless test run of the WebGPU upscaler: Chrome (throwaway profile and HOME, native Wayland) inside a HEADLESS cage compositor on
# the Intel render node, exactly like tools/spikes/browser-sr/capture.sh: WAYLAND_DISPLAY/DISPLAY/DBUS are unset, nothing appears
# on the desktop. server.cpp serves the pages and clips and collects results; harness.html runs the suites and posts them.
#
#   webgpu/test/run.sh [--config intel|nvidia] [--scratch DIR] [--seconds N] [--only STR] [--timeout S] SUITE...
#   suites: probe selfcheck cost pipeline quality compat present   (default: all)
#   --config intel   (default) plain Chrome; WebGPU runs on the iGPU Chrome composites on (powerPreference low-power)
#   --config nvidia  Chrome's whole GPU stack on the RTX (only the NVIDIA Vulkan ICD visible + ANGLE-Vulkan); the only way found to make
#                    an NVIDIA WebGPU adapter exchange frames with Chrome on this hybrid laptop (see README)
#   --only STR       pipeline suite: only clips whose name contains STR
# Results: $SCRATCH/out/*-<config>.{json,md} are copied to webgpu/test/results/. The GPU must be idle (no compute apps, no Ollama model).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$HERE")
CONFIG=intel; SCRATCH=${NVVSR_SCRATCH:-/tmp/nvvsr-webgpu}; SECS=15; ONLY=""; TIMEOUT=1800
while [ $# -gt 0 ]; do
  case $1 in
    --config) CONFIG=$2; shift 2;; --scratch) SCRATCH=$2; shift 2;; --seconds) SECS=$2; shift 2;; --only) ONLY=$2; shift 2;; --timeout) TIMEOUT=$2; shift 2;;
    -*) echo "unknown option $1" >&2; exit 2;; *) break;;
  esac
done
SUITES="${*:-all}"; [ "$SUITES" = all ] && SUITES="probe selfcheck cost pipeline quality compat present"
case $CONFIG in intel) GPU=low-power;; nvidia) GPU=high-performance;; *) echo "--config intel|nvidia" >&2; exit 2;; esac
MEDIA=$SCRATCH/media OUT=$SCRATCH/out
mkdir -p "$SCRATCH/home" "$OUT" "$ROOT/test/results"
[ -d "$MEDIA" ] || "$HERE/make-media.sh" "$MEDIA"

[ -z "$(nvidia-smi --query-compute-apps=pid --format=csv,noheader)" ] || { echo "GPU compute apps are running: refusing to measure" >&2; exit 1; }
if pgrep -x ollama > /dev/null && [ "$(ollama ps | tail -n +2 | wc -l)" -gt 0 ]; then echo "an Ollama model is loaded: refusing to measure" >&2; exit 1; fi

g++ -O2 -std=c++17 -pthread "$HERE/server.cpp" -o "$SCRATCH/server"
run=$(mktemp -d "$SCRATCH/run-XXXXXX"); rt=$(mktemp -d /dev/shm/nvvsr-wg-XXXXXX); chmod 700 "$rt"
rm -f "$OUT/done" "$OUT/log.txt"; case " $SUITES " in *" quality "*) rm -f "$OUT"/q_*.png;; esac
SRV=0; CAGE=0
cleanup() { [ "$CAGE" -gt 0 ] && kill -- "-$CAGE" 2> /dev/null || true; [ "$SRV" -gt 0 ] && kill "$SRV" 2> /dev/null || true; rm -rf "$rt"; }
trap cleanup EXIT

"$SCRATCH/server" --webgpu "$ROOT" --media "$MEDIA" --out "$OUT" --wayland "$rt/wayland-0" > "$run/server.out" 2> "$run/server.err" &
SRV=$!
for _ in $(seq 100); do grep -q PORTS "$run/server.out" && break; sleep 0.1; done
read -r _ PA PB < "$run/server.out"

flags=(--user-data-dir="$run/profile" --no-first-run --no-default-browser-check --password-store=basic --ozone-platform=wayland
       --autoplay-policy=no-user-gesture-required --ignore-gpu-blocklist --force-device-scale-factor=2 --enable-webgpu-developer-features
       --host-resolver-rules="MAP nvvsr.test 127.0.0.1")
envx=(env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u HYPRLAND_INSTANCE_SIGNATURE -u XDG_SESSION_TYPE -u XDG_CURRENT_DESKTOP -u SWAYSOCK
      HOME="$SCRATCH/home" XDG_CONFIG_HOME="$SCRATCH/home/.config" XDG_CACHE_HOME="$SCRATCH/home/.cache" XDG_DATA_HOME="$SCRATCH/home/.local/share"
      XDG_RUNTIME_DIR="$rt" WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_RENDER_DRM_DEVICE="${RENDER_NODE:-/dev/dri/renderD128}" WLR_LIBINPUT_NO_DEVICES=1)
if [ "$CONFIG" = nvidia ]; then
  envx+=(VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json)
  flags+=(--use-angle=vulkan --use-vulkan=native --enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE)
fi
URL="http://127.0.0.1:$PA/test/harness.html?suites=${SUITES// /,}&gpu=$GPU&config=$CONFIG&seconds=$SECS&b=$PB&only=$ONLY"
echo "[run] $CONFIG, suites: $SUITES  (servers $PA / $PB, scratch $SCRATCH)" >&2
"${envx[@]}" setsid cage -- google-chrome-stable "${flags[@]}" --app="$URL" > "$run/chrome.log" 2> "$run/chrome.err" &
CAGE=$!
curl -s "http://127.0.0.1:$PA/root?pid=$CAGE" > /dev/null

seen=0; t0=$(date +%s)
until [ -f "$OUT/done" ]; do
  sleep 2
  [ -f "$OUT/log.txt" ] && { tail -n +$((seen + 1)) "$OUT/log.txt" | sed 's/^/[page] /' >&2; seen=$(wc -l < "$OUT/log.txt"); }
  [ $(($(date +%s) - t0)) -lt "$TIMEOUT" ] || { echo "[run] timeout after ${TIMEOUT}s; see $run" >&2; exit 1; }
  kill -0 "$CAGE" 2> /dev/null || { echo "[run] Chrome/cage exited early; see $run/chrome.err" >&2; exit 1; }
done
[ -f "$OUT/log.txt" ] && tail -n +$((seen + 1)) "$OUT/log.txt" | sed 's/^/[page] /' >&2

case " $SUITES " in *" quality "*) "$HERE/metrics.sh" "$OUT" "$MEDIA" > "$OUT/quality-$CONFIG.md"; cp "$OUT/quality.tsv" "$ROOT/test/results/quality-$CONFIG.tsv";; esac
cp "$OUT"/*-"$CONFIG".json "$OUT"/*-"$CONFIG".md "$ROOT/test/results/" 2> /dev/null || true
for s in $SUITES; do echo; echo "== $s ($CONFIG)"; cat "$OUT/$s-$CONFIG.md" 2> /dev/null || echo "(no result)"; done
if grep -q FAILED "$OUT/log.txt" 2> /dev/null; then grep FAILED "$OUT/log.txt" >&2; exit 1; fi
rm -rf "$run"
