// Test 4: colour correctness. Flat patches go through the pipeline at BICUBIC (no network, so a flat
// area stays exactly flat) and are compared with a CPU double-precision reference (+-3):
// {BT.601, BT.709} x {limited, full range} x {NV12, I420 input} x {RGB24 planar, RGBA, I420 output}.
#include "test_util.h"

struct Patch { int y, u, v; };

// 4x4 blocks of 40x24 input pixels
static const Patch kPatches[16] = {
    {100, 150, 90},  {16, 128, 128},  {235, 128, 128}, {128, 128, 128}, {81, 90, 240},   {145, 54, 34},
    {41, 240, 110},  {210, 16, 146},  {170, 166, 16},  {106, 202, 222}, {60, 100, 160},  {200, 100, 150},
    {30, 180, 80},   {250, 128, 128}, {0, 128, 128},   {180, 60, 200}};

static tu::Yuv420 make_frame(int w, int h) {
  tu::Yuv420 f(w, h);
  const int bw = w / 4, bh = h / 4;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) f.y.at(x, y) = (uint8_t)kPatches[(y / bh) * 4 + x / bw].y;
  for (int y = 0; y < h / 2; ++y)
    for (int x = 0; x < w / 2; ++x) {
      const Patch &p = kPatches[((2 * y) / bh) * 4 + (2 * x) / bw];
      f.u.at(x, y) = (uint8_t)p.u;
      f.v.at(x, y) = (uint8_t)p.v;
    }
  return f;
}

struct Worst { double rgb = 0, yuv = 0; };

static void run(int matrix, bool full, int in_fmt, int out_fmt, int w, int h, bool bt709_expected, Worst &worst) {
  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = w; cfg.in_h = h; cfg.out_w = 2 * w; cfg.out_h = 2 * h;
  cfg.quality = NVVSR_QUALITY_BICUBIC;
  cfg.in_format = in_fmt; cfg.out_format = out_fmt; cfg.matrix = matrix; cfg.full_range = full;
  nvvsr_ctx *ctx = nullptr;
  T_CHECK_OK(nvvsr_create(&cfg, &ctx));
  if (!ctx) return;
  tu::Yuv420 src = make_frame(w, h);
  tu::InFrame in(src, in_fmt == NVVSR_IN_NV12);
  tu::OutFrame out(2 * w, 2 * h, out_fmt);
  T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
  nvvsr_destroy(ctx);

  const int bw = 2 * w / 4, bh = 2 * h / 4;
  for (int i = 0; i < 16; ++i) {
    const Patch &p = kPatches[i];
    int cx = (i % 4) * bw + bw / 2, cy = (i / 4) * bh + bh / 2;
    tu::Rgbd e = tu::yuv_to_rgb(p.y, p.u, p.v, bt709_expected, full);
    double er = tu::clamp255(e.r), eg = tu::clamp255(e.g), eb = tu::clamp255(e.b);
    if (out_fmt == NVVSR_OUT_I420) {
      double Y, U, V;
      tu::rgb_to_yuv(er, eg, eb, bt709_expected, full, Y, U, V);
      double gy = out.px(0, cx, cy), gu = out.px(1, cx / 2, cy / 2), gv = out.px(2, cx / 2, cy / 2);
      double d = std::max({std::fabs(gy - tu::clamp255(Y)), std::fabs(gu - tu::clamp255(U)), std::fabs(gv - tu::clamp255(V))});
      worst.yuv = std::max(worst.yuv, d);
      T_CHECK(d <= 3.0, "patch %d YUV(%d,%d,%d) %s %s: got (%.0f,%.0f,%.0f) expected (%.1f,%.1f,%.1f)", i, p.y, p.u, p.v,
              bt709_expected ? "709" : "601", full ? "full" : "lim", gy, gu, gv, tu::clamp255(Y), tu::clamp255(U), tu::clamp255(V));
    } else {
      double gr, gg, gb;
      if (out_fmt == NVVSR_OUT_RGB24_PLANAR) {
        gr = out.px(0, cx, cy); gg = out.px(1, cx, cy); gb = out.px(2, cx, cy);
      } else {
        gr = out.px(0, 4 * cx, cy); gg = out.px(0, 4 * cx + 1, cy); gb = out.px(0, 4 * cx + 2, cy);
        T_CHECK(out.px(0, 4 * cx + 3, cy) == 255, "alpha != 255 at patch %d", i);
      }
      double d = std::max({std::fabs(gr - er), std::fabs(gg - eg), std::fabs(gb - eb)});
      worst.rgb = std::max(worst.rgb, d);
      T_CHECK(d <= 3.0, "patch %d YUV(%d,%d,%d) %s %s: got RGB(%.0f,%.0f,%.0f) expected (%.1f,%.1f,%.1f)", i, p.y, p.u, p.v,
              bt709_expected ? "709" : "601", full ? "full" : "lim", gr, gg, gb, er, eg, eb);
    }
  }
}

int main() {
  if (int s = tu::skip_unless_usable("test_color")) return s;

  Worst worst;
  const int w = 160, h = 96;
  for (int m : {NVVSR_MATRIX_BT601, NVVSR_MATRIX_BT709})
    for (int full : {0, 1})
      for (int in_fmt : {NVVSR_IN_NV12, NVVSR_IN_I420})
        for (int out_fmt : {NVVSR_OUT_RGB24_PLANAR, NVVSR_OUT_RGBA_CHUNKY, NVVSR_OUT_I420}) {
          std::printf("  %s %-7s %s -> %s\n", m == NVVSR_MATRIX_BT601 ? "BT.601" : "BT.709", full ? "full" : "limited",
                      in_fmt == NVVSR_IN_NV12 ? "NV12" : "I420",
                      out_fmt == 0 ? "RGB24_PLANAR" : out_fmt == 1 ? "RGBA_CHUNKY" : "I420");
          run(m, full, in_fmt, out_fmt, w, h, m == NVVSR_MATRIX_BT709, worst);
        }
  std::printf("  worst deviation: RGB %.2f, YUV round trip %.2f (limit 3)\n", worst.rgb, worst.yuv);

  // matrix AUTO: BT.709 for 1280x720 and 1920x1080, BT.601 for 640x480 / 960x540 (w < 1280 and h <= 576)
  struct Auto { int w, h; bool bt709; } autos[] = {{1280, 720, true}, {640, 480, false}, {960, 540, false}, {704, 576, false}, {720, 640, true}};
  for (auto &a : autos) {
    std::printf("  AUTO matrix at %dx%d -> expect %s\n", a.w, a.h, a.bt709 ? "BT.709" : "BT.601");
    Worst wa;
    run(NVVSR_MATRIX_AUTO, false, NVVSR_IN_NV12, NVVSR_OUT_RGB24_PLANAR, a.w, a.h, a.bt709, wa);
  }
  return tu::finish("test_color");
}
