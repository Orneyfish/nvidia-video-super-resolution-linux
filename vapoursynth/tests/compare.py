"""Compare frames of a VapourSynth script's output 0 with a raw planar reference file.

  python -I compare.py script.vpy ref.raw [max_abs_diff]

The reference holds the frames as written by nvvsr_vs_ref (planes back to back, no row padding,
plane order as in VapourSynth: R,G,B or Y,U,V). Prints one summary line; exit status 0 = match.
"""
import runpy
import sys

import numpy as np
import vapoursynth as vs

script, ref_path = sys.argv[1], sys.argv[2]
tol = int(sys.argv[3]) if len(sys.argv) > 3 else 0

runpy.run_path(script)
clip = vs.get_outputs()[0].clip
ref = np.fromfile(ref_path, dtype=np.uint8)
pos = 0
worst = 0
for n in range(clip.num_frames):
    frame = clip.get_frame(n)
    for p in range(frame.format.num_planes):
        plane = np.asarray(frame[p])  # honours the row stride
        want = ref[pos:pos + plane.size].reshape(plane.shape)
        pos += plane.size
        worst = max(worst, int(np.abs(plane.astype(np.int16) - want.astype(np.int16)).max()))
ok = worst <= tol and pos == ref.size
print("frames=%d max_abs_diff=%d (tolerance %d) bytes_compared=%d/%d" % (clip.num_frames, worst, tol, pos, ref.size))
sys.exit(0 if ok else 1)
