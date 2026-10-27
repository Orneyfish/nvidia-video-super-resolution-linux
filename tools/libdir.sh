#!/bin/bash
# Prints NVVSR_LIBDIR for a development checkout: the two library directories of the unpacked nvidia-vfx wheel.
# usage: export NVVSR_LIBDIR=$(tools/libdir.sh)
root=$(cd "$(dirname "$0")/.." && pwd)/third_party/nvidia-vfx
[ -d "$root/nvvfx/libs" ] || { echo "libdir.sh: $root is missing; run tools/fetch-nvidia-libs.sh" >&2; exit 1; }
echo "$root/nvvfx/libs:$root/nvidia_vfx.libs"
