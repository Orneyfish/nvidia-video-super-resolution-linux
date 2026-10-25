#!/usr/bin/env bash
# Tests of mpv/vsr.lua. Prints PASS/FAIL per check; the exit status is the number of failed checks.
#
#   mpv/tests/run.sh               unit tests (LuaJIT) + headless integration tests in the private mpv (uses the GPU)
#   mpv/tests/run.sh --unit        only the unit tests (no mpv, no GPU)
#   mpv/tests/run.sh --bench       only the real-time playback numbers (720p30, 1080p60; needs an idle machine)
#   mpv/tests/run.sh --only PAT..  only integration scenarios whose tag contains PAT (class, window, fullscreen,
#                                  hwdec, keys, fail, path, power, adaptive, lifecycle, or a file name)
#
# Environment (all optional): NVVSR_LIBDIR (default: the repo's .venv wheel directories), NVVSR_PLUGIN (default
# vapoursynth/build/libvsnvvsr.so, built when missing), NVVSR_MPV (default ~/.local/opt/mpv-vsr/bin/mpv),
# NVVSR_TEST_TMP (scratch directory, kept; default a temporary one; the test videos are generated there).
# Nothing is written outside the scratch directory: mpv runs with --no-config (or a scratch --config-dir), headless
# (--vo=null, no window), and VapourSynth gets a scratch XDG_CONFIG_HOME. The GPU tests refuse to run while
# another process uses the GPU or an Ollama model is resident.
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
MODE=all
ONLY=()
while [ $# -gt 0 ]; do
  case $1 in
    --unit) MODE=unit ;;
    --bench) MODE=bench ;;
    --only) shift; while [ $# -gt 0 ] && [[ $1 != --* ]]; do ONLY+=("$1"); shift; done; continue ;;
    *) echo "usage: $0 [--unit | --bench] [--only PATTERN...]" >&2; exit 2 ;;
  esac
  shift
done

PASS=0; FAIL=0
count() {  # reads PASS/FAIL lines on stdin, echoes them, updates the counters in the caller via files
  local line
  while IFS= read -r line; do
    case $line in SUMMARY*) continue ;; esac
    echo "$line"
    case $line in PASS*) echo . >>"$CNT.pass" ;; FAIL*) echo . >>"$CNT.fail" ;; esac
  done
}

if [ -n "${NVVSR_TEST_TMP:-}" ]; then
  SCR=$NVVSR_TEST_TMP; mkdir -p "$SCR"
else
  SCR=$(mktemp -d); trap 'rm -rf "$SCR"' EXIT
fi
CNT=$SCR/.count; : >"$CNT.pass"; : >"$CNT.fail"

echo "== 0. unit tests (pure logic, LuaJIT)"
luajit "$HERE/test_logic.lua" | count
if [ $MODE = unit ]; then
  P=$(wc -l <"$CNT.pass"); F=$(wc -l <"$CNT.fail")
  echo; echo "SUMMARY: $P passed, $F failed"; exit "$F"
fi

# ---- environment for the integration tests -------------------------------------------------------
if [ -z "${NVVSR_LIBDIR:-}" ]; then
  sp=$(ls -d "$REPO"/.venv/lib/python3*/site-packages 2>/dev/null | head -1)
  [ -n "$sp" ] && export NVVSR_LIBDIR="$sp/nvvfx/libs:$sp/nvidia_vfx.libs"
fi
MPV=${NVVSR_MPV:-$HOME/.local/opt/mpv-vsr/bin/mpv}
if ! "$MPV" -v --no-config --idle=no 2>&1 | grep -q "List of enabled features:.* vapoursynth "; then
  echo "SKIP integration tests: $MPV missing or built without vapoursynth"; exit 0
fi
busy=$(nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv,noheader 2>/dev/null)
if [ -n "$busy" ]; then echo "SKIP integration tests: another process uses the GPU: $busy"; exit 0; fi
if command -v ollama >/dev/null 2>&1; then
  resident=$(ollama ps 2>/dev/null | tail -n +2)
  if [ -n "$resident" ]; then echo "SKIP integration tests: an Ollama model is resident: $resident"; exit 0; fi
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
if [ -z "${NVVSR_PLUGIN:-}" ]; then
  cmake -S "$REPO/vapoursynth" -B "$REPO/vapoursynth/build" -DCMAKE_BUILD_TYPE=Release >/dev/null &&
    cmake --build "$REPO/vapoursynth/build" -j >/dev/null || { echo "FAIL build: cannot build the plugin"; exit 1; }
  export NVVSR_PLUGIN=$REPO/vapoursynth/build/libvsnvvsr.so
fi
# plugin build whose frames fail after N (env NVVSR_VS_FAIL_AFTER), in the scratch directory
HOOK=$SCR/hook-build
if cmake -S "$REPO/vapoursynth" -B "$HOOK" -DNVVSR_VS_TESTHOOK=ON -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 &&
   cmake --build "$HOOK" -j >/dev/null 2>&1; then
  export T_HOOK_PLUGIN=$HOOK/libvsnvvsr.so
else
  echo "note: cannot build the test-hook plugin, those cases are skipped"
fi

MEDIA=$SCR/media
bash "$HERE/make_media.sh" "$MEDIA" || { echo "FAIL media: ffmpeg could not generate the test videos"; exit 1; }

export T_REPO=$REPO T_SCR=$SCR T_MPV=$MPV T_MEDIA=$MEDIA
cd "$SCR" || exit 1
if [ $MODE = bench ]; then
  python3 -I -B "$HERE/integration.py" --bench
  exit 0
fi
echo "== integration tests"
python3 -I -B "$HERE/integration.py" "${ONLY[@]}" | count
P=$(wc -l <"$CNT.pass"); F=$(wc -l <"$CNT.fail")
echo; echo "SUMMARY: $P passed, $F failed (unit + integration)"
exit "$F"
