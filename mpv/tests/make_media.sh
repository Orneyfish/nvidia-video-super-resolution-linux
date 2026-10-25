#!/usr/bin/env bash
# Generates the synthetic test videos of mpv/tests/run.sh with ffmpeg: make_media.sh DIR
# Files already present are kept. Everything is a few hundred kB at most.
set -eu
DIR=${1:?usage: make_media.sh DIR}
mkdir -p "$DIR"
cd "$DIR"
F="ffmpeg -v error -y -nostdin"
X264="-c:v libx264 -preset veryfast"
X265="-c:v libx265 -preset veryfast"
SRC() { echo "-f lavfi -i testsrc2=size=$1:rate=$2"; }

gen() {  # NAME ffmpeg-args...: build NAME unless it exists
  local name=$1; shift
  [ -s "$name" ] || { $F "$@" "$name.part.${name##*.}" && mv "$name.part.${name##*.}" "$name"; }
}

gen v720p30.mp4    $(SRC 1280x720 30)    -t 20 $X264 -pix_fmt yuv420p -g 60 &
gen v1080p60.mp4   $(SRC 1920x1080 60)   -t 20 $X264 -pix_fmt yuv420p -g 120 &
gen long720p30.mp4 $(SRC 1280x720 30)    -t 100 $X264 -pix_fmt yuv420p -g 60 &
gen v480p.mp4      $(SRC 854x480 30)     -t 8  $X264 -pix_fmt yuv420p &
gen v240p.mp4      $(SRC 320x240 30)     -t 8  $X264 -pix_fmt yuv420p &
gen v1440p.mp4     $(SRC 2560x1440 30)   -t 6  $X264 -pix_fmt yuv420p &
gen v720p10b_hevc.mp4 $(SRC 1280x720 30) -t 8  $X265 -pix_fmt yuv420p10le -x265-params log-level=error &
gen v720p10b_h264.mp4 $(SRC 1280x720 30) -t 8  $X264 -pix_fmt yuv420p10le &
gen v720p444.mp4   $(SRC 1280x720 30)    -t 8  $X264 -pix_fmt yuv444p &
# HDR (PQ, BT.2020), HLG, and an SDR stream that only has BT.2020 primaries
gen hdr_pq.mp4     $(SRC 1280x720 30)    -t 8  $X265 -pix_fmt yuv420p10le -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
                     -x265-params log-level=error:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc &
gen hdr_hlg.mp4    $(SRC 1280x720 30)    -t 8  $X265 -pix_fmt yuv420p10le \
                     -x265-params log-level=error:colorprim=bt2020:transfer=arib-std-b67:colormatrix=bt2020nc &
gen sdr_bt2020.mp4 $(SRC 1280x720 30)    -t 8  $X264 -pix_fmt yuv420p -color_primaries bt2020 -colorspace bt2020nc -color_trc bt709 &
# interlaced (top field first)
gen v1080i.mp4     $(SRC 1920x1080 30000/1001) -t 8 -vf interlace=scan=tff $X264 -pix_fmt yuv420p -flags +ilme+ildct -x264-params tff=1 &
gen v576i.mp4      $(SRC 720x576 25)     -t 8  -vf interlace=scan=tff $X264 -pix_fmt yuv420p -flags +ilme+ildct -x264-params tff=1 &
gen audio.m4a      -f lavfi -i sine=frequency=440 -t 8 -c:a aac &
gen still.png      $(SRC 1280x720 1) -frames:v 1 &
wait
gen art.m4a        -i audio.m4a -i still.png -map 0:a -map 1:v -c copy -disposition:v attached_pic
