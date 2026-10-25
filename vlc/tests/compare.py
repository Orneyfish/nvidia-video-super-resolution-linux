#!/usr/bin/env python3
"""Compares the frames VLC wrote through the nvvsr module (the `yuv` video output: a header line, then
"FRAME\\n" + planar frame per picture) with a reference made from the same source frames.

  compare.py OUT.yuv REF.i420 BIC.i420 WxH [--frames N] [--expect-exact]

REF.i420 is the libnvvsr result for the first source frames (nvvsr_vlc_ref), BIC.i420 a bicubic upscale of the same
frames (or `-` to compare with REF only). For every checked output frame the matching source frame is found by the best luma PSNR against REF. Prints one
line, e.g. `frames=30 size=2560x1440 exact=30/30 psnr_ref=inf psnr_bicubic=41.2 ssim_luma=0.94`, and exits 0 when the
frames match REF (exactly with --expect-exact, otherwise PSNR >= 45 dB) and are closer to REF than a different
frame would be. Run it with `python -I`.
"""
import sys

import numpy as np


def read_stream(path, w, h, limit):
    fsize = w * h * 3 // 2
    frames = []
    with open(path, "rb") as f:
        header = f.readline().decode("ascii", "replace")
        while len(frames) < limit:
            tag = f.readline()
            if not tag:
                break
            if tag != b"FRAME\n":
                raise SystemExit("bad stream: expected FRAME, got %r" % tag[:16])
            buf = f.read(fsize)
            if len(buf) != fsize:
                break
            frames.append(np.frombuffer(buf, dtype=np.uint8))
    return header, frames


def load_raw(path, w, h):
    fsize = w * h * 3 // 2
    data = np.memmap(path, dtype=np.uint8, mode="r")
    return [data[i * fsize:(i + 1) * fsize] for i in range(len(data) // fsize)]


def psnr(a, b):
    d = a.astype(np.int32) - b.astype(np.int32)
    mse = float(np.mean(d * d))
    return float("inf") if mse == 0 else 10.0 * np.log10(255.0 * 255.0 / mse)


def ssim_luma(a, b, w, h):
    """Mean SSIM of the luma plane on 8x8 blocks (a cheap version, enough to compare two upscales)."""
    ya = a[: w * h].reshape(h, w).astype(np.float64)
    yb = b[: w * h].reshape(h, w).astype(np.float64)
    h8, w8 = h // 8 * 8, w // 8 * 8
    ya = ya[:h8, :w8].reshape(h8 // 8, 8, w8 // 8, 8).transpose(0, 2, 1, 3).reshape(-1, 64)
    yb = yb[:h8, :w8].reshape(h8 // 8, 8, w8 // 8, 8).transpose(0, 2, 1, 3).reshape(-1, 64)
    ma, mb = ya.mean(1), yb.mean(1)
    va, vb = ya.var(1), yb.var(1)
    cov = ((ya - ma[:, None]) * (yb - mb[:, None])).mean(1)
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma ** 2 + mb ** 2 + c1) * (va + vb + c2))
    return float(s.mean())


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("ref")
    ap.add_argument("bic")
    ap.add_argument("size")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--expect-exact", action="store_true")
    a = ap.parse_args()
    out_path, ref_path, bic_path = a.out, a.ref, a.bic
    w, h = (int(x) for x in a.size.split("x"))
    limit = a.frames
    exact_wanted = a.expect_exact

    header, frames = read_stream(out_path, w, h, limit)
    if not frames:
        print("frames=0 header=%r" % header.strip())
        return 1
    ref = load_raw(ref_path, w, h)
    bic = load_raw(bic_path, w, h) if bic_path != "-" else ref
    n = min(len(ref), len(bic))
    exact = 0
    worst_ref = float("inf")
    psnr_bic = []
    ssim_out_bic = []
    ssim_out_ref = []
    bad = 0
    for fr in frames:
        # matching source frame: best luma PSNR against the SR reference (luma only, every 2nd row for speed)
        best, best_i = -1.0, -1
        ya = fr[: w * h].reshape(h, w)[::2]
        for i in range(n):
            p = psnr(ya, ref[i][: w * h].reshape(h, w)[::2])
            if p > best:
                best, best_i = p, i
        full = psnr(fr, ref[best_i])
        if full == float("inf"):
            exact += 1
        worst_ref = min(worst_ref, full)
        psnr_bic.append(psnr(fr, bic[best_i]))
        ssim_out_bic.append(ssim_luma(fr, bic[best_i], w, h))
        ssim_out_ref.append(ssim_luma(fr, ref[best_i], w, h))
        if (exact_wanted and full != float("inf")) or full < 45.0:
            bad += 1
    print("frames=%d header=%r size=%dx%d exact=%d/%d psnr_ref_min=%s psnr_bicubic_mean=%.2f ssim_vs_bicubic=%.4f ssim_vs_ref=%.4f"
          % (len(frames), header.strip(), w, h, exact, len(frames),
             "inf" if worst_ref == float("inf") else "%.2f" % worst_ref, float(np.mean(psnr_bic)),
             float(np.mean(ssim_out_bic)), float(np.mean(ssim_out_ref))))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
