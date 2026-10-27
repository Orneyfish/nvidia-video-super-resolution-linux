#!/bin/bash
# Bash driver for the mpv integration groups (mpv/tests/groups/*.sh, helpers in lib.sh). Prints PASS/FAIL per check.
#
#   run_groups.sh [--only GROUP...]     groups: the file names in groups/ (default: all)
#
# Environment (optional): NVVSR_TEST_TMP (scratch directory, kept; default $SCR/mpv-groups), NVVSR_LIBDIR (default:
# tools/libdir.sh), NVVSR_PLUGIN (default vapoursynth/build/libvsnvvsr.so), NVVSR_MPV (default
# ~/.local/opt/mpv-vsr/bin/mpv), NVVSR_ONLY_TAGS (space-separated check tags, see lib.sh wanted()).
# Exit status: the number of failed checks (capped at 255). Skips, with exit 0, while another process uses the GPU
# or an Ollama model is resident.
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
ONLY=()
while [ $# -gt 0 ]; do
    case $1 in
        --only) shift; while [ $# -gt 0 ] && [[ $1 != --* ]]; do ONLY+=("$1"); shift; done; continue ;;
        *) echo "usage: $0 [--only GROUP...]" >&2; exit 2 ;;
    esac
    shift
done

T_SCR=${NVVSR_TEST_TMP:-${SCR:-$(mktemp -d)}/mpv-groups}
mkdir -p "$T_SCR" || exit 1
if [ -z "${NVVSR_LIBDIR:-}" ]; then NVVSR_LIBDIR=$("$REPO/tools/libdir.sh") || exit 1; fi
export NVVSR_LIBDIR
export NVVSR_PLUGIN=${NVVSR_PLUGIN:-$REPO/vapoursynth/build/libvsnvvsr.so}
[ -f "$NVVSR_PLUGIN" ] || { echo "FAIL build: $NVVSR_PLUGIN missing, build vapoursynth/ first"; exit 1; }
T_MPV=${NVVSR_MPV:-$HOME/.local/opt/mpv-vsr/bin/mpv}
busy=$(nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader 2>/dev/null)
if [ -n "$busy" ]; then echo "SKIP mpv groups: another process uses the GPU: $busy"; exit 0; fi
if command -v ollama >/dev/null 2>&1; then
    resident=$(ollama ps 2>/dev/null | tail -n +2)
    if [ -n "$resident" ]; then echo "SKIP mpv groups: an Ollama model is resident: $resident"; exit 0; fi
fi

# VapourSynth finds its libvsscript entry in a scratch XDG config (same setup as run.sh)
export XDG_CONFIG_HOME=$T_SCR/xdg
mkdir -p "$XDG_CONFIG_HOME"
vapoursynth config >/dev/null 2>&1
CFG=$XDG_CONFIG_HOME/vapoursynth/vapoursynth.toml
sed 's|^"[^"]*"|"/usr/lib/libvsscript.so"|' "$CFG" > "$CFG.libvsscript" && cat "$CFG.libvsscript" >> "$CFG" && rm "$CFG.libvsscript"

export T_REPO=$REPO T_SCR T_MPV
export T_MEDIA=$T_SCR/media
bash "$HERE/make_media.sh" "$T_MEDIA" || { echo "FAIL media: ffmpeg could not generate the test videos"; exit 1; }
export MPVCTL=$T_SCR/mpvctl
g++ -std=c++17 -O2 -Wall -Wextra -o "$MPVCTL" "$HERE/mpvctl.cpp" || { echo "FAIL build: mpvctl"; exit 1; }

. "$HERE/lib.sh"
for g in "$HERE"/groups/*.sh; do . "$g"; done
if [ ${#ONLY[@]} -eq 0 ]; then
    for g in "$HERE"/groups/*.sh; do ONLY+=("$(basename "$g" .sh)"); done
fi
cd "$T_SCR" || exit 1
for g in "${ONLY[@]}"; do
    declare -F "group_$g" >/dev/null || { echo "no such group: $g (groups/$g.sh)" >&2; exit 2; }
    "group_$g"
done

echo; echo "SUMMARY: $PASSED passed, $FAILED failed"
exit $(( FAILED > 255 ? 255 : FAILED ))
