// The plugin's output equals libnvvsr called directly, byte for byte, on a
// 1282x722 pattern (row strides padded), and grey input keeps its level. The test links only libvapoursynth, so libnvvsr is taken from the
// process after LoadPlugin (the plugin loaded it; RTLD_NOLOAD returns that same object). Needs
// NVVSR_PLUGIN and NVVSR_LIBDIR (ctest sets them).
#include <dlfcn.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "../../core/include/nvvsr.h"  // the only core header the plugin tests may use (no CMake include path for it)
#include "vs_check.h"
#include "vs_harness.h"

using vst::Env;
using vst::Map;
using vst::Spec;

namespace {

struct Lib {
  decltype(&nvvsr_config_init) config_init = nullptr;
  decltype(&nvvsr_probe) probe = nullptr;
  decltype(&nvvsr_create) create = nullptr;
  decltype(&nvvsr_process) process = nullptr;
  decltype(&nvvsr_destroy) destroy = nullptr;
  decltype(&nvvsr_last_error) last_error = nullptr;
};

template <class F>
F sym(void *h, const char *name) {
  return reinterpret_cast<F>(dlsym(h, name));
}

bool load_lib(Lib *L, std::string *err) {
  void *h = dlopen("libnvvsr.so.0", RTLD_NOW | RTLD_NOLOAD);
  if (!h) {
    *err = "libnvvsr.so.0 is not loaded (the plugin did not load it)";
    return false;
  }
  L->config_init = sym<decltype(L->config_init)>(h, "nvvsr_config_init");
  L->probe = sym<decltype(L->probe)>(h, "nvvsr_probe");
  L->create = sym<decltype(L->create)>(h, "nvvsr_create");
  L->process = sym<decltype(L->process)>(h, "nvvsr_process");
  L->destroy = sym<decltype(L->destroy)>(h, "nvvsr_destroy");
  L->last_error = sym<decltype(L->last_error)>(h, "nvvsr_last_error");
  if (!L->config_init || !L->probe || !L->create || !L->process || !L->destroy || !L->last_error) {
    *err = "libnvvsr.so.0 lacks a function the reference needs";
    return false;
  }
  return true;
}

// Planes of a frame back to back, rows without padding.
std::vector<uint8_t> packed(const vst::Frame &f) {
  std::vector<uint8_t> out;
  for (int p = 0; p < 3; ++p)
    for (int y = 0; y < f.height(p); ++y) {
      const uint8_t *row = f.plane(p) + (ptrdiff_t)y * f.stride(p);
      out.insert(out.end(), row, row + f.width(p));
    }
  return out;
}

// libnvvsr on `in` (each entry one packed YUV420P8 frame of iw x ih): the packed results at ow x oh,
// RGB24 planes or I420 planes. An empty result and *err set on failure.
std::vector<uint8_t> ref_run(const Lib &L, int iw, int ih, int ow, int oh, nvvsr_matrix matrix, bool full_range, bool yuv,
                             nvvsr_quality quality, const std::vector<std::vector<uint8_t>> &in, std::string *err) {
  nvvsr_config cfg;
  L.config_init(&cfg);
  cfg.in_w = iw;
  cfg.in_h = ih;
  cfg.out_w = ow;
  cfg.out_h = oh;
  cfg.quality = quality;
  cfg.in_format = NVVSR_IN_I420;
  cfg.out_format = yuv ? NVVSR_OUT_I420 : NVVSR_OUT_RGB24_PLANAR;
  cfg.matrix = matrix;
  cfg.full_range = full_range;
  if (L.probe() != NVVSR_OK) {
    *err = std::string("probe: ") + L.last_error();
    return {};
  }
  nvvsr_ctx *ctx = nullptr;
  if (L.create(&cfg, &ctx) != NVVSR_OK) {
    *err = std::string("create: ") + L.last_error();
    return {};
  }
  const size_t out_bytes = yuv ? (size_t)ow * oh + 2 * (size_t)(ow / 2) * (oh / 2) : 3 * (size_t)ow * oh;
  std::vector<uint8_t> out;
  for (const std::vector<uint8_t> &fr : in) {
    std::vector<uint8_t> o(out_bytes);
    nvvsr_image_in src{};
    nvvsr_image_out dst{};
    src.plane[0] = fr.data();
    src.stride[0] = iw;
    src.plane[1] = fr.data() + (size_t)iw * ih;
    src.stride[1] = iw / 2;
    src.plane[2] = src.plane[1] + (size_t)(iw / 2) * (ih / 2);
    src.stride[2] = iw / 2;
    dst.plane[0] = o.data();
    dst.stride[0] = ow;
    dst.plane[1] = o.data() + (size_t)ow * oh;
    dst.stride[1] = yuv ? ow / 2 : ow;
    dst.plane[2] = dst.plane[1] + (yuv ? (size_t)(ow / 2) * (oh / 2) : (size_t)ow * oh);
    dst.stride[2] = dst.stride[1];
    const int rc = L.process(ctx, &src, &dst);
    if (rc != NVVSR_OK) {
      *err = std::string("process: ") + L.last_error();
      out.clear();
      break;
    }
    out.insert(out.end(), o.begin(), o.end());
  }
  L.destroy(ctx);
  return out;
}

// Mean and standard deviation of plane p of an 8-bit frame.
struct Stats {
  double mean = 0, stddev = 0;
};
Stats plane_stats(const vst::Frame &f, int p) {
  double sum = 0, sq = 0;
  const double n = (double)f.width(p) * f.height(p);
  for (int y = 0; y < f.height(p); ++y) {
    const uint8_t *row = f.plane(p) + (ptrdiff_t)y * f.stride(p);
    for (int x = 0; x < f.width(p); ++x) {
      sum += row[x];
      sq += (double)row[x] * row[x];
    }
  }
  Stats s;
  s.mean = sum / n;
  s.stddev = std::sqrt(std::max(0.0, sq / n - s.mean * s.mean));
  return s;
}

struct Case {
  const char *name;
  const char *quality;  // the Upscale argument
  nvvsr_quality q;      // the same quality for libnvvsr
  const char *matrix;   // Upscale argument, or nullptr (not passed)
  nvvsr_matrix m;
  int range;            // Upscale argument (0 full, 1 limited), or -1 (not passed)
  bool yuv;             // format=yuv420p8
};

// Plugin output vs libnvvsr on the same 3 frames: max_abs_diff must be 0 and every byte compared.
void cmp_ref(const Env &env, const Lib &L, const vst::Result &src, const std::vector<std::vector<uint8_t>> &in, const Case &c) {
  Map a = env.map();
  a.i("width", 2564).i("height", 1444).s("quality", c.quality);
  if (c.matrix) a.s("matrix", c.matrix);
  if (c.range >= 0) a.i("range", c.range);
  if (c.yuv) a.s("format", "yuv420p8");
  vst::Result up = env.then(src, "nvvsr", "Upscale", a);
  std::string err;
  std::vector<uint8_t> want = ref_run(L, 1282, 722, 2564, 1444, c.m, c.range == 0, c.yuv, c.q, in, &err);
  std::vector<uint8_t> got;
  for (int n = 0; n < (int)in.size() && err.empty(); ++n) {
    vst::Frame f = up.frame(n, &err);
    if (f) {
      const std::vector<uint8_t> b = packed(f);
      got.insert(got.end(), b.begin(), b.end());
    }
  }
  if (!err.empty()) {
    vt::check(false, c.name, err);
    return;
  }
  int worst = 0;
  const size_t n = std::min(got.size(), want.size());
  for (size_t i = 0; i < n; ++i) worst = std::max(worst, std::abs((int)got[i] - (int)want[i]));
  const bool same_size = !want.empty() && got.size() == want.size();
  vt::check(same_size && worst == 0, c.name,
            "frames=3 max_abs_diff=" + std::to_string(worst) + " bytes_compared=" + std::to_string(n) + "/" +
                std::to_string(want.size()) + " (plugin " + std::to_string(got.size()) + " bytes)");
}

// Grey input (neutral chroma) upscaled to RGB24: the mean level follows the limited-range luma of the source,
// R=G=B within `spread`, and the picture is not flat. Both frames of a 640x360 pattern must pass.
void sanity(const Env &env, const char *name, const char *quality, int spread) {
  vst::Result src = env.test_clip(Spec{640, 360, pfYUV420P8, 2, true, true});
  Map a = env.map();
  a.i("width", 1280).i("height", 720).s("quality", quality);
  vst::Result up = env.then(src, "nvvsr", "Upscale", a);
  std::string why, err;
  for (int n = 0; n < 2 && why.empty(); ++n) {
    vst::Frame in = src.frame(n, &err), out = up.frame(n, &err);
    if (!in || !out) {
      why = err;
      break;
    }
    double expect = 0;  // mean of ((Y - 16) * 255 / 219) clipped to 0..255, as in the Python check
    for (int y = 0; y < in.height(0); ++y) {
      const uint8_t *row = in.plane(0) + (ptrdiff_t)y * in.stride(0);
      for (int x = 0; x < in.width(0); ++x) expect += std::min(255.0, std::max(0.0, (row[x] - 16) * 255.0 / 219.0));
    }
    expect /= (double)in.width(0) * in.height(0);
    int spread_seen = 0;
    for (int y = 0; y < out.height(0); ++y) {
      const uint8_t *r = out.plane(0) + (ptrdiff_t)y * out.stride(0);
      const uint8_t *g = out.plane(1) + (ptrdiff_t)y * out.stride(1);
      const uint8_t *b = out.plane(2) + (ptrdiff_t)y * out.stride(2);
      for (int x = 0; x < out.width(0); ++x)
        spread_seen = std::max({spread_seen, std::abs(r[x] - g[x]), std::abs(g[x] - b[x])});
    }
    const Stats gs = plane_stats(out, 1);
    const std::string frame_note = "frame " + std::to_string(n) + ": mean(G)=" + std::to_string(gs.mean) +
                                   " expected " + std::to_string(expect) + ", spread " + std::to_string(spread_seen) +
                                   ", std(G)=" + std::to_string(gs.stddev);
    if (std::fabs(gs.mean - expect) >= 3) why = "mean level off; " + frame_note;
    else if (spread_seen > spread) why = "grey input should give R=G=B; " + frame_note;
    else if (gs.stddev <= 20) why = "output looks constant; " + frame_note;
  }
  vt::check(why.empty(), name, why);
}

}  // namespace

int main() {
  Env env;  // first: it must outlive every Map, Node and Frame below
  if (!env) {
    std::printf("FAIL cannot create a VapourSynth core\n");
    return 1;
  }
  std::string err;
  if (!vt::check(env.load_nvvsr(&err), "plugin loads", err)) return vt::finish("test_correct");
  Lib L;
  if (!vt::check(load_lib(&L, &err), "libnvvsr is available to the reference", err)) return vt::finish("test_correct");

  std::printf("== 3. correctness against libnvvsr called directly (byte for byte)\n");
  // 1282x722 -> 2564x1444: neither size is a multiple of VapourSynth's row alignment, so row strides are padded.
  vst::Result src = env.test_clip(Spec{1282, 722, pfYUV420P8, 3, true});
  std::vector<std::vector<uint8_t>> in;
  for (int n = 0; n < 3 && src; ++n) {
    vst::Frame f = src.frame(n, &err);
    if (f) in.push_back(packed(f));
  }
  size_t in_bytes = 0;
  for (const auto &fr : in) in_bytes += fr.size();
  vt::check_eq("pattern source written (3 frames I420)", std::to_string(in_bytes),
               std::to_string(3 * (1282 * 722 + 2 * 641 * 361)));
  if (in.size() != 3) return vt::finish("test_correct");  // no source frames: nothing to compare

  const Case cases[] = {
      {"bicubic RGB24 == libnvvsr (709 by size, limited)", "bicubic", NVVSR_QUALITY_BICUBIC, nullptr, NVVSR_MATRIX_AUTO, -1, false},
      {"medium  RGB24 == libnvvsr (709 by size, limited)", "medium", NVVSR_QUALITY_MEDIUM, nullptr, NVVSR_MATRIX_AUTO, -1, false},
      {"bicubic RGB24, matrix=601 range=0 (full) == libnvvsr", "bicubic", NVVSR_QUALITY_BICUBIC, "601", NVVSR_MATRIX_BT601, 0,
       false},
      {"bicubic YUV420P8 output == libnvvsr (I420 path)", "bicubic", NVVSR_QUALITY_BICUBIC, nullptr, NVVSR_MATRIX_AUTO, -1, true},
      {"high    YUV420P8, matrix=709 range=1 == libnvvsr", "high", NVVSR_QUALITY_HIGH, "709", NVVSR_MATRIX_BT709, 1, true},
  };
  for (const Case &c : cases) cmp_ref(env, L, src, in, c);

  sanity(env, "sanity (bicubic): grey input keeps its level, R=G=B within 3, not constant", "bicubic", 3);
  sanity(env, "sanity (medium): grey input keeps its level, R=G=B within 12, not constant", "medium", 12);

  return vt::finish("test_correct");
}
