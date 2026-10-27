#!/bin/bash
# Self-test of compare.cpp on small synthetic frames (no GPU): identical frames, a degraded copy checked against
# ffmpeg's own psnr filter, frame order, --frames, a bad stream and a usage error. Prints PASS/FAIL per check and a
# SUMMARY line; the exit status is the number of failed checks.
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
PASS=0
FAIL=0
ok()  { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1: $2"; FAIL=$((FAIL + 1)); }

for tool in g++ ffmpeg; do
  command -v "$tool" >/dev/null || { echo "FAIL prerequisite: $tool not found"; exit 1; }
done
g++ -std=c++17 -O2 -Wall -Wextra -o "$T/compare" "$HERE/compare.cpp" || { echo "FAIL build: compare.cpp does not compile"; exit 1; }
C=$T/compare
W=320
H=180
SIZE=${W}x${H}
FS=$((W * H * 3 / 2))

ffmpeg -v error -f lavfi -i "testsrc2=size=$SIZE:rate=30" -frames:v 3 -pix_fmt yuv420p -f rawvideo "$T/ref.i420" \
  || { echo "FAIL prerequisite: ffmpeg could not make the source frames"; exit 1; }

# mkyuv OUT.yuv RAW.i420 IDX...: a VLC-style yuv capture (header line, then FRAME + the frames picked by index)
mkyuv() {
  local out=$1 raw=$2 i
  shift 2
  {
    printf 'YUV4MPEG2 W%d H%d F30:1 Ip A1:1 C420\n' "$W" "$H"
    for i in "$@"; do
      printf 'FRAME\n'
      tail -c +$((i * FS + 1)) "$raw" | head -c "$FS"
    done
  } >"$out"
}

# 1. identical frames: exact, PSNR inf, exit 0
mkyuv "$T/same.yuv" "$T/ref.i420" 0 1 2
out=$("$C" "$T/same.yuv" "$T/ref.i420" - "$SIZE" --frames 3 --expect-exact); rc=$?
echo "     $out"
if [ "$rc" -eq 0 ]; then ok "identical frames: exit 0"; else bad "identical frames: exit" "got $rc, expected 0"; fi
case $out in *"exact=3/3"*) ok "identical frames: exact=3/3" ;; *) bad "identical frames: exact" "$out" ;; esac
case $out in *"psnr_ref_min=inf"*) ok "identical frames: psnr inf" ;; *) bad "identical frames: psnr" "$out" ;; esac

# 2. degraded copy (noise): PSNR within 0.05 dB of ffmpeg's psnr filter (worst frame), exit per the rules (< 45 dB -> 1)
ffmpeg -v error -f rawvideo -pix_fmt yuv420p -s "$SIZE" -i "$T/ref.i420" -vf noise=alls=20 \
  -f rawvideo -pix_fmt yuv420p "$T/noisy.i420" || { echo "FAIL prerequisite: noise filter"; exit 1; }
mkyuv "$T/noisy.yuv" "$T/noisy.i420" 0 1 2
ffmpeg -v error -f rawvideo -pix_fmt yuv420p -s "$SIZE" -i "$T/noisy.i420" \
  -f rawvideo -pix_fmt yuv420p -s "$SIZE" -i "$T/ref.i420" \
  -lavfi "psnr=stats_file=$T/psnr.log" -f null - || { echo "FAIL prerequisite: ffmpeg psnr"; exit 1; }
ff_min=$(sed -n 's/.*psnr_avg:\([0-9.inf]*\).*/\1/p' "$T/psnr.log" | sort -g | head -1)
out=$("$C" "$T/noisy.yuv" "$T/ref.i420" - "$SIZE" --frames 3); rc=$?
echo "     $out"
ours=$(printf '%s' "$out" | sed -n 's/.*psnr_ref_min=\([0-9.inf]*\).*/\1/p')
if [ -n "$ff_min" ] && [ -n "$ours" ] && awk -v a="$ours" -v b="$ff_min" 'BEGIN { d = a - b; exit !(d <= 0.05 && d >= -0.05) }'; then
  ok "degraded copy: psnr_ref_min $ours within 0.05 dB of ffmpeg psnr $ff_min"
else
  bad "degraded copy: psnr" "ours '$ours', ffmpeg '$ff_min'"
fi
if [ "$rc" -eq 1 ]; then ok "degraded copy (below 45 dB): exit 1"; else bad "degraded copy: exit" "got $rc, expected 1"; fi

# 3. wrong frame order: every output frame is matched to the source frame it copies (exact 3/3 under --expect-exact)
mkyuv "$T/order.yuv" "$T/ref.i420" 2 0 1
out=$("$C" "$T/order.yuv" "$T/ref.i420" - "$SIZE" --frames 3 --expect-exact); rc=$?
echo "     $out"
if [ "$rc" -eq 0 ] && case $out in *"exact=3/3"*) true ;; *) false ;; esac; then
  ok "frames out of order (2 0 1): each matched to its source, exit 0"
else
  bad "frames out of order" "rc $rc: $out"
fi

# 4. --frames limits the frames read and printed
out=$("$C" "$T/same.yuv" "$T/ref.i420" - "$SIZE" --frames 2 --expect-exact); rc=$?
case $out in "frames=2 "*) [ "$rc" -eq 0 ] && ok "--frames 2: two frames read, exit 0" || bad "--frames 2: exit" "$rc" ;; *) bad "--frames 2" "$out" ;; esac

# 5. a stream without FRAME records is an error (exit 1), not a silent pass
printf 'YUV4MPEG2 W%d H%d F30:1 Ip A1:1 C420\nGARBAGE\n' "$W" "$H" >"$T/bad.yuv"
"$C" "$T/bad.yuv" "$T/ref.i420" - "$SIZE" >/dev/null 2>&1; rc=$?
if [ "$rc" -eq 1 ]; then ok "bad stream: exit 1"; else bad "bad stream: exit" "got $rc, expected 1"; fi

# 6. usage error: missing arguments -> exit 2
"$C" "$T/same.yuv" >/dev/null 2>&1; rc=$?
if [ "$rc" -eq 2 ]; then ok "missing arguments: exit 2"; else bad "usage: exit" "got $rc, expected 2"; fi

echo "SUMMARY: $PASS passed, $FAIL failed"
exit "$FAIL"
