#!/usr/bin/env bash
# Tests of the VLC 3 module libnvvsr_plugin.so (vout display "nvvsr" + video filter "nvvsr"). Prints PASS/FAIL per
# check; the exit status is the number of failed checks.
#
#   vlc/tests/run.sh               functional tests, headless (about 6 minutes, uses the GPU)
#   vlc/tests/run.sh --cage        plus VLC with a real GL video output inside a headless cage compositor
#   vlc/tests/run.sh --bench       plus timing / VRAM numbers (needs an otherwise idle GPU, see below)
#   vlc/tests/run.sh --only TAG..  run only the sections with these tags (modules unit filter vout gating errors
#                                  eligible realtime cage bench)
#
# Environment (all optional): NVVSR_LIBDIR (default: the repo's .venv wheel directories), NVVSR_TEST_TMP (scratch
# directory, kept; default a temporary one), NVVSR_TEST_MEDIA (directory with ready-made clips, default
# <scratch>/media, generated with ffmpeg), NVVSR_GPU_LOCK (lock file created by --bench, default <scratch>/gpu-bench.lock).
#
# Headless means: VLC runs with the dummy interface, WITHOUT DISPLAY/WAYLAND_DISPLAY/DBUS (so no window can open on
# a desktop; the vout window modules just fail to connect), with a scratch HOME/XDG so no user configuration is read,
# a scratch plugin directory (VLC_PLUGIN_PATH, plus a scratch vlc-cache-gen run) and the stock plugin directory.
# Nothing is written to ~/.config, ~/.local or /usr. The pictures of the video output are captured by letting the nvvsr
# display wrap VLC's own `yuv` video output (--nvvsr-vout=yuv), which writes every shown frame to a file.
# --cage runs VLC inside `cage` with WLR_BACKENDS=headless (no connection to the desktop compositor).
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WANT_BENCH=0
WANT_CAGE=0
ONLY=""
while [ $# -gt 0 ]; do
  case $1 in
    --bench) WANT_BENCH=1 ;;
    --cage) WANT_CAGE=1 ;;
    --only) shift; while [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; do ONLY="$ONLY $1"; shift; done; continue ;;
    *) echo "usage: $0 [--cage] [--bench] [--only TAG...]" >&2; exit 2 ;;
  esac
  shift
done
want() { [ -z "$ONLY" ] || case " $ONLY " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

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
MEDIA=${NVVSR_TEST_MEDIA:-$SCR/media}
LOCK=${NVVSR_GPU_LOCK:-$SCR/gpu-bench.lock}
trap 'rm -f "$LOCK" /dev/shm/nvvsr-rt-$$ 2>/dev/null; [ -n "${NVVSR_TEST_TMP:-}" ] || rm -rf "$SCR"' EXIT

for tool in vlc ffmpeg ffprobe python3 cmake; do
  command -v "$tool" >/dev/null || { echo "FAIL prerequisite: $tool not found"; exit 1; }
done

if [ ! -f "$REPO/build/core/libnvvsr.so" ]; then
  cmake -S "$REPO/core" -B "$REPO/build/core" -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build "$REPO/build/core" -j >/dev/null
fi
# The test build has the test hooks (NVVSR_VLC_FAIL_AFTER, NVVSR_VLC_POWER_DIR) compiled in; it lives in its own directory.
BUILD=$REPO/vlc/build-test
cmake -S "$REPO/vlc" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DNVVSR_VLC_TESTHOOK=ON >/dev/null &&
  cmake --build "$BUILD" -j >/dev/null || { echo "FAIL build: cannot build the module"; exit 1; }
PLUGIN=$BUILD/libnvvsr_plugin.so
REF=$BUILD/nvvsr_vlc_ref

PASS=0; FAIL=0
ok()  { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1: $2"; FAIL=$((FAIL + 1)); }
check_eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$2', expected '$3'"; fi; }
check_ge() { if awk -v a="$2" -v b="$3" 'BEGIN { exit !(a >= b) }'; then ok "$1 ($2)"; else bad "$1" "got $2, expected >= $3"; fi; }
check_le() { if awk -v a="$2" -v b="$3" 'BEGIN { exit !(a <= b) }'; then ok "$1 ($2)"; else bad "$1" "got $2, expected <= $3"; fi; }
has() { grep -q -- "$2" "$1"; }                      # has FILE REGEX
count() { local n; n=$(grep -c -- "$2" "$1" 2>/dev/null); echo "${n:-0}"; }

mkdir -p "$SCR/home/.config" "$SCR/home/.cache" "$SCR/home/.local/share" "$SCR/plugins/video_output" "$SCR/rt" "$SCR/out"
chmod 700 "$SCR/rt"
ln -sf "$PLUGIN" "$SCR/plugins/video_output/libnvvsr_plugin.so"
rm -f "$SCR/plugins/plugins.dat"
/usr/lib/vlc/vlc-cache-gen "$SCR/plugins" >/dev/null 2>&1

# vrun LOG [env VAR=val ...] -- vlc-args...: one headless VLC run. Sets $RC and $ELAPSED (seconds).
RC=0; ELAPSED=0
vrun() {
  local log=$1; shift
  local envs=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
  [ "${1:-}" = "--" ] && shift
  local t0 t1
  t0=$(date +%s.%N)
  env -u DISPLAY -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u XDG_SESSION_TYPE \
    HOME="$SCR/home" XDG_CONFIG_HOME="$SCR/home/.config" XDG_CACHE_HOME="$SCR/home/.cache" XDG_DATA_HOME="$SCR/home/.local/share" \
    XDG_RUNTIME_DIR="$SCR/rt" VLC_PLUGIN_PATH="$SCR/plugins" TMPDIR="$SCR" "${envs[@]}" \
    timeout "${VLC_TIMEOUT:-120}" vlc -I dummy --no-audio --no-video-title-show --play-and-exit -vv \
      --avcodec-hw=none "$@" >"$log" 2>&1
  RC=$?
  t1=$(date +%s.%N)
  ELAPSED=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }')
}

# vrun_rc LOG "script" ENV... -- vlc-args: like vrun but with the oldrc interface; the script lines are rc commands
# or "sleep N" and are fed to the interface on stdin (it is how the fullscreen state is toggled headlessly).
vrun_rc() {
  local log=$1 script=$2; shift 2
  local envs=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
  [ "${1:-}" = "--" ] && shift
  ( while IFS= read -r line; do case $line in sleep*) sleep "${line#sleep }" ;; *) echo "$line" ;; esac; done <<<"$script" ) |
    env -u DISPLAY -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u XDG_SESSION_TYPE \
      HOME="$SCR/home" XDG_CONFIG_HOME="$SCR/home/.config" XDG_CACHE_HOME="$SCR/home/.cache" XDG_DATA_HOME="$SCR/home/.local/share" \
      XDG_RUNTIME_DIR="$SCR/rt" VLC_PLUGIN_PATH="$SCR/plugins" TMPDIR="$SCR" "${envs[@]}" \
      timeout "${VLC_TIMEOUT:-120}" vlc -I oldrc --rc-fake-tty --no-audio --no-video-title-show -vv \
        --avcodec-hw=none "$@" >"$log" 2>&1
  RC=${PIPESTATUS[1]}
}

# WxH of the first video stream of a media file
vsize() { ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$1" 2>/dev/null | head -1; }
# yuv header of a capture: "WxH" of the frames in a file written by the yuv video output
yuv_size() { head -c 200 "$1" | head -1 | sed -n 's/^[^ ]* W\([0-9]*\) H\([0-9]*\) .*/\1x\2/p'; }
yuv_frames() {  # number of FRAME records (counted by parsing, not by file size)
  python3 -I - "$1" <<'EOF'
import sys
f = open(sys.argv[1], "rb")
hdr = f.readline().decode().split()
w = int(hdr[1][1:]); h = int(hdr[2][1:]); fs = w * h * 3 // 2
n = 0
while True:
    tag = f.readline()
    if not tag:
        break
    if tag != b"FRAME\n":
        print("bad"); sys.exit(0)
    f.seek(fs, 1)
    n += 1
print(n)
EOF
}

# ---- media -------------------------------------------------------------------------------------
F="ffmpeg -v error -y -nostdin"
X264="-c:v libx264 -preset veryfast"
SRC() { echo "-f lavfi -i testsrc2=size=$1:rate=$2"; }
gen() { local name=$1; shift; [ -s "$MEDIA/$name" ] || { $F "$@" "$MEDIA/$name.part.${name##*.}" && mv "$MEDIA/$name.part.${name##*.}" "$MEDIA/$name"; }; }
mkdir -p "$MEDIA"
gen v720p30.mp4  $(SRC 1280x720 30)   -t 12 $X264 -pix_fmt yuv420p -g 60 &
gen v1080p60.mp4 $(SRC 1920x1080 60)  -t 12 $X264 -pix_fmt yuv420p -g 120 &
gen v1440p.mp4   $(SRC 2560x1440 30)  -t 4  $X264 -pix_fmt yuv420p &
gen v480p.mp4    $(SRC 854x480 30)    -t 8  $X264 -pix_fmt yuv420p &
gen v240p.mp4    $(SRC 320x240 30)    -t 4  $X264 -pix_fmt yuv420p &
gen v720p10b.mp4 $(SRC 1280x720 30)   -t 4  -c:v libx265 -preset veryfast -pix_fmt yuv420p10le -x265-params log-level=error &
gen hdr_pq.mp4   $(SRC 1280x720 30)   -t 4  -c:v libx265 -preset veryfast -pix_fmt yuv420p10le -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
                   -x265-params log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc &
gen v1080i.mp4   $(SRC 1920x1080 30000/1001) -t 4 -vf interlace=scan=tff $X264 -pix_fmt yuv420p -flags +ilme+ildct -x264-params tff=1 &
gen sdr_bt2020.mp4 $(SRC 1280x720 30) -t 4  $X264 -pix_fmt yuv420p -color_primaries bt2020 -colorspace bt2020nc -color_trc bt709 &
wait
for f in v720p30 v1080p60 v1440p v480p v240p v720p10b hdr_pq sdr_bt2020 v1080i; do
  [ -s "$MEDIA/$f.mp4" ] || { echo "FAIL media: cannot generate $f.mp4"; exit 1; }
done

# Raw source frames and references (frames are compared byte for byte with what VLC shows).
REFDIR=$SCR/ref; mkdir -p "$REFDIR"
mkref() {  # NAME IWxIH OWxOH QUALITY FRAMES [nobic]: decoded source frames, libnvvsr reference, bicubic reference
  local n=$1 is=$2 os=$3 q=$4 frames=$5 nobic=${6:-}
  [ -s "$REFDIR/$n.src.i420" ] || $F -i "$MEDIA/$n.mp4" -frames:v "$frames" -pix_fmt yuv420p -f rawvideo "$REFDIR/$n.src.i420"
  [ -s "$REFDIR/$n.ref.i420" ] || "$REF" "$is" "$os" "$q" auto limited "$frames" "$REFDIR/$n.src.i420" "$REFDIR/$n.ref.i420"
  [ -n "$nobic" ] && return 0
  [ -s "$REFDIR/$n.bic.i420" ] || $F -f rawvideo -pix_fmt yuv420p -s "$is" -i "$REFDIR/$n.src.i420" -vf "scale=${os/x/:}:flags=bicubic" -pix_fmt yuv420p -f rawvideo "$REFDIR/$n.bic.i420"
}
compare() { python3 -I "$HERE/compare.py" "$@"; }   # OUT.yuv REF BIC WxH [--frames N] [--expect-exact]

PANEL="--nvvsr-width=2560 --nvvsr-height=1440"
CAP="--vout=nvvsr --nvvsr-vout=yuv"                # the nvvsr display wrapping the frame-dumping `yuv` output

# ================================================================================================
if want modules; then
echo "== 1. module loads, exports, linking"
vrun "$SCR/list.log" -- --list
count_list=$(grep -c "nvvsr " "$SCR/list.log")
check_eq "vlc --list shows both entry points of the module (vout display + video filter)" "$count_list" "2"
has "$SCR/list.log" "nvvsr .*NVIDIA RTX video super resolution output" && ok "vout display \"NVIDIA RTX video super resolution output\" is listed" || bad "vout display listed" "not in --list"
has "$SCR/list.log" "nvvsr .*NVIDIA RTX video super resolution filter" && ok "video filter \"NVIDIA RTX video super resolution filter\" is listed" || bad "video filter listed" "not in --list"
vrun "$SCR/help.log" -- -p nvvsr --advanced --help-verbose
missing=""
for o in nvvsr-enable nvvsr-quality nvvsr-fullscreen-only nvvsr-ac-only nvvsr-width nvvsr-height nvvsr-min-height nvvsr-min-gain nvvsr-vout; do
  grep -q -- "--$o" "$SCR/help.log" || missing="$missing $o"
done
check_eq "all documented options exist in --help" "${missing:- none}" " none"
syms=$(nm -D --defined-only "$PLUGIN" | awk '{print $3}' | tr '\n' ' ')
check_eq "only vlc_entry__3_0_0f is exported" "$syms" "vlc_entry__3_0_0f "
lddout=$(env -u LD_LIBRARY_PATH ldd "$PLUGIN" | grep libnvvsr)
case $lddout in *"/libnvvsr.so"*) ok "libnvvsr.so resolves via RUNPATH without LD_LIBRARY_PATH" ;; *) bad "RUNPATH" "$lddout" ;; esac
rp=$(readelf -d "$PLUGIN" | sed -n 's/.*RUNPATH.*\[\(.*\)\]/\1/p')
case $rp in *: | :* | *::*) bad "RUNPATH has no empty entry" "$rp" ;; *) ok "RUNPATH has no empty (current directory) entry" ;; esac
check_eq "no wheel/CUDA library is linked (libnvvsr dlopens them)" "$(readelf -d "$PLUGIN" | grep -c -i 'NEEDED.*\(cuda\|nvvfx\|nvidia\|npp\)')" "0"
fi

# ================================================================================================
if want unit; then
echo "== 2. unit tests of the pure logic (classification, geometry, power, CPU fallback)"
out=$(TMPDIR=$SCR "$BUILD/nvvsr_vlc_unit" 2>&1); rc=$?
printf '%s\n' "$out" | grep -E "^(PASS|FAIL)"
up=$(printf '%s\n' "$out" | grep -c "^PASS"); uf=$(printf '%s\n' "$out" | grep -c "^FAIL")
PASS=$((PASS + up)); FAIL=$((FAIL + uf))
[ $rc -eq 0 ] && [ "$uf" -eq 0 ] || bad "unit test binary" "exit status $rc"
fi

# ================================================================================================
if want filter; then
echo "== 3. why a video filter is not enough in the vout, and what the video filter does"
vrun "$SCR/croppadd.log" -- --vout=dummy --video-filter=croppadd --croppadd-croptop=100 --run-time=2 "$MEDIA/v720p30.mp4"
has "$SCR/croppadd.log" "Adding a filter to compensate for format changes" && ok "stock evidence: the vout adds a converter after a size-changing video filter (croppadd 1280x620 -> 1280x720)" || bad "vout compensation" "log line missing"
vrun "$SCR/vf_in_vout.log" -- --vout=dummy --video-filter=nvvsr --run-time=2 "$MEDIA/v720p30.mp4"
check_eq "--video-filter=nvvsr in the vout: playback continues" "$RC" "0"
has "$SCR/vf_in_vout.log" "nvvsr filter warning: not loaded" && ok "the nvvsr video filter refuses to load in the vout, with a warning" || bad "filter refusal" "no warning"
check_eq "...and it never creates a super resolution context there" "$(count "$SCR/vf_in_vout.log" 'super resolution [0-9]*x[0-9]* ->')" "0"

mkref v720p30 1280x720 2560x1440 high 60
rm -f "$SCR/out/tc.ts"
vrun "$SCR/tc.log" -- --sout "#transcode{vfilter=nvvsr{width=2560,height=1440,quality=high},vcodec=mp4v,vb=40000,fps=30}:std{access=file,mux=ts,dst=$SCR/out/tc.ts}" --run-time=3 "$MEDIA/v720p30.mp4"
check_eq "transcode with vfilter=nvvsr: exit status" "$RC" "0"
check_eq "transcoded stream is 2560x1440" "$(vsize "$SCR/out/tc.ts")" "2560x1440"
has "$SCR/tc.log" "nvvsr filter: super resolution 1280x720 -> 2560x1440" && ok "log: filter created 1280x720 -> 2560x1440" || bad "filter log" "line missing"
$F -i "$SCR/out/tc.ts" -frames:v 1 -pix_fmt yuv420p -f rawvideo "$SCR/out/tc.f0.i420" 2>/dev/null
p=$(python3 -I - "$SCR/out/tc.f0.i420" "$REFDIR/v720p30.ref.i420" "$REFDIR/v720p30.bic.i420" <<'EOF'
import sys, numpy as np
w, h = 2560, 1440; fs = w * h * 3 // 2
a = np.fromfile(sys.argv[1], dtype=np.uint8, count=fs)
def psnr(x, y):
    d = x.astype(np.int32) - y.astype(np.int32); m = float(np.mean(d * d))
    return 99.0 if m == 0 else 10 * np.log10(255 * 255 / m)
ref = np.memmap(sys.argv[2], dtype=np.uint8, mode="r"); bic = np.memmap(sys.argv[3], dtype=np.uint8, mode="r")
n = len(ref) // fs
best = max(range(n), key=lambda i: psnr(a[: w * h], ref[i * fs: i * fs + w * h]))
print("%.1f %.1f" % (psnr(a, ref[best * fs: (best + 1) * fs]), psnr(a, bic[best * fs: (best + 1) * fs])))
EOF
)
check_ge "first transcoded frame vs libnvvsr reference (lossy MPEG-4, 40 Mbit/s) PSNR dB" "${p%% *}" 34
for clip in v1440p v240p; do
  rm -f "$SCR/out/tc2.ts"
  vrun "$SCR/tc2.log" -- --sout "#transcode{vfilter=nvvsr{width=2560,height=1440},vcodec=mp4v,vb=40000}:std{access=file,mux=ts,dst=$SCR/out/tc2.ts}" "$MEDIA/$clip.mp4"
  want_size=$(vsize "$MEDIA/$clip.mp4")
  check_eq "transcode $clip: not upscaled by the filter (stays $want_size)" "$(vsize "$SCR/out/tc2.ts")" "$want_size"
done
fi

# ================================================================================================
if want vout; then
echo "== 4. super resolution through the vout display (frames captured from VLC's own output)"
mkref v720p30 1280x720 2560x1440 high 60
rm -f "$SCR/out/sr720.yuv"
vrun "$SCR/sr720.log" -- $CAP --yuv-file="$SCR/out/sr720.yuv" --fullscreen $PANEL --run-time=4 "$MEDIA/v720p30.mp4"
check_eq "720p, fullscreen, panel 2560x1440: exit status" "$RC" "0"
check_eq "720p: frames shown by the video output are 2560x1440 (header of the capture)" "$(yuv_size "$SCR/out/sr720.yuv")" "2560x1440"
has "$SCR/sr720.log" "super resolution on: 1280x720 -> 2560x1440" && ok "log: super resolution on 1280x720 -> 2560x1440" || bad "sr log" "line missing"
res=$(compare "$SCR/out/sr720.yuv" "$REFDIR/v720p30.ref.i420" "$REFDIR/v720p30.bic.i420" 2560x1440 --frames 12 --expect-exact 2>&1); rc=$?
echo "     $res"
[ $rc -eq 0 ] && ok "720p: 12 captured frames are byte-identical to libnvvsr called directly on the same decoded frames" || bad "720p frames" "$res"
ps=$(printf '%s' "$res" | sed -n 's/.*psnr_bicubic_mean=\([0-9.]*\).*/\1/p'); ss=$(printf '%s' "$res" | sed -n 's/.*ssim_vs_bicubic=\([0-9.]*\).*/\1/p')
check_ge "720p: PSNR of the SR frames vs a bicubic upscale of the same source frame (dB, the SR is a different image, not a copy)" "${ps:-0}" 25
check_le "720p: ...and it is not just the bicubic image either (SSIM vs bicubic < 0.9999)" "${ss:-1}" 0.9999
check_ge "720p: shown frames count" "$(yuv_frames "$SCR/out/sr720.yuv")" 60

mkref v1080p60 1920x1080 2560x1440 auto 100 nobic
rm -f "$SCR/out/sr1080.yuv"
vrun "$SCR/sr1080.log" -- $CAP --yuv-file="$SCR/out/sr1080.yuv" --fullscreen $PANEL --run-time=3 "$MEDIA/v1080p60.mp4"
check_eq "1080p60 -> 2560x1440: frames are 2560x1440" "$(yuv_size "$SCR/out/sr1080.yuv")" "2560x1440"
res=$(compare "$SCR/out/sr1080.yuv" "$REFDIR/v1080p60.ref.i420" - 2560x1440 --frames 6 --expect-exact 2>&1); rc=$?
echo "     $res"
[ $rc -eq 0 ] && ok "1080p60: 6 captured frames are byte-identical to libnvvsr called directly" || bad "1080p frames" "$res"

# panel from the display size instead of the options: the dummy/yuv output reports the source size as its window,
# so no upscaling is wanted (scale 1.0); that is the "display unknown/not larger" rule
rm -f "$SCR/out/nopanel.yuv"
vrun "$SCR/nopanel.log" -- $CAP --yuv-file="$SCR/out/nopanel.yuv" --fullscreen --run-time=2 "$MEDIA/v720p30.mp4"
check_eq "no panel size known (display = video size): left alone at 1280x720" "$(yuv_size "$SCR/out/nopanel.yuv")" "1280x720"
check_eq "...and no super resolution context was created" "$(count "$SCR/nopanel.log" 'super resolution [0-9]*x[0-9]* ->')" "0"

# quality option reaches the core
rm -f "$SCR/out/q.yuv"
vrun "$SCR/q.log" -- $CAP --yuv-file="$SCR/out/q.yuv" --fullscreen $PANEL --nvvsr-quality=medium --run-time=2 "$MEDIA/v720p30.mp4"
has "$SCR/q.log" "quality 2 (requested 2)" && ok "--nvvsr-quality=medium reaches libnvvsr (quality 2)" || bad "quality option" "$(grep -m1 'super resolution 1280x720' "$SCR/q.log")"
has "$SCR/sr720.log" "quality 3 (requested -1)" && ok "quality auto at 720p resolves to HIGH (3)" || bad "auto quality" "$(grep -m1 'super resolution 1280x720' "$SCR/sr720.log")"
vrun "$SCR/q2.log" -- $CAP --yuv-file="$SCR/out/q.yuv" --fullscreen $PANEL --nvvsr-quality=bogus --run-time=2 "$MEDIA/v720p30.mp4"
has "$SCR/q2.log" "unknown nvvsr-quality 'bogus', using auto" && ok "an unknown quality name warns and falls back to auto" || bad "bogus quality" "no warning"
fi

# ================================================================================================
if want gating; then
echo "== 5. fullscreen / power gating"
mkref v720p30 1280x720 2560x1440 high 60
# never fullscreen: pass-through, frames identical to the decoder's
rm -f "$SCR/out/win.yuv"
vrun "$SCR/win.log" -- $CAP --yuv-file="$SCR/out/win.yuv" $PANEL --run-time=3 "$MEDIA/v720p30.mp4"
check_eq "windowed: shown frames keep the video size" "$(yuv_size "$SCR/out/win.yuv")" "1280x720"
check_eq "windowed: no super resolution context is created" "$(count "$SCR/win.log" 'super resolution [0-9]*x[0-9]* ->')" "0"
res=$(compare "$SCR/out/win.yuv" "$REFDIR/v720p30.src.i420" "$REFDIR/v720p30.src.i420" 1280x720 --frames 8 --expect-exact 2>&1); rc=$?
[ $rc -eq 0 ] && ok "windowed: pass-through frames are byte-identical to the decoder's frames" || bad "pass-through frames" "$res"

# toggled through the vout's fullscreen variable (the same one the GUI, the F key and double click change)
rm -f "$SCR/out/toggle.yuv"
vrun_rc "$SCR/toggle.log" $'sleep 3\nf on\nsleep 3\nf off\nsleep 2\nquit' -- $CAP --yuv-file="$SCR/out/toggle.yuv" $PANEL "$MEDIA/v720p30.mp4"
on_line=$(grep -n "super resolution on: 1280x720 -> 2560x1440" "$SCR/toggle.log" | head -1 | cut -d: -f1)
off_line=$(grep -n "super resolution off (not fullscreen)" "$SCR/toggle.log" | head -1 | cut -d: -f1)
[ -n "$on_line" ] && ok "entering fullscreen switches super resolution on" || bad "toggle on" "no 'super resolution on' line"
[ -n "$off_line" ] && [ -n "$on_line" ] && [ "$off_line" -gt "$on_line" ] && ok "leaving fullscreen switches it off again (after the 'on')" || bad "toggle off" "on=$on_line off=$off_line"
check_eq "after leaving fullscreen the shown frames are back at 1280x720" "$(yuv_size "$SCR/out/toggle.yuv")" "1280x720"
check_eq "the context is created once for the whole session (effect recycled by the core)" "$(count "$SCR/toggle.log" 'super resolution [0-9]*x[0-9]* ->.*quality')" "1"
check_ge "frames processed while fullscreen" "$(sed -n 's/.*super resolution 1280x720 -> 2560x1440: \([0-9]*\) frames.*/\1/p' "$SCR/toggle.log" | head -1)" 60
sw=$(sed -n 's/.*video output reopened for \([a-z ]*\) in \([0-9.]*\) ms.*/\1:\2/p' "$SCR/toggle.log" | tr '\n' ' ')
echo "     reopen times (headless dummy-like output): $sw"

# --no-nvvsr-fullscreen-only: any window bigger than the video
rm -f "$SCR/out/nofs.yuv"
vrun "$SCR/nofs.log" -- $CAP --yuv-file="$SCR/out/nofs.yuv" --no-nvvsr-fullscreen-only $PANEL --run-time=3 "$MEDIA/v720p30.mp4"
check_eq "--no-nvvsr-fullscreen-only: upscales without fullscreen" "$(yuv_size "$SCR/out/nofs.yuv")" "2560x1440"

# power: battery only -> off, mains -> on
mkdir -p "$SCR/power-bat/BAT0" "$SCR/power-ac/BAT0" "$SCR/power-ac/AC"
echo Battery >"$SCR/power-bat/BAT0/type"; echo Battery >"$SCR/power-ac/BAT0/type"
echo Mains >"$SCR/power-ac/AC/type"; echo 1 >"$SCR/power-ac/AC/online"
rm -f "$SCR/out/bat.yuv" "$SCR/out/ac.yuv"
vrun "$SCR/bat.log" NVVSR_VLC_POWER_DIR="$SCR/power-bat" -- $CAP --yuv-file="$SCR/out/bat.yuv" --fullscreen $PANEL --run-time=3 "$MEDIA/v720p30.mp4"
check_eq "on battery (ac-only default): not upscaled" "$(yuv_size "$SCR/out/bat.yuv")" "1280x720"
vrun "$SCR/ac.log" NVVSR_VLC_POWER_DIR="$SCR/power-ac" -- $CAP --yuv-file="$SCR/out/ac.yuv" --fullscreen $PANEL --run-time=3 "$MEDIA/v720p30.mp4"
check_eq "on mains: upscaled" "$(yuv_size "$SCR/out/ac.yuv")" "2560x1440"
rm -f "$SCR/out/bat2.yuv"
vrun "$SCR/bat2.log" NVVSR_VLC_POWER_DIR="$SCR/power-bat" -- $CAP --yuv-file="$SCR/out/bat2.yuv" --fullscreen --no-nvvsr-ac-only $PANEL --run-time=3 "$MEDIA/v720p30.mp4"
check_eq "on battery with --no-nvvsr-ac-only: upscaled" "$(yuv_size "$SCR/out/bat2.yuv")" "2560x1440"
fi

# ================================================================================================
if want errors; then
echo "== 6. failures fall back to the original video"
mkref v720p30 1280x720 2560x1440 high 60
# bogus library directory
rm -f "$SCR/out/bogus.yuv"
vrun "$SCR/bogus.log" NVVSR_LIBDIR=/nonexistent -- $CAP --yuv-file="$SCR/out/bogus.yuv" --fullscreen $PANEL "$MEDIA/v720p30.mp4"
check_eq "bogus NVVSR_LIBDIR: exit status" "$RC" "0"
check_eq "bogus NVVSR_LIBDIR: plays the whole original (shown frames at 1280x720)" "$(yuv_size "$SCR/out/bogus.yuv")" "1280x720"
check_ge "bogus NVVSR_LIBDIR: frames shown" "$(yuv_frames "$SCR/out/bogus.yuv")" 300
check_eq "bogus NVVSR_LIBDIR: exactly one warning from the module" "$(count "$SCR/bogus.log" 'nvvsr vout display warning')" "1"
has "$SCR/bogus.log" "NVIDIA super resolution is not available" && ok "the warning says super resolution is unavailable" || bad "bogus warning text" "$(grep -m1 'nvvsr vout display warning' "$SCR/bogus.log")"
res=$(compare "$SCR/out/bogus.yuv" "$REFDIR/v720p30.src.i420" "$REFDIR/v720p30.src.i420" 1280x720 --frames 8 --expect-exact 2>&1) && ok "bogus NVVSR_LIBDIR: shown frames are the original decoder frames" || bad "bogus frames" "$res"
# no library directory configured at all (and no ~/.config/nvvsr/libdir in the scratch HOME)
rm -f "$SCR/out/nolib.yuv"
vrun "$SCR/nolib.log" NVVSR_LIBDIR= -- $CAP --yuv-file="$SCR/out/nolib.yuv" --fullscreen $PANEL --run-time=3 "$MEDIA/v720p30.mp4"
check_eq "no libdir configured: plays the original" "$(yuv_size "$SCR/out/nolib.yuv")" "1280x720"
check_eq "no libdir configured: one warning" "$(count "$SCR/nolib.log" 'nvvsr vout display warning')" "1"
# the module is also automatically bypassed once a probe failed in this process (second file of a playlist)
rm -f "$SCR/out/pl.yuv"
vrun "$SCR/pl.log" NVVSR_LIBDIR=/nonexistent -- $CAP --yuv-file="$SCR/out/pl.yuv" --fullscreen $PANEL --run-time=2 "$MEDIA/v720p30.mp4" "$MEDIA/v480p.mp4"
check_eq "playlist after a failed probe: still exactly one warning" "$(count "$SCR/pl.log" 'nvvsr vout display warning')" "1"
check_eq "playlist after a failed probe: exit status" "$RC" "0"

# SR fails in the middle of the stream (test hook): CPU fallback for the frame, back to pass-through, no crash
rm -f "$SCR/out/failmid.yuv"
vrun "$SCR/failmid.log" NVVSR_VLC_FAIL_AFTER=15 -- $CAP --yuv-file="$SCR/out/failmid.yuv" --fullscreen $PANEL --run-time=6 "$MEDIA/v720p30.mp4"
check_eq "SR failing after 15 frames: exit status" "$RC" "0"
check_eq "SR failing after 15 frames: one failure warning" "$(count "$SCR/failmid.log" 'super resolution failed')" "1"
has "$SCR/failmid.log" "super resolution off (unavailable)" && ok "after the failure the display goes back to the source size" || bad "failure fallback" "no 'off (unavailable)' line"
check_eq "after the failure the shown frames are at 1280x720" "$(yuv_size "$SCR/out/failmid.yuv")" "1280x720"
check_ge "playback continued after the failure (frames shown)" "$(yuv_frames "$SCR/out/failmid.yuv")" 60
has "$SCR/failmid.log" "frames by CPU fallback" && ok "summary reports the frames made by the CPU fallback" || bad "fallback summary" "line missing"
check_eq "no crash/segfault in any of the failure runs" "$(grep -l -E 'dumped core|Segmentation' "$SCR"/bogus.log "$SCR"/nolib.log "$SCR"/failmid.log 2>/dev/null | wc -l)" "0"
fi

# ================================================================================================
if want eligible; then
echo "== 7. eligibility: sources that must be left alone"
for spec in "v1440p:2560x1440:1440p source on a 1440p panel" "v720p10b:1280x720:10-bit 4:2:0 (HEVC main10)" "hdr_pq:1280x720:HDR (PQ, BT.2020)" "sdr_bt2020:1280x720:SDR with BT.2020 primaries" "v240p:320x240:240p (below nvvsr-min-height 360)"; do
  IFS=: read -r clip size desc <<<"$spec"
  rm -f "$SCR/out/el.yuv"
  vrun "$SCR/el_$clip.log" -- $CAP --yuv-file="$SCR/out/el.yuv" --fullscreen $PANEL --run-time=3 "$MEDIA/$clip.mp4"
  check_eq "$desc: untouched ($size)" "$(yuv_size "$SCR/out/el.yuv")" "$size"
  check_eq "$desc: no super resolution context" "$(count "$SCR/el_$clip.log" 'super resolution [0-9]*x[0-9]* ->')" "0"
done
rm -f "$SCR/out/el.yuv"
vrun "$SCR/el_i.log" -- $CAP --yuv-file="$SCR/out/el.yuv" --fullscreen --deinterlace=0 $PANEL --run-time=3 "$MEDIA/v1080i.mp4"
check_eq "interlaced 1080i without a deinterlacer: untouched (1920x1080)" "$(yuv_size "$SCR/out/el.yuv")" "1920x1080"
has "$SCR/el_i.log" "interlaced video: super resolution is not applied" && ok "interlaced: the module says why" || bad "interlaced log" "line missing"
rm -f "$SCR/out/el.yuv"
vrun "$SCR/el_240.log" -- $CAP --yuv-file="$SCR/out/el.yuv" --fullscreen $PANEL --nvvsr-min-height=0 --run-time=3 "$MEDIA/v240p.mp4"
check_eq "240p with --nvvsr-min-height=0: upscaled, capped at 4x (1280x960)" "$(yuv_size "$SCR/out/el.yuv")" "1280x960"
rm -f "$SCR/out/el.yuv"
vrun "$SCR/el_gain.log" -- $CAP --yuv-file="$SCR/out/el.yuv" --fullscreen --nvvsr-width=1290 --nvvsr-height=726 --run-time=3 "$MEDIA/v720p30.mp4"
check_eq "720p on a 1290x726 panel (+0.8%, below nvvsr-min-gain 2%): untouched" "$(yuv_size "$SCR/out/el.yuv")" "1280x720"
# auto output selection: without --vout the module is picked by score and steps aside when it cannot help
vrun "$SCR/auto_list.log" -- --vout=dummy --run-time=1 "$MEDIA/v1440p.mp4"
has "$SCR/auto_list.log" "using vout display module \"vdummy\"" && ok "explicit --vout=dummy does not involve the module at all" || bad "explicit vout" "vdummy not used"
fi

# ================================================================================================
if want realtime; then
echo "== 8. real-time playback, 0 late/lost pictures"
rt() {  # NAME CLIP SECONDS FPS
  local name=$1 clip=$2 secs=$3 fps=$4
  vrun "$SCR/rt_$name.log" -- --vout=nvvsr --nvvsr-vout=dummy --fullscreen $PANEL --run-time="$secs" "$MEDIA/$clip.mp4"
  check_eq "$name: exit status" "$RC" "0"
  local late lost frames avg
  late=$(count "$SCR/rt_$name.log" 'picture is too late to be displayed')
  lost=$(count "$SCR/rt_$name.log" 'buffer deleted late\|late picture skipped\|Too many late\|lost picture\|output picture dropped\|dropping frames')
  check_eq "$name: pictures dropped as too late" "$late" "0"
  check_eq "$name: lost/dropped pictures reported by the output" "$lost" "0"
  frames=$(sed -n 's/.*super resolution [0-9x]* -> [0-9x]*: \([0-9]*\) frames, SR \([0-9.]*\) ms avg.*/\1/p' "$SCR/rt_$name.log" | head -1)
  avg=$(sed -n 's/.*super resolution [0-9x]* -> [0-9x]*: \([0-9]*\) frames, SR \([0-9.]*\) ms avg.*/\2/p' "$SCR/rt_$name.log" | head -1)
  check_ge "$name: frames super-resolved in $secs s at $fps fps (>= 90% of the clip)" "${frames:-0}" "$(awk -v s="$secs" -v f="$fps" 'BEGIN { printf "%d", s * f * 0.9 }')"
  check_le "$name: wall clock is real time (s)" "$ELAPSED" "$(awk -v s="$secs" 'BEGIN { print s + 4 }')"
  echo "     $name: $frames SR frames, SR $avg ms avg, $ELAPSED s wall"
}
rt 720p30 v720p30 10 30
rt 1080p60 v1080p60 10 60
fi

# ================================================================================================
if [ "$WANT_CAGE" = 1 ] && want cage; then
echo "== 9. real GL video output inside a headless cage compositor"
if ! command -v cage >/dev/null; then
  echo "SKIP cage not installed"
else
  ln -sfn "$SCR/rt" "/dev/shm/nvvsr-rt-$$"    # wayland socket paths must be short
  run_cage() {  # LOG vlc-args...
    local log=$1; shift
    env -u WAYLAND_DISPLAY -u DISPLAY -u DBUS_SESSION_BUS_ADDRESS -u HYPRLAND_INSTANCE_SIGNATURE -u XDG_SESSION_TYPE \
      XDG_RUNTIME_DIR="/dev/shm/nvvsr-rt-$$" WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
      HOME="$SCR/home" XDG_CONFIG_HOME="$SCR/home/.config" XDG_CACHE_HOME="$SCR/home/.cache" XDG_DATA_HOME="$SCR/home/.local/share" \
      VLC_PLUGIN_PATH="$SCR/plugins" \
      timeout 90 cage -- env -u WAYLAND_DISPLAY vlc -I dummy --no-audio --no-video-title-show --play-and-exit -vv "$@" >"$log" 2>&1
    RC=$?
  }
  # the compositor's output is 1280x720: the 480p clip is upscaled to fill it; the output module is chosen by VLC
  run_cage "$SCR/cage.log" --fullscreen --run-time=6 "$MEDIA/v480p.mp4"
  check_eq "cage: exit status" "$RC" "0"
  has "$SCR/cage.log" "using vout display module \"nvvsr\"" && ok "cage: the module is picked automatically (no --vout)" || bad "cage auto" "nvvsr not selected"
  has "$SCR/cage.log" "wrapping the video output (OpenGL video output)" && ok "cage: it wraps VLC's normal OpenGL output" || bad "cage nested" "$(grep -m1 'wrapping' "$SCR/cage.log")"
  has "$SCR/cage.log" "super resolution on: 854x480 -> 1280x720" && ok "cage: super resolution on 854x480 -> 1280x720 (the window size)" || bad "cage on" "line missing"
  fr=$(sed -n 's/.*super resolution 854x480 -> 1280x720: \([0-9]*\) frames.*/\1/p' "$SCR/cage.log" | head -1)
  check_ge "cage: frames super-resolved on the real output" "${fr:-0}" 60
  has "$SCR/cage.log" " 0 frames by CPU fallback" && ok "cage: 0 frames by CPU fallback" || bad "cage fallback" "$(grep -m1 'CPU fallback' "$SCR/cage.log")"
  run_cage "$SCR/cage_w.log" --run-time=4 "$MEDIA/v480p.mp4"
  check_eq "cage: windowed (not --fullscreen) is not upscaled" "$(count "$SCR/cage_w.log" 'super resolution on')" "0"
  rm -f "/dev/shm/nvvsr-rt-$$"
fi
fi

# ================================================================================================
if [ "$WANT_BENCH" = 1 ] && want bench; then
echo "== 10. timing and VRAM (needs an idle GPU)"
busy=$(nvidia-smi --query-compute-apps=pid --format=csv,noheader 2>/dev/null | grep -c .)
oll=$(command -v ollama >/dev/null && ollama ps 2>/dev/null | tail -n +2 | grep -c .)
if [ "${busy:-0}" -ne 0 ] || [ "${oll:-0}" -ne 0 ]; then
  echo "SKIP bench: the GPU is in use (compute apps: $busy, ollama models: $oll)"
else
  echo "$$" >"$LOCK"
  gpu_mem() { nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits | head -1; }
  sample_max() {  # PID_FILE -> prints the max memory.used seen until the file disappears
    local m=0 v
    while [ -e "$1" ]; do v=$(gpu_mem); [ "$v" -gt "$m" ] && m=$v; sleep 0.2; done
    echo "$m"
  }
  base=$(gpu_mem)
  for spec in "720p30:v720p30:2560x1440" "1080p60:v1080p60:2560x1440"; do
    IFS=: read -r name clip size <<<"$spec"
    marker=$SCR/bench.running; : >"$marker"
    sample_max "$marker" >"$SCR/bench.mem" &
    spid=$!
    vrun "$SCR/bench_$name.log" -- --vout=nvvsr --nvvsr-vout=dummy --fullscreen $PANEL --run-time=10 "$MEDIA/$clip.mp4"
    rm -f "$marker"; wait $spid
    peak=$(cat "$SCR/bench.mem")
    avg=$(sed -n 's/.*SR \([0-9.]*\) ms avg \/ \([0-9.]*\) ms max.*/\1 ms avg, \2 ms max/p' "$SCR/bench_$name.log" | head -1)
    late=$(count "$SCR/bench_$name.log" 'picture is too late to be displayed')
    echo "     $name -> $size: SR $avg; VRAM peak $((peak - base)) MiB above idle ($peak MiB total, $base MiB idle); late pictures $late"
    ok "bench $name: recorded"
  done
  # SR off (same clip, windowed): the cost of the wrapper alone
  vrun "$SCR/bench_off.log" -- --vout=nvvsr --nvvsr-vout=dummy $PANEL --run-time=10 "$MEDIA/v1080p60.mp4"
  echo "     1080p60 windowed (pass-through): wall $ELAPSED s, late pictures $(count "$SCR/bench_off.log" 'picture is too late to be displayed')"
  rm -f "$LOCK"
fi
fi

echo
echo "== summary: $PASS passed, $FAIL failed"
exit $FAIL
