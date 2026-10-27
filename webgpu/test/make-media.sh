#!/bin/bash
# Generate the test media for the WebGPU upscaler harness. Nothing is downloaded: the sources are images that ship with
# the OS (wallpapers, an SDDM theme, ImageMagick docs). Output goes to $1 (default $NVVSR_SCRATCH/media); never into the repo.
#
# quality clips : <name>_lr.mp4 = pan over a source image, cropped to the reference size, box-downscaled by 2
#                 (ffmpeg scale=area), H.264 yuv420p BT.709 limited, CRF 14; <name>_lrq.mp4 = bicubic downscale + CRF 27;
#                 <name>_lr133.mp4 / _lr300.mp4 = 1080p / 480p versions (1.33x and 3x to 1440p), bicubic, CRF 18.  Reference frames: <name>_ref_<n>.png
#                 (the same crop, lossless, RGB).  The upscaled LR frame is compared to the reference frame.
# throughput    : pixie_<480|720|1080>p<30|60>.mp4, 20 s of fast pan over the same image
# colour        : colour_<bt709|bt601|bt709pc|bt2020>.mp4 (flat patches, tagged) + colour_patches.png (expected RGB)
# misc          : rot90.mp4 (display-matrix rotation), clearkey.* (CENC-encrypted fMP4 for the EME test)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-${NVVSR_SCRATCH:-/tmp/nvvsr-webgpu}/media}
mkdir -p "$OUT"; cd "$OUT"
FF="ffmpeg -v error -y -nostdin"
X264="-c:v libx264 -preset slow -crf 14 -pix_fmt yuv420p -x264-params colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv -movflags +faststart"
FRAMES=(10 70 130 190 250 290)

# name|source|refW|refH|x-expr|y-expr   (n = frame number 0..299 at 30 fps; expressions contain no commas)
CLIPS=(
 "maldives|/usr/share/sddm/themes/maldives/background.jpg|2560|1440|16+16*sin(n/40)|n*314/300"
 "pixie|/usr/share/sddm/themes/pixie/assets/background.jpg|2560|1440|3600+n*4|1500+n"
 "lady|/usr/share/wallpapers/cachyos-wallpapers/BlueLady.png|2560|1440|n*4|360+n*2"
 "space|/usr/share/wallpapers/cachyos-wallpapers/CachyOS_GreenSpace.png|2560|1440|n*4|360"
 "north|/usr/share/wallpapers/cachyos-wallpapers/north.png|2560|1440|n*4|300+n"
 "mountain|/usr/share/doc/ImageMagick-7/www/image/mountains-clahe.jpg|1280|720|n*0.7|n*0.9"
)

for spec in "${CLIPS[@]}"; do
  IFS='|' read -r name src rw rh xe ye <<<"$spec"
  [ -f "$src" ] || { echo "missing source $src" >&2; exit 1; }
  lw=$((rw/2)); lh=$((rh/2))
  crop="format=rgb24,crop=$rw:$rh:'$xe':'$ye'"
  if [ ! -f "${name}_lr.mp4" ]; then
    $FF -loop 1 -framerate 30 -i "$src" -t 10 \
      -vf "$crop,scale=$lw:$lh:flags=area,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p" $X264 "${name}_lr.mp4"
  fi
  # realistic variant: bicubic downscale + lossy H.264 (CRF 27), like a streaming service would deliver
  if [ ! -f "${name}_lrq.mp4" ]; then
    $FF -loop 1 -framerate 30 -i "$src" -t 10 \
      -vf "$crop,scale=$lw:$lh:flags=bicubic,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p" ${X264/-preset slow -crf 14/-preset medium -crf 27} "${name}_lrq.mp4"
  fi
  if [ "$name" != mountain ]; then   # other scale factors: 1080p -> 1440p (1.33x) and 480p -> 1440p (3x), bicubic downscale, CRF 18
    for v in "lr133:1920:1080" "lr300:854:480"; do
      IFS=: read -r vn vw vh <<<"$v"
      [ -f "${name}_$vn.mp4" ] || $FF -loop 1 -framerate 30 -i "$src" -t 10 \
        -vf "$crop,scale=$vw:$vh:flags=bicubic,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p" ${X264/-preset slow -crf 14/-preset slow -crf 18} "${name}_$vn.mp4"
    done
  fi
  for k in "${FRAMES[@]}"; do
    [ -f "${name}_ref_$k.png" ] || $FF -loop 1 -framerate 30 -i "$src" -t 10 -vf "$crop,select='eq(n\,$k)'" -fps_mode passthrough -frames:v 1 -update 1 "${name}_ref_$k.png"
  done
  echo "quality clip $name: ${lw}x${lh} -> ${rw}x${rh}"
done

# --- throughput clips: same image, fast sinusoidal pan, 20 s -------------------------------------------------------------
SRC=/usr/share/sddm/themes/pixie/assets/background.jpg
for fps in 30 60; do
  div=$((fps*4))   # pan period scales with fps so the motion per second is the same
  for h in 480 720 1080; do
    w=$((h*16/9)); w=$((w/2*2))
    f="pixie_${h}p${fps}.mp4"
    [ -f "$f" ] || $FF -loop 1 -framerate $fps -i "$SRC" -t 20 \
      -vf "format=rgb24,crop=2560:1440:'3800+1200*sin(n/$div)':'1500+300*sin(n/$((div+50)))',scale=$w:$h:flags=area,scale=out_color_matrix=bt709:out_range=tv,format=yuv420p" \
      -c:v libx264 -preset veryfast -crf 20 -g $((fps*2)) -pix_fmt yuv420p -x264-params colorprim=bt709:transfer=bt709:colormatrix=bt709:range=tv -movflags +faststart "$f"
  done
done

# Note: only -x264-params writes primaries/transfer into the H.264 VUI; ffmpeg's -color_primaries/-color_trc did not, and Chrome then
# decodes BT.709 video with the BT.601 matrix. All clips are therefore tagged through -x264-params.
# --- colour patches (known RGB) tagged four ways -------------------------------------------------------------------------
PATCHES=(235,235,235 180,180,180 128,128,128 64,64,64 16,16,16 255,0,0 0,255,0 0,0,255 255,255,0 0,255,255 255,0,255 200,150,120 120,80,60 90,140,200 240,120,30 30,160,90)
if [ ! -f colour_patches.png ]; then   # 4x4 grid of 320x180 flat patches, generated directly in RGB
  graph=""; for i in "${!PATCHES[@]}"; do IFS=, read -r r g b <<<"${PATCHES[$i]}"; graph+="$(printf 'color=c=0x%02x%02x%02x:s=320x180:d=1,format=rgb24[p%d];' "$r" "$g" "$b" "$i")"; done
  for row in 0 1 2 3; do graph+="[p$((row*4))][p$((row*4+1))][p$((row*4+2))][p$((row*4+3))]hstack=4[r$row];"; done
  $FF -filter_complex "${graph}[r0][r1][r2][r3]vstack=4" -frames:v 1 colour_patches.png
  printf '%s\n' "${PATCHES[@]}" > colour_patches.txt
fi
mk() { # name matrix-for-conversion tag-matrix tag-prim tag-trc range
  [ -f "colour_$1.mp4" ] || $FF -loop 1 -framerate 30 -i colour_patches.png -t 2 \
    -vf "scale=out_color_matrix=$2:out_range=$6,format=yuv420p" -c:v libx264 -preset fast -crf 10 -pix_fmt yuv420p \
    -x264-params colorprim=$4:transfer=$5:colormatrix=$3:range=$6 "colour_$1.mp4"
}
mk bt709   bt709  bt709     bt709     bt709     tv
mk bt601   bt601  smpte170m smpte170m smpte170m tv
mk bt709pc bt709  bt709     bt709     bt709     pc
mk bt2020  bt2020 bt2020nc  bt2020    bt2020-10 tv

# --- rotated video (display matrix) -----------------------------------------------------------------------------------
[ -f rot90.mp4 ] || ffmpeg -v error -y -nostdin -display_rotation:v 90 -i colour_bt709.mp4 -c copy rot90.mp4

# --- ClearKey (CENC) encrypted fragmented MP4 for the EME test (the key is public test data) --------------------------------
KEY=00112233445566778899aabbccddeeff; KID=a0a1a2a3a4a5a6a7a8a9aaabacadaeaf
[ -f clearkey.mp4 ] || ffmpeg -v error -y -nostdin -i pixie_720p30.mp4 -t 6 -c copy -encryption_scheme cenc-aes-ctr \
  -encryption_key $KEY -encryption_kid $KID -movflags frag_keyframe+empty_moov+default_base_moof clearkey.mp4
echo "$KID $KEY" > clearkey.key
IFS=, read -r prof lvl < <(ffprobe -v error -select_streams v:0 -show_entries stream=profile,level -of csv=p=0 clearkey.mp4)   # e.g. "High,31"
case $prof in Baseline|"Constrained Baseline") pp=42;; Main) pp=4d;; *) pp=64;; esac
printf 'avc1.%s00%02x\n' $pp "$lvl" > clearkey.codec
ls -la "$OUT" | head -60
