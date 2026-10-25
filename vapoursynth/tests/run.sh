#!/usr/bin/env bash
# Tests of the nvvsr VapourSynth plugin and mpv/vsr.vpy. Prints PASS/FAIL per check; the exit status is
# the number of failed checks.
#
#   vapoursynth/tests/run.sh             functional tests (about 1-2 minutes, uses the GPU)
#   vapoursynth/tests/run.sh --bench     plus throughput of pool=1/2/3 (needs an idle machine, see below)
#   vapoursynth/tests/run.sh --mpv       plus an end-to-end run in the private mpv (~/.local/opt/mpv-vsr)
#
# Environment (all optional): NVVSR_LIBDIR (default: the repo's .venv wheel directories), NVVSR_PLUGIN
# (default vapoursynth/build/libvsnvvsr.so, built here when missing), NVVSR_MPV (default
# ~/.local/opt/mpv-vsr/bin/mpv), NVVSR_TEST_TMP (scratch directory, kept; default a temporary one).
# Nothing is written to ~/.config or ~/.local: VapourSynth gets a scratch XDG_CONFIG_HOME.
# The benchmark refuses to run while a compiler is running, an Ollama model is resident in VRAM or any
# other process uses the GPU (the pool smoke runs in section 4 are NOT benchmarks, only deadlock checks).
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
VSR_VPY=$REPO/mpv/vsr.vpy
WANT_BENCH=0
WANT_MPV=0
for a in "$@"; do
  case $a in
    --bench) WANT_BENCH=1 ;;
    --mpv) WANT_MPV=1 ;;
    *) echo "usage: $0 [--bench] [--mpv]" >&2; exit 2 ;;
  esac
done

# ---- environment -------------------------------------------------------------------------------
if [ -z "${NVVSR_LIBDIR:-}" ]; then
  sp=$(ls -d "$REPO"/.venv/lib/python3*/site-packages 2>/dev/null | head -1)
  [ -n "$sp" ] && export NVVSR_LIBDIR="$sp/nvvfx/libs:$sp/nvidia_vfx.libs"
fi
if [ -n "${NVVSR_TEST_TMP:-}" ]; then
  SCR=$NVVSR_TEST_TMP; mkdir -p "$SCR"
else
  SCR=$(mktemp -d); trap 'rm -rf "$SCR"' EXIT
fi
export XDG_CONFIG_HOME=$SCR/xdg
mkdir -p "$XDG_CONFIG_HOME"
vapoursynth config >/dev/null 2>&1   # python key
CFG=$XDG_CONFIG_HOME/vapoursynth/vapoursynth.toml
# mpv looks the entry up under its libvsscript.so path: same values as the python-package key.
sed 's|^"[^"]*"|"/usr/lib/libvsscript.so"|' "$CFG" > "$CFG.libvsscript" && cat "$CFG.libvsscript" >> "$CFG" && rm "$CFG.libvsscript"

if [ ! -f "$REPO/build/core/libnvvsr.so" ]; then
  cmake -S "$REPO/core" -B "$REPO/build/core" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$REPO/build/core" -j >/dev/null
fi
cmake -S "$REPO/vapoursynth" -B "$REPO/vapoursynth/build" -DCMAKE_BUILD_TYPE=Release >/dev/null &&
  cmake --build "$REPO/vapoursynth/build" -j >/dev/null || { echo "FAIL build: cannot build the plugin"; exit 1; }
export NVVSR_PLUGIN=${NVVSR_PLUGIN:-$REPO/vapoursynth/build/libvsnvvsr.so}
REF=$REPO/vapoursynth/build/nvvsr_vs_ref
CLIP=$HERE/clip.vpy
cd "$SCR" || exit 1

PASS=0; FAIL=0
ok()  { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1: $2"; FAIL=$((FAIL + 1)); }
check_eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$2', expected '$3'"; fi; }

# vinfo VAR=value...: "WxH FORMAT" of clip.vpy's output, or "ERROR rc=N: first error line".
vinfo() {
  local out rc
  out=$(env "$@" vspipe "$CLIP" --info - 2>&1); rc=$?
  if [ $rc -ne 0 ]; then echo "ERROR rc=$rc: $(printf '%s\n' "$out" | grep -m1 -E 'Upscale|rror')"; return; fi
  echo "$(printf '%s\n' "$out" | sed -n 's/^Width: //p')x$(printf '%s\n' "$out" | sed -n 's/^Height: //p') $(printf '%s\n' "$out" | sed -n 's/^Format Name: //p')"
}
# expect_error NAME NEEDLE VAR=value...: the script must fail cleanly (rc 1) with NEEDLE in the message.
expect_error() {
  local name=$1 needle=$2; shift 2
  local got; got=$(vinfo "$@")
  case $got in
    "ERROR rc=1: "*"$needle"*) ok "$name" ;;
    *) bad "$name" "got '$got', expected an error containing '$needle'" ;;
  esac
}
UP=(T_UP=1)

echo "== 1. plugin loads, signature, linking"
python -I - "$NVVSR_PLUGIN" <<'EOF' >"$SCR/sig.txt" 2>&1
import sys
import vapoursynth as vs
vs.core.std.LoadPlugin(sys.argv[1])
p = [p for p in vs.core.plugins() if p.namespace == "nvvsr"][0]
print(p.identifier, [(f.name, f.signature) for f in p.functions()])
print(callable(vs.core.nvvsr.Upscale))
EOF
sig=$(cat "$SCR/sig.txt")
case $sig in
  *"com.nvvsr.upscale"*"Upscale"*"clip:vnode;width:int:opt;height:int:opt;quality:data:opt;strength:float:opt;matrix:data:opt;range:int:opt;pool:int:opt;fps:float:opt;format:data:opt;"*True)
    ok "plugin loads; core.nvvsr.Upscale signature is registered" ;;
  *) bad "plugin loads" "$sig" ;;
esac
syms=$(nm -D --defined-only "$NVVSR_PLUGIN" | awk '{print $3}' | tr '\n' ' ')
check_eq "only VapourSynthPluginInit2 is exported" "$syms" "VapourSynthPluginInit2 "
lddout=$(env -u LD_LIBRARY_PATH ldd "$NVVSR_PLUGIN" | grep libnvvsr)
case $lddout in *"/libnvvsr.so"*) ok "libnvvsr.so resolves via RUNPATH without LD_LIBRARY_PATH" ;; *) bad "RUNPATH" "$lddout" ;; esac
rp=$(readelf -d "$NVVSR_PLUGIN" | sed -n 's/.*RUNPATH.*\[\(.*\)\]/\1/p')
case $rp in *: | :* | *::*) bad "RUNPATH has no empty entry" "$rp" ;; *) ok "RUNPATH has no empty (current directory) entry" ;; esac

echo "== 2. output geometry and format"
check_eq "720p -> 2560x1440 RGB24" "$(vinfo "${UP[@]}" T_UP_WIDTH=2560 T_UP_HEIGHT=1440)" "2560x1440 RGB24"
check_eq "width only (1280x720, width=1920) -> 1920x1080" "$(vinfo "${UP[@]}" T_UP_WIDTH=1920)" "1920x1080 RGB24"
check_eq "height only (1280x720, height=1080) -> 1920x1080" "$(vinfo "${UP[@]}" T_UP_HEIGHT=1080)" "1920x1080 RGB24"
check_eq "derived size is rounded to even (854x480, width=1000 -> 1000x562)" "$(vinfo "${UP[@]}" T_W=854 T_H=480 T_UP_WIDTH=1000)" "1000x562 RGB24"
check_eq "no size -> same size as the input (still processed)" "$(vinfo "${UP[@]}")" "1280x720 RGB24"
check_eq "format=yuv420p8 -> YUV420P8" "$(vinfo "${UP[@]}" T_UP_HEIGHT=1440 T_UP_FORMAT=yuv420p8)" "2560x1440 YUV420P8"
check_eq "exactly 4x is accepted (640x360 -> 2560x1440)" "$(vinfo "${UP[@]}" T_W=640 T_H=360 T_UP_WIDTH=2560 T_UP_HEIGHT=1440)" "2560x1440 RGB24"
check_eq "every pool size 1..4 is accepted" "$(for p in 1 2 3 4; do vinfo "${UP[@]}" T_UP_POOL=$p; done | sort -u)" "1280x720 RGB24"

echo "== 2b. unsupported input and bad arguments: clear errors, no crash"
expect_error "10-bit input is rejected"      "unsupported input format YUV420P10" "${UP[@]}" T_SRC_FORMAT=YUV420P10
expect_error "GRAY8 input is rejected"       "unsupported input format Gray8"      "${UP[@]}" T_SRC_FORMAT=GRAY8
expect_error "YUV444P8 input is rejected"    "unsupported input format YUV444P8"   "${UP[@]}" T_SRC_FORMAT=YUV444P8
expect_error "RGB24 input is rejected"       "unsupported input format RGB24"      "${UP[@]}" T_SRC_FORMAT=RGB24
expect_error "variable-size input is rejected" "constant format and size"          "${UP[@]}" T_VARIABLE=1
expect_error "downscaling is rejected"       "downscaling is not supported"        "${UP[@]}" T_UP_WIDTH=640 T_UP_HEIGHT=360
expect_error "more than 4x is rejected"      "at most 4x"                          "${UP[@]}" T_W=640 T_H=360 T_UP_WIDTH=2800 T_UP_HEIGHT=1500
expect_error "unknown quality is rejected"   "quality must be"                     "${UP[@]}" T_UP_QUALITY=superb
expect_error "pool=0 is rejected"            "pool must be between 1 and 4"        "${UP[@]}" T_UP_POOL=0
expect_error "pool=5 is rejected"            "pool must be between 1 and 4"        "${UP[@]}" T_UP_POOL=5
expect_error "strength=2 is rejected"        "strength must be between 0 and 1"    "${UP[@]}" T_UP_STRENGTH=2
expect_error "unknown format is rejected"    "format must be rgb24 or yuv420p8"    "${UP[@]}" T_UP_FORMAT=nv12
expect_error "unknown matrix is rejected"    "matrix must be"                      "${UP[@]}" T_UP_MATRIX=2020
expect_error "range=2 is rejected"           "range must be 0 (full) or 1 (limited)" "${UP[@]}" T_UP_RANGE=2
for q in auto bicubic low medium high ultra streaming_medium streaming_ultra; do
  got=$(vinfo "${UP[@]}" T_UP_QUALITY=$q T_UP_STRENGTH=0.5); check_eq "quality=$q is accepted" "$got" "1280x720 RGB24"
done

echo "== 3. correctness against libnvvsr called directly (byte for byte)"
# 1282x722 -> 2564x1444: neither size is a multiple of VapourSynth's row alignment, so row strides are padded.
export T_PATTERN=1 T_W=1282 T_H=722 T_N=3
vspipe "$CLIP" src.i420 >/dev/null 2>&1
check_eq "pattern source written (3 frames I420)" "$(stat -c %s src.i420)" "$((3 * (1282 * 722 + 2 * 641 * 361)))"
cmp_ref() {  # NAME QUALITY MATRIXARG RANGEARG FORMAT [env...]: reference run vs plugin run
  local name=$1 q=$2 m=$3 r=$4 f=$5; shift 5
  "$REF" 1282x722 2564x1444 "$q" "$m" "$r" "$f" 3 src.i420 ref.raw 2>ref.err || { bad "$name" "reference tool failed: $(cat ref.err)"; return; }
  local res; res=$(env T_UP=1 T_UP_WIDTH=2564 T_UP_HEIGHT=1444 T_UP_QUALITY="$q" "$@" python -I "$HERE/compare.py" "$CLIP" ref.raw 2>&1)
  case $res in *"max_abs_diff=0 "*) ok "$name ($res)" ;; *) bad "$name" "$res" ;; esac
}
cmp_ref "bicubic RGB24 == libnvvsr (709 by size, limited)" bicubic auto limited rgb24
cmp_ref "medium  RGB24 == libnvvsr (709 by size, limited)" medium auto limited rgb24
cmp_ref "bicubic RGB24, matrix=601 range=0 (full) == libnvvsr" bicubic 601 full rgb24 T_UP_MATRIX=601 T_UP_RANGE=0
cmp_ref "bicubic YUV420P8 output == libnvvsr (I420 path)" bicubic auto limited yuv420p8 T_UP_FORMAT=yuv420p8
cmp_ref "high    YUV420P8, matrix=709 range=1 == libnvvsr" high 709 limited yuv420p8 T_UP_FORMAT=yuv420p8 T_UP_MATRIX=709 T_UP_RANGE=1
unset T_PATTERN T_W T_H T_N

# Independent sanity bounds (no libnvvsr): grey input -> R=G=B and the level follows the limited-range mapping.
sanity() {  # QUALITY MAX_CHANNEL_SPREAD
  env T_UP=1 T_NEUTRAL=1 T_PATTERN=1 T_W=640 T_H=360 T_N=2 T_UP_WIDTH=1280 T_UP_HEIGHT=720 T_UP_QUALITY=$1 python -I - "$CLIP" "$2" <<'EOF2' 2>&1
import os, runpy, sys
import numpy as np
up = runpy.run_path(sys.argv[1])["clip"]
os.environ.pop("T_UP")
src = runpy.run_path(sys.argv[1])["clip"]  # the same script without Upscale
for n in range(2):
    y = np.asarray(src.get_frame(n)[0]).astype(float)
    r, g, b = (np.asarray(up.get_frame(n)[p]).astype(int) for p in range(3))
    expect = ((y - 16) * 255 / 219).clip(0, 255).mean()
    spread = max(np.abs(r - g).max(), np.abs(g - b).max())
    print("frame %d: mean(G)=%.1f expected %.1f, max|R-G|,|G-B|=%d, std(G)=%.1f" % (n, g.mean(), expect, spread, g.std()))
    assert abs(g.mean() - expect) < 3, "mean level off"
    assert spread <= int(sys.argv[2]), "grey input should give R=G=B"
    assert g.std() > 20, "output looks constant"
EOF2
}
for qs in "bicubic 3" "medium 12"; do
  set -- $qs; res=$(sanity "$1" "$2")
  case $res in *Error*) bad "sanity ($1): grey input keeps its level, R=G=B within $2, not constant" "$res" ;; *"frame 1:"*) ok "sanity ($1): grey input keeps its level, R=G=B within $2, not constant ($(echo "$res" | tail -1))" ;; *) bad "sanity ($1)" "$res" ;; esac
done

echo "== 3b. frame properties and colour selection"
python -I "$HERE/props_check.py" "$NVVSR_PLUGIN" 2>"$SCR/props.err" | while IFS= read -r line; do
  case $line in PASS*) echo "$line" ;; FAIL*) echo "$line" ;; esac
done | tee "$SCR/props.out"
PASS=$((PASS + $(grep -c '^PASS' "$SCR/props.out"))); FAIL=$((FAIL + $(grep -c '^FAIL' "$SCR/props.out")))
[ "$(grep -c . "$SCR/props.out")" -gt 0 ] || bad "props_check" "no output: $(cat "$SCR/props.err")"

echo "== 4. concurrency: results independent of pool size and request depth; no deadlock"
md5_of() {  # pool requests
  env T_UP=1 T_PATTERN=1 T_W=1920 T_H=1080 T_N=24 T_UP_WIDTH=2560 T_UP_HEIGHT=1440 T_UP_QUALITY=medium T_UP_POOL=$1 \
    timeout 180 vspipe --requests "$2" "$CLIP" - 2>/dev/null | md5sum | cut -d' ' -f1
}
m11=$(md5_of 1 1); m24=$(md5_of 2 4); m38=$(md5_of 3 8); m44=$(md5_of 4 4)
if [ -n "$m11" ] && [ "$m11" = "$m24" ] && [ "$m24" = "$m38" ] && [ "$m38" = "$m44" ] && [ "$m11" != "d41d8cd98f00b204e9800998ecf8427e" ]; then
  ok "24 frames 1080p->1440p MEDIUM: identical output for pool/requests 1/1, 2/4, 3/8, 4/4 ($m11)"
else bad "pool independence" "md5 1/1=$m11 2/4=$m24 3/8=$m38 4/4=$m44"; fi

bench_gate() {
  local busy; busy=$(pgrep -a 'meson|ninja|cc1plus|cc1' | head -3)
  [ -n "$busy" ] && { echo "a compiler is running: $busy"; return 1; }
  local others; others=$(nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader 2>/dev/null)
  [ -n "$others" ] && { echo "another process uses the GPU: $others"; return 1; }
  if command -v ollama >/dev/null 2>&1; then
    local ps; ps=$(ollama ps 2>/dev/null | tail -n +2)
    [ -n "$ps" ] && { echo "an Ollama model is resident: $ps"; return 1; }
  fi
  return 0
}

run_pool() {  # POOL FRAMES -> "fps peakMiB"
  local pool=$1 frames=$2 base peak=0 v out fps
  base=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1)
  : > "$SCR/vram.log"
  ( while :; do nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1 >> "$SCR/vram.log"; sleep 0.2; done ) &
  local sampler=$!
  out=$(env T_UP=1 T_W=1920 T_H=1080 T_N=$frames T_UP_WIDTH=2560 T_UP_HEIGHT=1440 T_UP_QUALITY=medium T_UP_POOL=$pool \
        timeout 300 vspipe --requests 4 "$CLIP" - 2>&1 >/dev/null)
  kill $sampler 2>/dev/null; wait $sampler 2>/dev/null
  while read -r v; do [ "$v" -gt "$peak" ] && peak=$v; done < "$SCR/vram.log"
  fps=$(printf '%s\n' "$out" | sed -n 's/.*(\([0-9.]*\) fps).*/\1/p')
  echo "${fps:-0} $((peak - base))"
}
# a short smoke run of every pool size is part of the default run (catches deadlocks via timeout; the fps
# shown there include start-up and are only indicative)
for p in 1 2 3 4; do
  r=$(run_pool $p 60); fps=${r% *}
  if awk "BEGIN{exit !($fps > 5)}"; then ok "pool=$p, 60 frames 1080p->1440p MEDIUM, --requests 4: ${fps} fps, VRAM +${r#* } MiB"; else bad "pool=$p smoke run" "fps=$fps"; fi
done
if [ $WANT_BENCH = 1 ]; then
  echo "== 4b. benchmark: 300 frames 1080p->1440p MEDIUM, vspipe --requests 4 to /dev/null"
  if why=$(bench_gate); then
    run_pool 1 60 >/dev/null  # warm-up (GPU clocks, effect cache)
    for p in 1 2 3; do
      r=$(run_pool $p 300); echo "BENCH pool=$p: ${r% *} fps, GPU memory +${r#* } MiB over idle"
    done
  else echo "SKIP bench: $why"; fi
fi

echo "== 5. error paths fall back to pass-through"
out=$(env NVVSR_LIBDIR=/nonexistent/dir "${UP[@]}" vspipe "$CLIP" --info - 2>&1); rc=$?
case $out in *"Upscale: super resolution is unavailable"*) [ $rc -eq 1 ] && ok "bogus NVVSR_LIBDIR: Upscale raises a clean VapourSynth error (rc=1)" || bad "bogus NVVSR_LIBDIR" "rc=$rc" ;; *) bad "bogus NVVSR_LIBDIR" "rc=$rc: $out" ;; esac
out=$(env NVVSR_LIBDIR= HOME=$SCR/nohome XDG_CONFIG_HOME=$SCR/xdg "${UP[@]}" vspipe "$CLIP" --info - 2>&1); rc=$?
case $out in *"super resolution is unavailable"*) ok "no library directory configured: clean error (rc=$rc)" ;; *) bad "no libdir" "rc=$rc: $out" ;; esac

# vsr.vpy under vspipe with mpv's variables
vsr() {  # VAR=value... -> "WxH FORMAT | first log line"
  local out err rc
  err=$SCR/vsr.err
  out=$(env T_VSR="$VSR_VPY" "$@" vspipe "$HERE/mpv_env.vpy" --info - 2>"$err"); rc=$?
  if [ $rc -ne 0 ]; then echo "SCRIPT-FAILED rc=$rc $(grep -m1 -E 'rror' "$err")"; return; fi
  echo "$(printf '%s\n' "$out" | sed -n 's/^Width: //p')x$(printf '%s\n' "$out" | sed -n 's/^Height: //p') $(printf '%s\n' "$out" | sed -n 's/^Format Name: //p')|$(grep -m1 '^vsr.vpy:' "$err")"
}
echo "-- mpv/vsr.vpy"
got=$(vsr T_DISPLAY=2560x1440 T_W=1280 T_H=720);              check_eq "vsr.vpy: 720p on a 2560x1440 display -> 2560x1440 YUV420P8" "$got" "2560x1440 YUV420P8|"
got=$(vsr T_DISPLAY=2560x1440 T_W=1920 T_H=1080);             check_eq "vsr.vpy: 1080p -> 2560x1440" "$got" "2560x1440 YUV420P8|"
got=$(vsr T_DISPLAY=2560x1440 T_W=854 T_H=480);               check_eq "vsr.vpy: 480p -> fitted to the display width, aspect kept, even (854x480 -> 2560x1438)" "${got%%|*}" "2560x1438 YUV420P8"
got=$(vsr T_DISPLAY=2560x1440 T_W=1280 T_H=544);              check_eq "vsr.vpy: 2.35:1 source keeps its aspect (1280x544 -> 2560x1088)" "${got%%|*}" "2560x1088 YUV420P8"
got=$(vsr T_DISPLAY=2560x1440 T_W=640 T_H=360);               check_eq "vsr.vpy: 360p (4x) -> 2560x1440" "${got%%|*}" "2560x1440 YUV420P8"
got=$(vsr T_DISPLAY=2560x1440 T_W=320 T_H=180);               check_eq "vsr.vpy: more than 4x is capped at 4x (320x180 -> 1280x720)" "${got%%|*}" "1280x720 YUV420P8"
got=$(vsr T_DISPLAY=1920x1080 T_W=1920 T_H=1080);             check_eq "vsr.vpy: source == display -> pass-through" "$got" "1920x1080 YUV420P8|"
got=$(vsr T_DISPLAY=2560x1440 T_W=2560 T_H=1440);             check_eq "vsr.vpy: source == display (1440p) -> pass-through" "$got" "2560x1440 YUV420P8|"
got=$(vsr T_DISPLAY=1920x1080 T_W=3840 T_H=2160);             check_eq "vsr.vpy: source larger than display -> never downscaled" "$got" "3840x2160 YUV420P8|"
got=$(vsr T_DISPLAY=2560x1440 T_W=2560 T_H=1080);             check_eq "vsr.vpy: ultrawide 2560x1080 on 2560x1440 (width already fits) -> pass-through" "$got" "2560x1080 YUV420P8|"
got=$(vsr T_DISPLAY=2560x1440 T_SRC_FORMAT=YUV420P10);        check_eq "vsr.vpy: 10-bit -> pass-through, with a reason" "$got" "1280x720 YUV420P10|vsr.vpy: YUV420P10 is not 8-bit 4:2:0, pass-through"
got=$(vsr T_DISPLAY=2560x1440 T_SRC_FORMAT=YUV444P8);         check_eq "vsr.vpy: 4:4:4 -> pass-through" "${got%%|*}" "1280x720 YUV444P8"
got=$(vsr T_DISPLAY=0x0);                                     check_eq "vsr.vpy: unknown display -> pass-through, with a reason" "$got" "1280x720 YUV420P8|vsr.vpy: display size unknown (set NVVSR_DISPLAY=WxH), pass-through"
got=$(vsr T_DISPLAY=0x0 NVVSR_DISPLAY=2560x1440);             check_eq "vsr.vpy: NVVSR_DISPLAY substitutes the display size" "${got%%|*}" "2560x1440 YUV420P8"
got=$(vsr T_DISPLAY=2560x1440 T_USERDATA="quality=high;pool=2;strength=0.3");   check_eq "vsr.vpy: user-data quality/pool/strength accepted" "$got" "2560x1440 YUV420P8|"
got=$(vsr T_DISPLAY=2560x1440 T_USERDATA="pool=9");           check_eq "vsr.vpy: pool=9 is clamped to 4" "$got" "2560x1440 YUV420P8|"
got=$(vsr T_DISPLAY=2560x1440 T_USERDATA="quality=superb");   case $got in "1280x720 YUV420P8|vsr.vpy: super resolution disabled, pass-through (Error: Upscale: quality must be"*) ok "vsr.vpy: bad quality -> pass-through, reason printed" ;; *) bad "vsr.vpy bad quality" "$got" ;; esac
got=$(vsr T_DISPLAY=2560x1440 T_USERDATA="strength=abc");     case $got in "1280x720 YUV420P8|vsr.vpy: super resolution disabled, pass-through (ValueError"*) ok "vsr.vpy: unparsable option -> pass-through, reason printed" ;; *) bad "vsr.vpy bad option" "$got" ;; esac
got=$(vsr T_DISPLAY=2560x1440 NVVSR_LIBDIR=/nonexistent/dir); case $got in "1280x720 YUV420P8|vsr.vpy: super resolution disabled, pass-through (Error: Upscale: super resolution is unavailable"*) ok "vsr.vpy: bogus NVVSR_LIBDIR -> pass-through (original size and format)" ;; *) bad "vsr.vpy bogus libdir" "$got" ;; esac
got=$(vsr T_DISPLAY=2560x1440 NVVSR_PLUGIN=/nonexistent/lib.so); case $got in "1280x720 YUV420P8|vsr.vpy: super resolution disabled, pass-through ("*) ok "vsr.vpy: missing plugin file -> pass-through" ;; *) bad "vsr.vpy missing plugin" "$got" ;; esac
got=$(vsr T_DISPLAY=2560x1440 NVVSR_PLUGIN=/etc/hostname);    case $got in "1280x720 YUV420P8|vsr.vpy: super resolution disabled, pass-through ("*) ok "vsr.vpy: plugin file that is not a plugin -> pass-through" ;; *) bad "vsr.vpy broken plugin" "$got" ;; esac

# ---- optional: end to end in the private mpv ----------------------------------------------------
if [ $WANT_MPV = 1 ]; then
  echo "== 6. mpv end to end (private build with --vf=vapoursynth)"
  MPV=${NVVSR_MPV:-$HOME/.local/opt/mpv-vsr/bin/mpv}
  if ! "$MPV" -v --no-config --idle=no 2>&1 | grep -q "List of enabled features:.* vapoursynth "; then
    echo "SKIP mpv: $MPV missing or built without vapoursynth"
  else
    ffmpeg -v error -y -f lavfi -i "testsrc2=size=1280x720:rate=30" -t 10 -c:v libx264 -pix_fmt yuv420p -preset veryfast "$SCR/test720.mp4"
    # mpv_run LOG [env VAR=value...] -- [mpv args...]: wall seconds on stdout; the output is in LOG
    mpv_run() {
      local log=$1; shift; local envs=()
      while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
      local t0 t1
      t0=$(date +%s.%N)
      env NVVSR_DISPLAY=2560x1440 "${envs[@]}" timeout 120 "$MPV" --no-config --vo=null --ao=null --no-audio \
        --msg-level=all=info,statusline=status \
        '--term-status-msg=STAT t=${time-pos} dropped=${frame-drop-count} dec_dropped=${decoder-frame-drop-count}' \
        "--vf=vapoursynth=file=$VSR_VPY:buffered-frames=4:concurrent-frames=2" "$@" "$SCR/test720.mp4" 2>&1 | tr '\r' '\n' >"$log"
      t1=$(date +%s.%N)
      echo "$t1 - $t0" | bc
    }
    wall=$(mpv_run mpv-fast.log -- --untimed)
    grep -q "^VO: \[null\] 2560x1440 yuv420p" "$SCR/mpv-fast.log" && ok "mpv: 1280x720 H.264 -> 2560x1440 yuv420p through vsr.vpy" || bad "mpv output size" "$(grep -m3 -E '^VO:|rror' "$SCR/mpv-fast.log")"
    stat=$(grep STAT "$SCR/mpv-fast.log" | tail -1)
    case $stat in *"dropped=0 dec_dropped=0") ok "mpv --untimed: 300 frames in ${wall} s = $(echo "scale=1; 300 / $wall" | bc) fps (incl. start-up), no dropped frames" ;; *) bad "mpv --untimed" "$stat" ;; esac
    wall=$(mpv_run mpv-real.log -- )
    stat=$(grep STAT "$SCR/mpv-real.log" | tail -1)
    case $stat in *"dropped=0 dec_dropped=0") ok "mpv real time: 10 s clip played in ${wall} s, no dropped frames" ;; *) bad "mpv real time" "$stat" ;; esac
    # failures must leave a playing video
    mpv_run mpv-bad1.log NVVSR_LIBDIR=/nonexistent -- --untimed >/dev/null
    grep -q "^VO: \[null\] 1280x720 yuv420p" "$SCR/mpv-bad1.log" && grep -q "^vsr.vpy: super resolution disabled" "$SCR/mpv-bad1.log" &&
      ok "mpv: bogus NVVSR_LIBDIR -> plays the original 1280x720, reason on stderr" || bad "mpv bogus libdir" "$(grep -m3 -E '^VO:|vsr.vpy|rror' "$SCR/mpv-bad1.log")"
    mpv_run mpv-bad2.log NVVSR_PLUGIN=/nonexistent/x.so -- --untimed >/dev/null
    grep -q "^VO: \[null\] 1280x720 yuv420p" "$SCR/mpv-bad2.log" && ok "mpv: missing plugin file -> plays the original" || bad "mpv missing plugin" "$(grep -m3 -E '^VO:|vsr.vpy|rror' "$SCR/mpv-bad2.log")"
    # runtime failure: a test build of the plugin that fails every frame after the 60th
    HOOK=$SCR/hook-build
    if cmake -S "$REPO/vapoursynth" -B "$HOOK" -DNVVSR_VS_TESTHOOK=ON -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$HOOK" -j >/dev/null; then
      mpv_run mpv-bad3.log NVVSR_PLUGIN="$HOOK/libvsnvvsr.so" NVVSR_VS_FAIL_AFTER=60 -- --untimed >/dev/null
      if grep -q "Disabling filter vapoursynth.00 because it has failed" "$SCR/mpv-bad3.log" && grep -q "Exiting... (End of file)" "$SCR/mpv-bad3.log"; then
        ok "mpv: frame errors at frame 60 -> mpv disables the filter and keeps playing to the end ($(grep -m1 'Filter error' "$SCR/mpv-bad3.log"))"
      else bad "mpv runtime failure" "$(grep -m4 -E 'rror|Disabling|Exiting' "$SCR/mpv-bad3.log")"; fi
    else bad "mpv runtime failure" "cannot build the test-hook plugin"; fi
  fi
fi

echo
echo "SUMMARY: $PASS passed, $FAIL failed"
exit $FAIL
