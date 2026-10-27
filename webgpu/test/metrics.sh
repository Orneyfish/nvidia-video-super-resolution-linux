#!/bin/bash
# PSNR / SSIM of the upscaled canvas captures against the lossless reference frames, with ffmpeg's psnr and ssim filters.
# usage: metrics.sh OUT_DIR MEDIA_DIR    (reads OUT_DIR/quality-spec.txt written by the quality suite; markdown tables on stdout,
#                                         per-frame numbers in OUT_DIR/quality.tsv)
#   OUT_DIR/q_<name>.<variant>_<mode>_<frame>.png   captures from the harness
#   MEDIA_DIR/<name>_ref_<frame>.png                references from make-media.sh
# RGB PSNR is ffmpeg's "average" (PSNR of the mean MSE over R, G, B); SSIM is ffmpeg's "All" (mean of the R, G, B planes).
set -eu
OUT=$1; MEDIA=$2
# shellcheck source=/dev/null
. "$OUT/quality-spec.txt"          # CONTENTS="maldives.lr ..." MODES="bilinear fsr ..." FRAMES="10 70 ..."

one() {
  local c=$1 m=$2 k=$3 ref up p s
  ref="$MEDIA/${c%%.*}_ref_$k.png"; up="$OUT/q_${c}_${m}_$k.png"
  p=$(ffmpeg -hide_banner -nostdin -i "$ref" -i "$up" -lavfi "[0:v][1:v]psnr" -f null - 2>&1 | sed -n 's/.*average:\([0-9.]*\).*/\1/p')
  s=$(ffmpeg -hide_banner -nostdin -i "$ref" -i "$up" -lavfi "[0:v][1:v]ssim" -f null - 2>&1 | sed -n 's/.* All:\([0-9.]*\).*/\1/p')
  [ -n "$p" ] && [ -n "$s" ] || { echo "metrics failed for $up" >&2; exit 1; }
  printf '%s\t%s\t%s\t%s\t%s\n' "$c" "$m" "$k" "$p" "$s"
}
export -f one; export OUT MEDIA

for c in $CONTENTS; do for m in $MODES; do for k in $FRAMES; do echo "$c $m $k"; done; done; done |
  xargs -P 8 -L 1 bash -c 'one $0 $1 $2' > "$OUT/quality.tsv"

VARIANTS=$(for c in $CONTENTS; do echo "${c#*.}"; done | awk '!s[$0]++' | tr '\n' ' ')
NAMES=$(for c in $CONTENTS; do echo "${c%%.*}"; done | awk '!s[$0]++' | tr '\n' ' ')
# one table per variant: rows = modes, columns = contents, cells = "PSNR dB / SSIM" (mean over the frames), last column = mean over contents
awk -F'\t' -v modes="$MODES" -v names="$NAMES" -v variants="$VARIANTS" '
  { split($1, a, "."); k = a[2] SUBSEP $2 SUBSEP a[1]; P[k] += $4; S[k] += $5; N[k]++; v = a[2] SUBSEP $2; PV[v] += $4; SV[v] += $5; NV[v]++ }
  END {
    nm = split(modes, M, " "); nn = split(names, C, " "); nv = split(variants, V, " ")
    for (vi = 1; vi <= nv; vi++) {
      printf "\nvariant %s\n\n| mode |", V[vi]; for (ci = 1; ci <= nn; ci++) if ((V[vi] SUBSEP M[1] SUBSEP C[ci]) in N) printf " %s |", C[ci]; printf " mean |\n|---|"
      for (ci = 1; ci <= nn; ci++) if ((V[vi] SUBSEP M[1] SUBSEP C[ci]) in N) printf "---|"; printf "---|\n"
      for (mi = 1; mi <= nm; mi++) {
        printf "| %s |", M[mi]
        for (ci = 1; ci <= nn; ci++) { k = V[vi] SUBSEP M[mi] SUBSEP C[ci]; if (k in N) printf " %.2f / %.4f |", P[k] / N[k], S[k] / N[k] }
        v = V[vi] SUBSEP M[mi]; printf " %.2f / %.4f |\n", PV[v] / NV[v], SV[v] / NV[v]
      }
    }
  }' "$OUT/quality.tsv"
