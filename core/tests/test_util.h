// Shared helpers for the libnvvsr tests (header-only; uses only the public nvvsr.h).
#pragma once

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nvvsr.h"

namespace tu {

// ---------------------------------------------------------------------------
// test bookkeeping
// ---------------------------------------------------------------------------
inline int &failures() {
  static int n = 0;
  return n;
}

#define T_CHECK(cond, ...)                                              \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ++tu::failures();                                                 \
      std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);   \
      std::printf(__VA_ARGS__);                                         \
      std::printf("\n");                                                \
      std::fflush(stdout);                                              \
    }                                                                   \
  } while (0)

#define T_CHECK_OK(expr)                                                              \
  do {                                                                                \
    int rc_ = (expr);                                                                 \
    if (rc_ != NVVSR_OK) {                                                            \
      ++tu::failures();                                                               \
      std::printf("  FAIL %s:%d: %s -> %d (%s) [%s]\n", __FILE__, __LINE__, #expr, rc_, \
                  nvvsr_strerror(rc_), nvvsr_last_error());                           \
      std::fflush(stdout);                                                            \
    }                                                                                 \
  } while (0)

// Exit code convention: 0 pass, 1 fail, 77 skip (ctest SKIP_RETURN_CODE).
inline int finish(const char *name) {
  std::printf("%s: %s\n", name, failures() ? "FAILED" : "passed");
  return failures() ? 1 : 0;
}

// Returns 0 if SR is usable, else prints the reason and returns 77 for main() to return.
inline int skip_unless_usable(const char *name) {
  int rc = nvvsr_probe();
  if (rc == NVVSR_OK) return 0;
  std::printf("%s: SKIP: SR unavailable: %s (%s)\n", name, nvvsr_strerror(rc), nvvsr_last_error());
  return 77;
}

inline double now_ms() {
  using C = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(C::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// planes / frames
// ---------------------------------------------------------------------------
struct Plane {
  int w = 0, h = 0;
  std::vector<uint8_t> d;
  Plane() = default;
  Plane(int w_, int h_, uint8_t v = 0) : w(w_), h(h_), d((size_t)w_ * h_, v) {}
  uint8_t &at(int x, int y) { return d[(size_t)y * w + x]; }
  uint8_t at(int x, int y) const { return d[(size_t)y * w + x]; }
};

struct Yuv420 {
  int w = 0, h = 0;
  Plane y, u, v;
  Yuv420() = default;
  Yuv420(int w_, int h_, uint8_t yv = 0, uint8_t uv = 128, uint8_t vv = 128)
      : w(w_), h(h_), y(w_, h_, yv), u(w_ / 2, h_ / 2, uv), v(w_ / 2, h_ / 2, vv) {}
};

// Input frame description with stride padding, so the tests also cover non-tight strides.
struct InFrame {
  std::vector<uint8_t> buf[3];
  nvvsr_image_in img{};
  InFrame(const Yuv420 &f, bool nv12, int pad = 0) {
    const int w = f.w, h = f.h;
    if (nv12) {
      const int s0 = w + pad, s1 = w + pad;
      buf[0].assign((size_t)s0 * h, 0);
      buf[1].assign((size_t)s1 * (h / 2), 0);
      for (int y = 0; y < h; ++y) std::memcpy(&buf[0][(size_t)y * s0], &f.y.d[(size_t)y * w], w);
      for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
          buf[1][(size_t)y * s1 + 2 * x] = f.u.at(x, y);
          buf[1][(size_t)y * s1 + 2 * x + 1] = f.v.at(x, y);
        }
      img.plane[0] = buf[0].data();
      img.plane[1] = buf[1].data();
      img.plane[2] = nullptr;
      img.stride[0] = s0;
      img.stride[1] = s1;
      img.stride[2] = 0;
    } else {
      const int s0 = w + pad, s1 = w / 2 + pad;
      buf[0].assign((size_t)s0 * h, 0);
      buf[1].assign((size_t)s1 * (h / 2), 0);
      buf[2].assign((size_t)s1 * (h / 2), 0);
      for (int y = 0; y < h; ++y) std::memcpy(&buf[0][(size_t)y * s0], &f.y.d[(size_t)y * w], w);
      for (int y = 0; y < h / 2; ++y) {
        std::memcpy(&buf[1][(size_t)y * s1], &f.u.d[(size_t)y * (w / 2)], w / 2);
        std::memcpy(&buf[2][(size_t)y * s1], &f.v.d[(size_t)y * (w / 2)], w / 2);
      }
      for (int k = 0; k < 3; ++k) img.plane[k] = buf[k].data();
      img.stride[0] = s0;
      img.stride[1] = s1;
      img.stride[2] = s1;
    }
  }
};

// Output buffer with guard bytes: every plane has `pad` extra bytes per row and a trailing
// guard region, all filled with a sentinel that must be untouched after nvvsr_process().
struct OutFrame {
  static constexpr uint8_t kSentinel = 0xA5;
  int plane_w[3] = {0, 0, 0}, plane_h[3] = {0, 0, 0};
  std::vector<uint8_t> buf[3];
  nvvsr_image_out img{};
  OutFrame(int out_w, int out_h, int out_format, int pad = 0) {
    int n = 0;
    if (out_format == NVVSR_OUT_RGB24_PLANAR) {
      n = 3;
      for (int k = 0; k < 3; ++k) { plane_w[k] = out_w; plane_h[k] = out_h; }
    } else if (out_format == NVVSR_OUT_RGBA_CHUNKY) {
      n = 1;
      plane_w[0] = out_w * 4; plane_h[0] = out_h;
    } else {
      n = 3;
      plane_w[0] = out_w; plane_h[0] = out_h;
      plane_w[1] = plane_w[2] = out_w / 2; plane_h[1] = plane_h[2] = out_h / 2;
    }
    for (int k = 0; k < n; ++k) {
      const int stride = plane_w[k] + pad;
      buf[k].assign((size_t)stride * plane_h[k] + 4096, kSentinel);
      img.plane[k] = buf[k].data();
      img.stride[k] = stride;
    }
  }
  // true if all padding bytes and the trailing guard are still the sentinel
  bool guards_intact() const {
    for (int k = 0; k < 3; ++k) {
      if (!img.plane[k]) continue;
      const int stride = img.stride[k];
      for (int y = 0; y < plane_h[k]; ++y)
        for (int x = plane_w[k]; x < stride; ++x)
          if (buf[k][(size_t)y * stride + x] != kSentinel) return false;
      for (size_t i = (size_t)stride * plane_h[k]; i < buf[k].size(); ++i)
        if (buf[k][i] != kSentinel) return false;
    }
    return true;
  }
  uint8_t px(int plane, int x, int y) const { return buf[plane][(size_t)y * img.stride[plane] + x]; }
  // copy a tight plane out
  Plane plane_copy(int k) const {
    Plane p(plane_w[k], plane_h[k]);
    for (int y = 0; y < p.h; ++y) std::memcpy(&p.d[(size_t)y * p.w], &buf[k][(size_t)y * img.stride[k]], p.w);
    return p;
  }
};

inline void plane_stats(const Plane &p, int &mn, int &mx, double &mean) {
  mn = 255; mx = 0; double s = 0;
  for (uint8_t v : p.d) { mn = std::min<int>(mn, v); mx = std::max<int>(mx, v); s += v; }
  mean = p.d.empty() ? 0 : s / p.d.size();
}

// ---------------------------------------------------------------------------
// synthetic scene with real detail: thin anti-aliased shapes, edges, texture
// ---------------------------------------------------------------------------
inline double hash01(int x, int y, int s) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)s * 2147483647u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (h & 0xFFFFFF) / 16777216.0;
}
inline double vnoise(double x, double y, int s) {
  int xi = (int)std::floor(x), yi = (int)std::floor(y);
  double fx = x - xi, fy = y - yi;
  fx = fx * fx * (3 - 2 * fx);
  fy = fy * fy * (3 - 2 * fy);
  double a = hash01(xi, yi, s), b = hash01(xi + 1, yi, s), c = hash01(xi, yi + 1, s), d = hash01(xi + 1, yi + 1, s);
  return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}
// X,Y in reference pixels of a 1280x720 canvas. Returns luma in 0..1.
inline double scene_luma(double X, double Y, int seed) {
  double v = 0.22 + 0.30 * (Y / 720.0) + 0.06 * (X / 1280.0);
  v += 0.10 * (vnoise(X / 7.0, Y / 7.0, seed) - 0.5) + 0.06 * (vnoise(X / 2.7, Y / 2.7, seed + 1) - 0.5);
  // fine ring and a filled disc with a dark core
  double d1 = std::hypot(X - 380, Y - 290);
  if (std::fabs(d1 - 130) < 2.2) v = 0.96;
  double d2 = std::hypot(X - 880, Y - 430);
  if (d2 < 95) v = d2 < 38 ? 0.08 : 0.80;
  // Siemens star
  double dx = X - 640, dy = Y - 360, r = std::hypot(dx, dy);
  if (r < 110) v = 0.5 + 0.45 * (std::sin(28.0 * std::atan2(dy, dx)) > 0 ? 1 : -1);
  // checkerboard patch with 9px cells
  if (X > 60 && X < 300 && Y > 500 && Y < 680) v = ((((int)(X - 60)) / 9 + ((int)(Y - 500)) / 9) & 1) ? 0.9 : 0.12;
  // thin lines: 1.4px wide horizontals/verticals and a diagonal
  if (std::fabs(Y - 120) < 0.7 || std::fabs(Y - 150) < 0.7) v = 0.97;
  if (std::fabs(X - 1100) < 0.7 || std::fabs(X - 1130) < 0.7) v = 0.03;
  if (std::fabs((X - 700) * 0.6 - (Y - 600) * 0.8) < 0.9 && X > 700 && X < 1000) v = 0.95;
  // pseudo text: small bars of varying width
  if (Y > 40 && Y < 100) {
    int row = ((int)(Y - 40)) / 12;
    if (((int)(Y - 40)) % 12 < 8) {
      double gx = X - 80;
      int cell = (int)std::floor(gx / 6.0);
      if (gx > 0 && gx < 560 && hash01(cell, row, 99) > 0.45) v = 0.05;
    }
  }
  return std::min(1.0, std::max(0.0, v));
}
inline double scene_chroma(double X, double Y, int which) {
  double c = 0.5 + 0.18 * std::sin(X / 170.0 + (which ? 1.7 : 0.0)) * std::cos(Y / 120.0);
  if (std::hypot(X - 880, Y - 430) < 95) c = which ? 0.25 : 0.72;
  if (X > 60 && X < 300 && Y > 500 && Y < 680) c = which ? 0.62 : 0.35;
  return std::min(1.0, std::max(0.0, c));
}

// Render the scene at w x h (any size; canvas coordinates scale), 3x3 supersampled luma.
inline Yuv420 render_scene(int w, int h, int seed = 1) {
  Yuv420 f(w, h);
  const double sx = 1280.0 / w, sy = 720.0 / h;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      double acc = 0;
      for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) acc += scene_luma((x + (i + 0.5) / 3.0) * sx, (y + (j + 0.5) / 3.0) * sy, seed);
      f.y.at(x, y) = (uint8_t)std::lround(16 + 219 * acc / 9.0);
    }
  for (int y = 0; y < h / 2; ++y)
    for (int x = 0; x < w / 2; ++x) {
      f.u.at(x, y) = (uint8_t)std::lround(16 + 224 * scene_chroma((2 * x + 1) * sx, (2 * y + 1) * sy, 0));
      f.v.at(x, y) = (uint8_t)std::lround(16 + 224 * scene_chroma((2 * x + 1) * sx, (2 * y + 1) * sy, 1));
    }
  return f;
}

// ---------------------------------------------------------------------------
// resampling references and metrics (CPU)
// ---------------------------------------------------------------------------
inline Plane box_down2(const Plane &p) {
  Plane o(p.w / 2, p.h / 2);
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x)
      o.at(x, y) = (uint8_t)((p.at(2 * x, 2 * y) + p.at(2 * x + 1, 2 * y) + p.at(2 * x, 2 * y + 1) + p.at(2 * x + 1, 2 * y + 1) + 2) >> 2);
  return o;
}
inline Yuv420 box_down2(const Yuv420 &f) {
  Yuv420 o;
  o.w = f.w / 2; o.h = f.h / 2;
  o.y = box_down2(f.y);
  o.u = box_down2(f.u);
  o.v = box_down2(f.v);
  return o;
}

inline double cubic_w(double t, double a) {  // Keys cubic convolution kernel
  t = std::fabs(t);
  if (t <= 1) return (a + 2) * t * t * t - (a + 3) * t * t + 1;
  if (t < 2) return a * t * t * t - 5 * a * t * t + 8 * a * t - 4 * a;
  return 0;
}
// Resize with half-pixel-centre alignment. kind: 0 bilinear, 1 bicubic (a = -0.5)
inline Plane resize(const Plane &p, int ow, int oh, int kind) {
  Plane o(ow, oh);
  auto clampi = [](int v, int lo, int hi) { return std::min(hi, std::max(lo, v)); };
  const double sx = (double)p.w / ow, sy = (double)p.h / oh;
  const int taps = kind ? 4 : 2;
  for (int y = 0; y < oh; ++y) {
    double fy = (y + 0.5) * sy - 0.5;
    int y0 = (int)std::floor(fy);
    for (int x = 0; x < ow; ++x) {
      double fx = (x + 0.5) * sx - 0.5;
      int x0 = (int)std::floor(fx);
      double acc = 0, wsum = 0;
      for (int j = 0; j < taps; ++j) {
        int yy = y0 - (taps / 2 - 1) + j;
        double wy = kind ? cubic_w(fy - yy, -0.5) : std::max(0.0, 1 - std::fabs(fy - yy));
        for (int i = 0; i < taps; ++i) {
          int xx = x0 - (taps / 2 - 1) + i;
          double wx = kind ? cubic_w(fx - xx, -0.5) : std::max(0.0, 1 - std::fabs(fx - xx));
          double w = wx * wy;
          acc += w * p.at(clampi(xx, 0, p.w - 1), clampi(yy, 0, p.h - 1));
          wsum += w;
        }
      }
      o.at(x, y) = (uint8_t)std::min(255.0, std::max(0.0, std::round(acc / wsum)));
    }
  }
  return o;
}

inline double mse(const Plane &a, const Plane &b, int border = 0) {
  double s = 0; size_t n = 0;
  for (int y = border; y < a.h - border; ++y)
    for (int x = border; x < a.w - border; ++x) {
      double d = (double)a.at(x, y) - b.at(x, y);
      s += d * d; ++n;
    }
  return n ? s / n : 0;
}
inline double psnr(const Plane &a, const Plane &b, int border = 0) {
  double m = mse(a, b, border);
  return m <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / m);
}
inline double mae(const Plane &a, const Plane &b, int border = 0) {
  double s = 0; size_t n = 0;
  for (int y = border; y < a.h - border; ++y)
    for (int x = border; x < a.w - border; ++x) { s += std::fabs((double)a.at(x, y) - b.at(x, y)); ++n; }
  return n ? s / n : 0;
}

// ---------------------------------------------------------------------------
// colour reference (CPU, double precision): YUV8 -> RGB (0..255, unclamped) and back
// ---------------------------------------------------------------------------
struct Rgbd { double r, g, b; };
inline void kr_kb(bool bt709, double &kr, double &kb) { kr = bt709 ? 0.2126 : 0.299; kb = bt709 ? 0.0722 : 0.114; }
inline Rgbd yuv_to_rgb(int Y, int U, int V, bool bt709, bool full) {
  double kr, kb; kr_kb(bt709, kr, kb);
  double kg = 1 - kr - kb;
  double y = full ? Y / 255.0 : (Y - 16) / 219.0;
  double cb = full ? (U - 128) / 255.0 : (U - 128) / 224.0;
  double cr = full ? (V - 128) / 255.0 : (V - 128) / 224.0;
  double r = y + 2 * (1 - kr) * cr, b = y + 2 * (1 - kb) * cb, g = (y - kr * r - kb * b) / kg;
  return {r * 255, g * 255, b * 255};
}
inline void rgb_to_yuv(double R, double G, double B, bool bt709, bool full, double &Y, double &U, double &V) {
  double kr, kb; kr_kb(bt709, kr, kb);
  double kg = 1 - kr - kb;
  double r = R / 255, g = G / 255, b = B / 255;
  double y = kr * r + kg * g + kb * b;
  double cb = (b - y) / (2 * (1 - kb)), cr = (r - y) / (2 * (1 - kr));
  if (full) { Y = 255 * y; U = 128 + 255 * cb; V = 128 + 255 * cr; }
  else { Y = 16 + 219 * y; U = 128 + 224 * cb; V = 128 + 224 * cr; }
}
inline double clamp255(double v) { return std::min(255.0, std::max(0.0, v)); }

// ---------------------------------------------------------------------------
// system info (best effort)
// ---------------------------------------------------------------------------
// GPU memory in use in MiB according to nvidia-smi, or -1 if unavailable.
inline long gpu_used_mib() {
  FILE *p = popen("nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits 2>/dev/null", "r");
  if (!p) return -1;
  char line[128] = {0};
  long v = -1;
  if (std::fgets(line, sizeof line, p)) v = std::atol(line);
  int rc = pclose(p);
  return rc == 0 ? v : -1;
}
inline long rss_mib() {
  FILE *f = std::fopen("/proc/self/statm", "r");
  if (!f) return -1;
  long pages = 0, rss = 0;
  if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = -1;
  std::fclose(f);
  return rss < 0 ? -1 : rss * sysconf(_SC_PAGESIZE) / (1024 * 1024);
}

}  // namespace tu
