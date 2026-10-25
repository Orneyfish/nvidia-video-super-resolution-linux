// Benchmark core shared by nvvsr_bench and the timing test. Uses only the public nvvsr.h.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "nvvsr.h"

namespace bench {

struct Cfg {
  int iw = 1920, ih = 1080, ow = 2560, oh = 1440;
  int quality = NVVSR_QUALITY_AUTO;
  int iters = 200;
  int in_format = NVVSR_IN_NV12;
  int out_format = NVVSR_OUT_RGB24_PLANAR;
  bool pinned = true;
  int matrix = NVVSR_MATRIX_AUTO;
  bool full_range = false;
  double fps = 0;
};

struct Result {
  int rc = 0;
  int quality = 0;
  double create_ms = 0;
  double wall_mean = 0, wall_min = 0, wall_max = 0;  // timing flag off: what a caller really sees
  nvvsr_timing stage{};                              // mean of the timed run
};

// Host buffers for one frame pair, pinned or pageable.
struct Frames {
  std::vector<uint8_t> heap_in[3], heap_out[3];
  void *pinned[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  nvvsr_image_in in{};
  nvvsr_image_out out{};
  bool ok = true;

  static uint8_t *take(Frames &f, int slot, std::vector<uint8_t> &heap, size_t bytes, bool pinned_mem) {
    if (pinned_mem) {
      f.pinned[slot] = nvvsr_alloc_host(bytes);
      if (f.pinned[slot]) return static_cast<uint8_t *>(f.pinned[slot]);
    }
    heap.assign(bytes, 0);
    return heap.data();
  }
  Frames(const Cfg &c) {
    const size_t iw = c.iw, ih = c.ih, ow = c.ow, oh = c.oh;
    size_t in_bytes[3] = {iw * ih, 0, 0};
    int in_stride[3] = {c.iw, 0, 0};
    if (c.in_format == NVVSR_IN_NV12) {
      in_bytes[1] = iw * (ih / 2); in_stride[1] = c.iw;
    } else {
      in_bytes[1] = in_bytes[2] = (iw / 2) * (ih / 2); in_stride[1] = in_stride[2] = c.iw / 2;
    }
    for (int k = 0; k < 3; ++k) {
      if (!in_bytes[k]) continue;
      uint8_t *p = take(*this, k, heap_in[k], in_bytes[k], c.pinned);
      // cheap non-trivial content
      uint32_t s = 12345u + k;
      for (size_t i = 0; i < in_bytes[k]; ++i) { s = s * 1664525u + 1013904223u; p[i] = (uint8_t)(16 + ((i % in_stride[k]) * 3 / 2 + (s >> 28)) % 200); }
      in.plane[k] = p; in.stride[k] = in_stride[k];
    }
    size_t out_bytes[3] = {0, 0, 0};
    int out_stride[3] = {0, 0, 0};
    if (c.out_format == NVVSR_OUT_RGB24_PLANAR) {
      for (int k = 0; k < 3; ++k) { out_bytes[k] = ow * oh; out_stride[k] = c.ow; }
    } else if (c.out_format == NVVSR_OUT_RGBA_CHUNKY) {
      out_bytes[0] = ow * oh * 4; out_stride[0] = c.ow * 4;
    } else {
      out_bytes[0] = ow * oh; out_stride[0] = c.ow;
      out_bytes[1] = out_bytes[2] = (ow / 2) * (oh / 2); out_stride[1] = out_stride[2] = c.ow / 2;
    }
    for (int k = 0; k < 3; ++k) {
      if (!out_bytes[k]) continue;
      out.plane[k] = take(*this, 3 + k, heap_out[k], out_bytes[k], c.pinned);
      out.stride[k] = out_stride[k];
    }
  }
  ~Frames() {
    for (void *p : pinned) nvvsr_free_host(p);
  }
  Frames(const Frames &) = delete;
  Frames &operator=(const Frames &) = delete;
};

inline double ms_now() {
  using C = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(C::now().time_since_epoch()).count();
}

inline nvvsr_config make_config(const Cfg &c, unsigned flags) {
  nvvsr_config n;
  nvvsr_config_init(&n);
  n.in_w = c.iw; n.in_h = c.ih; n.out_w = c.ow; n.out_h = c.oh;
  n.quality = c.quality; n.in_format = c.in_format; n.out_format = c.out_format;
  n.matrix = c.matrix; n.full_range = c.full_range; n.fps = c.fps; n.flags = flags;
  return n;
}

// Pass 1 (timing flag off): wall time per process() as a front-end sees it.
// Pass 2 (timing flag on): per-stage means.
inline Result run(const Cfg &c) {
  Result r;
  Frames fr(c);
  for (int pass = 0; pass < 2; ++pass) {
    nvvsr_config cfg = make_config(c, pass ? (unsigned)NVVSR_FLAG_TIMING : 0u);
    nvvsr_ctx *ctx = nullptr;
    double t0 = ms_now();
    r.rc = nvvsr_create(&cfg, &ctx);
    double create_ms = ms_now() - t0;
    if (r.rc != NVVSR_OK) return r;
    if (!pass) r.create_ms = create_ms;
    r.quality = nvvsr_get_quality(ctx);
    for (int i = 0; i < 10 && r.rc == NVVSR_OK; ++i) r.rc = nvvsr_process(ctx, &fr.in, &fr.out);
    double sum = 0, mn = 1e30, mx = 0;
    nvvsr_timing acc{};
    for (int i = 0; i < c.iters && r.rc == NVVSR_OK; ++i) {
      double a = ms_now();
      r.rc = nvvsr_process(ctx, &fr.in, &fr.out);
      double d = ms_now() - a;
      sum += d; mn = std::min(mn, d); mx = std::max(mx, d);
      nvvsr_timing t;
      nvvsr_last_timing(ctx, &t);
      acc.upload_ms += t.upload_ms; acc.convert_ms += t.convert_ms; acc.sr_ms += t.sr_ms;
      acc.download_ms += t.download_ms; acc.total_ms += t.total_ms;
    }
    nvvsr_destroy(ctx);
    if (r.rc != NVVSR_OK) return r;
    if (!pass) {
      r.wall_mean = sum / c.iters; r.wall_min = mn; r.wall_max = mx;
    } else {
      r.stage.upload_ms = acc.upload_ms / c.iters; r.stage.convert_ms = acc.convert_ms / c.iters;
      r.stage.sr_ms = acc.sr_ms / c.iters; r.stage.download_ms = acc.download_ms / c.iters;
      r.stage.total_ms = acc.total_ms / c.iters;
    }
  }
  return r;
}

inline const char *quality_name(int q) {
  switch (q) {
    case 0: return "bicubic"; case 1: return "low"; case 2: return "medium"; case 3: return "high";
    case 4: return "ultra"; case 21: return "stream-medium"; case 23: return "stream-ultra";
    default: return "?";
  }
}
inline const char *in_name(int f) { return f == NVVSR_IN_NV12 ? "nv12" : "i420"; }
inline const char *out_name(int f) { return f == NVVSR_OUT_RGB24_PLANAR ? "rgb24p" : f == NVVSR_OUT_RGBA_CHUNKY ? "rgba" : "i420"; }

inline void print_header() {
  std::printf("%-26s %-13s %-4s | %7s | %6s %6s %6s %6s = %6s | %6s %6s %6s\n", "case", "quality", "mem", "create",
              "upload", "conv", "SR", "down", "sum", "wall", "min", "max");
}
inline void print_row(const Cfg &c, const Result &r) {
  char name[96];
  std::snprintf(name, sizeof name, "%dx%d->%dx%d %s>%s", c.iw, c.ih, c.ow, c.oh, in_name(c.in_format), out_name(c.out_format));
  if (r.rc != NVVSR_OK) {
    std::printf("%-26s FAILED: %s (%s)\n", name, nvvsr_strerror(r.rc), nvvsr_last_error());
    return;
  }
  std::printf("%-26s %-13s %-4s | %5.0f ms | %6.2f %6.2f %6.2f %6.2f = %6.2f | %6.2f %6.2f %6.2f  (ms/frame)\n", name,
              quality_name(r.quality), c.pinned ? "pin" : "pg", r.create_ms, r.stage.upload_ms, r.stage.convert_ms,
              r.stage.sr_ms, r.stage.download_ms,
              r.stage.upload_ms + r.stage.convert_ms + r.stage.sr_ms + r.stage.download_ms, r.wall_mean, r.wall_min, r.wall_max);
}

}  // namespace bench
