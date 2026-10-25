// Test 2: NV12/I420 -> RGB24_PLANAR / RGBA_CHUNKY / I420 at 1280x720 -> 2560x1440, plus other
// scale ratios. Checks dimensions (guard bytes), non-constant output, alpha, consistency between
// formats, strides and determinism.
#include <cstring>

#include "test_util.h"

struct Result {
  std::vector<uint8_t> planes[3];
};

static bool run_case(const tu::Yuv420 &src, int in_fmt, int out_fmt, int ow, int oh, int quality, int pad, Result &res,
                     bool verbose = true) {
  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = src.w; cfg.in_h = src.h; cfg.out_w = ow; cfg.out_h = oh;
  cfg.quality = quality; cfg.in_format = in_fmt; cfg.out_format = out_fmt;
  nvvsr_ctx *ctx = nullptr;
  int rc = nvvsr_create(&cfg, &ctx);
  T_CHECK(rc == NVVSR_OK, "create %dx%d->%dx%d: %s [%s]", src.w, src.h, ow, oh, nvvsr_strerror(rc), nvvsr_last_error());
  if (rc != NVVSR_OK) return false;
  tu::InFrame in(src, in_fmt == NVVSR_IN_NV12, pad);
  tu::OutFrame out(ow, oh, out_fmt, pad);
  rc = nvvsr_process(ctx, &in.img, &out.img);
  T_CHECK(rc == NVVSR_OK, "process: %s [%s]", nvvsr_strerror(rc), nvvsr_last_error());
  if (rc != NVVSR_OK) { nvvsr_destroy(ctx); return false; }
  T_CHECK(out.guards_intact(), "%s>%s %dx%d->%dx%d: wrote outside the frame (padding/guard overwritten)",
          in_fmt == NVVSR_IN_NV12 ? "nv12" : "i420", out_fmt == 0 ? "rgb24p" : out_fmt == 1 ? "rgba" : "i420", src.w, src.h, ow,
          oh);
  // second frame on the same ctx must be bit-identical (stateless, buffers reused)
  tu::OutFrame again(ow, oh, out_fmt, pad);
  rc = nvvsr_process(ctx, &in.img, &again.img);
  T_CHECK(rc == NVVSR_OK, "second process failed");
  for (int k = 0; k < 3; ++k) {
    if (!out.img.plane[k]) continue;
    tu::Plane p = out.plane_copy(k), p2 = again.plane_copy(k);
    T_CHECK(p.d == p2.d, "second frame differs from first (plane %d)", k);
    int mn, mx; double mean;
    tu::plane_stats(p, mn, mx, mean);
    if (verbose) std::printf("    plane %d: %dx%d min %d max %d mean %.1f\n", k, p.w, p.h, mn, mx, mean);
    bool chroma = out_fmt == NVVSR_OUT_I420 && k > 0;
    T_CHECK(mx - mn > (chroma ? 10 : 80), "plane %d is (nearly) constant: min %d max %d", k, mn, mx);
    res.planes[k] = p.d;
  }
  if (out_fmt == NVVSR_OUT_RGBA_CHUNKY) {
    const std::vector<uint8_t> &p = res.planes[0];
    bool alpha_ok = true;
    for (size_t i = 3; i < p.size(); i += 4) alpha_ok &= p[i] == 255;
    T_CHECK(alpha_ok, "alpha channel is not 255 everywhere");
  }
  nvvsr_destroy(ctx);
  return true;
}

static int max_diff(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
  if (a.size() != b.size()) return 255;
  int m = 0;
  for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs((int)a[i] - (int)b[i]));
  return m;
}

int main() {
  if (int s = tu::skip_unless_usable("test_formats")) return s;

  const int W = 1280, H = 720, OW = 2560, OH = 1440;
  tu::Yuv420 src = tu::render_scene(W, H, 1);
  const int in_fmts[] = {NVVSR_IN_NV12, NVVSR_IN_I420};
  const int out_fmts[] = {NVVSR_OUT_RGB24_PLANAR, NVVSR_OUT_RGBA_CHUNKY, NVVSR_OUT_I420};
  Result res[2][3];
  for (int i = 0; i < 2; ++i)
    for (int o = 0; o < 3; ++o) {
      std::printf("  %s -> %s  %dx%d -> %dx%d HIGH (stride padded by 17)\n", i ? "I420" : "NV12",
                  o == 0 ? "RGB24_PLANAR" : o == 1 ? "RGBA_CHUNKY" : "I420", W, H, OW, OH);
      run_case(src, in_fmts[i], out_fmts[o], OW, OH, NVVSR_QUALITY_HIGH, 17, res[i][o]);
    }

  // NV12 and I420 input carry the same data -> same output
  for (int o = 0; o < 3; ++o)
    for (int k = 0; k < 3; ++k) {
      if (res[0][o].planes[k].empty()) continue;
      int d = max_diff(res[0][o].planes[k], res[1][o].planes[k]);
      T_CHECK(d <= 1, "NV12 vs I420 input differ by %d (out fmt %d plane %d)", d, o, k);
    }

  // RGB24 planar vs RGBA chunky: same SR output, just different layout
  {
    const auto &rgba = res[0][1].planes[0];
    int d = 0;
    for (int k = 0; k < 3; ++k)
      for (size_t i = 0; i < (size_t)OW * OH; ++i) d = std::max(d, std::abs((int)res[0][0].planes[k][i] - (int)rgba[4 * i + k]));
    T_CHECK(d <= 1, "RGB24 planar vs RGBA chunky differ by %d", d);
    std::printf("  planar-vs-chunky max diff: %d\n", d);

    // I420 output vs CPU conversion of the RGB output (BT.709 limited, auto matrix for 1280x720)
    tu::Plane Yc(OW, OH), yres(OW, OH);
    yres.d = res[0][2].planes[0];
    for (int y = 0; y < OH; ++y)
      for (int x = 0; x < OW; ++x) {
        double Y, U, V;
        size_t i = (size_t)y * OW + x;
        tu::rgb_to_yuv(res[0][0].planes[0][i], res[0][0].planes[1][i], res[0][0].planes[2][i], true, false, Y, U, V);
        Yc.at(x, y) = (uint8_t)std::lround(tu::clamp255(Y));
      }
    double m = tu::mae(Yc, yres);
    std::printf("  I420 luma vs CPU conversion of the RGB result: MAE %.3f\n", m);
    T_CHECK(m < 1.0, "I420 luma deviates from the RGB output by MAE %.3f", m);
  }

  // other geometries: 1.33x (1080p->1440p), 3x (480p->1440p), 1x, non-uniform, non-multiple sizes
  struct Geo { int iw, ih, ow, oh, q; };
  const Geo geos[] = {{1920, 1080, 2560, 1440, NVVSR_QUALITY_MEDIUM}, {854, 480, 2560, 1440, NVVSR_QUALITY_ULTRA},
                      {1280, 720, 1280, 720, NVVSR_QUALITY_HIGH},    {640, 360, 1280, 960, NVVSR_QUALITY_HIGH},
                      {1000, 562, 1750, 986, NVVSR_QUALITY_LOW},     {320, 240, 1280, 960, NVVSR_QUALITY_STREAMING_MEDIUM},
                      {1280, 720, 2560, 1440, NVVSR_QUALITY_STREAMING_ULTRA}, {1280, 720, 2560, 1440, NVVSR_QUALITY_LOW},
                      {1280, 720, 2560, 1440, NVVSR_QUALITY_MEDIUM}, {1280, 720, 2560, 1440, NVVSR_QUALITY_ULTRA},
                      {1280, 720, 2560, 1440, NVVSR_QUALITY_BICUBIC}};
  for (const Geo &g : geos) {
    std::printf("  geometry %dx%d -> %dx%d q=%d\n", g.iw, g.ih, g.ow, g.oh, g.q);
    tu::Yuv420 s = tu::render_scene(g.iw, g.ih, 2);
    Result r;
    run_case(s, NVVSR_IN_NV12, NVVSR_OUT_RGB24_PLANAR, g.ow, g.oh, g.q, 0, r, false);
    Result r2;
    run_case(s, NVVSR_IN_I420, NVVSR_OUT_I420, g.ow, g.oh, g.q, 5, r2, false);
  }
  return tu::finish("test_formats");
}
