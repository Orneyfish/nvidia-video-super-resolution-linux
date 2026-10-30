// Post-upscale sharpening of the luma plane, in place (used by nvvsr_webd, timed and checked by tests/test_webd.cpp).
// Unsharp mask with a 3x3 box blur: Y' = clamp(Y + a*(Y - box3x3(Y)), lo, hi), a = level*0.15. In integers, with
// d = 9*Y - sum3x3: Y' = Y + floor((level*d + 30) / 60). The 1-pixel border is unchanged. Rows are split over up to 4 threads.
#pragma once
#include <algorithm>
#include <cstdint>
#include <exception>
#include <thread>
#include <vector>

// Rows y0..y1-1 of src (an unmodified copy of the plane) into dst; vs is a scratch row of w uint16.
inline void sharpen_band(const uint8_t *__restrict src, uint8_t *__restrict dst, int w, int y0, int y1, int level, int lo, int hi,
                         uint16_t *__restrict vs) {
  for (int y = y0; y < y1; y++) {
    const uint8_t *a = src + (size_t)(y - 1) * w, *b = a + w, *c = b + w;
    uint8_t *o = dst + (size_t)y * w;
    for (int x = 0; x < w; x++) vs[x] = a[x] + b[x] + c[x];
    for (int x = 1; x < w - 1; x++) {
      const int d = 9 * b[x] - (vs[x - 1] + vs[x] + vs[x + 1]);
      const unsigned n = level * d + 18030;  // level*d + 30 + 60*300, so the unsigned division is a floor and n >= 0
      o[x] = std::min(std::max(b[x] + (int)(n / 60) - 300, lo), hi);
    }
  }
}

// y: w*h luma bytes (modified); level 1..7 (0 = no-op); tmp: scratch the caller keeps to avoid an allocation per frame.
inline void sharpen_luma(uint8_t *y, int w, int h, int level, bool full_range, std::vector<uint8_t> &tmp) {
  if (level <= 0 || w < 3 || h < 3) return;
  tmp.assign(y, y + (size_t)w * h);
  const int lo = full_range ? 0 : 16, hi = full_range ? 255 : 235;
  const int nt = std::max(1, std::min(4, w * h >> 19));
  std::vector<uint16_t> vs((size_t)w * nt);  // allocated here so the band threads cannot throw
  auto run = [&](int t) {
    sharpen_band(tmp.data(), y, w, 1 + (h - 2) * t / nt, 1 + (h - 2) * (t + 1) / nt, level, lo, hi, vs.data() + (size_t)w * t);
  };
  std::vector<std::thread> th;
  th.reserve(nt);
  for (int t = 1; t < nt; t++) {
    try { th.emplace_back(run, t); } catch (const std::exception &) { run(t); }  // no thread available: do the band here
  }
  run(0);
  for (std::thread &t : th) t.join();
}
