#!/bin/bash
# Static checks of the plugin binary (readelf/nm only, no GPU, no VapourSynth): port of section 1 of run.sh
# ("plugin loads, signature, linking"), with the function signatures read from the binary instead of through Python.
#   test_linking.sh [PLUGIN.so]        (default: $NVVSR_PLUGIN)
# Exit status: 0 when every check passed.
set -u

PLUGIN=${1:-${NVVSR_PLUGIN:-}}
if [ ! -f "$PLUGIN" ]; then echo "FAIL plugin file: '$PLUGIN' does not exist (pass it as the argument or set NVVSR_PLUGIN)"; exit 1; fi

CHECKS=0; FAILS=0
ok()  { echo "PASS $1"; CHECKS=$((CHECKS + 1)); }
bad() { echo "FAIL $1: $2"; CHECKS=$((CHECKS + 1)); FAILS=$((FAILS + 1)); }
check_eq() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "got '$2', expected '$3'"; fi; }

# The plugin's exports: VapourSynthPluginInit2 and nothing else.
syms=$(nm -D --defined-only "$PLUGIN" | awk '{print $3}' | tr '\n' ' ')
check_eq "only VapourSynthPluginInit2 is exported" "$syms" "VapourSynthPluginInit2 "

# Linking: libnvvsr by its soname, found through RUNPATH (not RPATH, not LD_LIBRARY_PATH).
dyn=$(readelf -d "$PLUGIN")
case $dyn in *"Shared library: [libnvvsr.so.0]"*) ok "libnvvsr.so.0 is a NEEDED library" ;; *) bad "NEEDED libnvvsr.so.0" "$(printf '%s\n' "$dyn" | grep NEEDED)" ;; esac
rp=$(printf '%s\n' "$dyn" | sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p')
if [ -z "$rp" ]; then bad "RUNPATH is present" "no RUNPATH entry in the dynamic section"; else ok "RUNPATH is present ($rp)"; fi
case $rp in
  *: | :* | *::*) bad "RUNPATH has no empty (current directory) entry" "$rp" ;;
  "") bad "RUNPATH has no empty (current directory) entry" "no RUNPATH" ;;
  *) ok "RUNPATH has no empty (current directory) entry" ;;
esac
case ":$rp:" in *:'$ORIGIN':*) ok "RUNPATH has \$ORIGIN (an installed copy finds libnvvsr.so.0 next to the plugin)" ;; *) bad "RUNPATH has \$ORIGIN" "$rp" ;; esac
case $dyn in *"(RPATH)"*) bad "no legacy RPATH (it would override LD_LIBRARY_PATH)" "$(printf '%s\n' "$dyn" | grep RPATH)" ;; *) ok "no legacy RPATH" ;; esac
lddout=$(env -u LD_LIBRARY_PATH ldd "$PLUGIN" 2>&1 | grep libnvvsr)
case $lddout in
  *"not found"*) bad "libnvvsr.so.0 resolves via RUNPATH without LD_LIBRARY_PATH" "$lddout" ;;
  *"/libnvvsr.so"*) ok "libnvvsr.so.0 resolves via RUNPATH without LD_LIBRARY_PATH" ;;
  *) bad "libnvvsr.so.0 resolves via RUNPATH without LD_LIBRARY_PATH" "ldd says: '$lddout'" ;;
esac

# Registration: the identifier, namespace, function names and the argument lists the docs promise are string
# constants of the binary (each is checked as a whole string, so a missing or extra argument fails).
strs=$(readelf -p .rodata "$PLUGIN" | sed -n 's/^ *\[ *[0-9a-f]*\]  //p')
has() { printf '%s\n' "$strs" | grep -qxF -- "$1"; }
has "com.nvvsr.upscale" && has "nvvsr" && ok "plugin identifier com.nvvsr.upscale, namespace nvvsr" || bad "plugin identifier" "com.nvvsr.upscale / nvvsr not found in .rodata"
has "Upscale" && ok "function Upscale is registered" || bad "function Upscale" "name not found in .rodata"
has "clip:vnode;width:int:opt;height:int:opt;quality:data:opt;strength:float:opt;matrix:data:opt;range:int:opt;pool:int:opt;fps:float:opt;format:data:opt;" &&
  ok "Upscale(clip, width, height, quality, strength, matrix, range, pool, fps, format) signature" || bad "Upscale signature" "argument list not found in .rodata"
has "Auto" && ok "function Auto is registered" || bad "function Auto" "name not found in .rodata"
has "clip:vnode;display_w:int:opt;display_h:int:opt;dar_w:int:opt;dar_h:int:opt;fps:float:opt;options:data:opt;" &&
  ok "Auto(clip, display_w, display_h, dar_w, dar_h, fps, options) signature" || bad "Auto signature" "argument list not found in .rodata"
has "clip:vnode;" && ok "both functions return clip:vnode;" || bad "return type" "clip:vnode; not found in .rodata"

echo "test_linking: $CHECKS checks, $FAILS failed"
[ "$FAILS" -eq 0 ]
