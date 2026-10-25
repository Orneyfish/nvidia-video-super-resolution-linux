// Test 3: quality vs ground truth. A synthetic 1280x720 scene (thin shapes, edges, texture) is the
// ground truth; it is box-downscaled 2x on the CPU to 640x360 NV12, upscaled 2x by libnvvsr at several
// qualities and by CPU bilinear / bicubic, and the luma is compared against the ground truth.
// SR networks are trained on natural images, so on synthetic content they may not beat bicubic:
// the numbers are printed honestly and the assertions are conservative sanity bounds.
#include "test_util.h"

static tu::Plane run_sr(const tu::Yuv420 &lo, int quality, bool &ok) {
  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = lo.w; cfg.in_h = lo.h; cfg.out_w = lo.w * 2; cfg.out_h = lo.h * 2;
  cfg.quality = quality; cfg.in_format = NVVSR_IN_NV12; cfg.out_format = NVVSR_OUT_I420;
  cfg.matrix = NVVSR_MATRIX_BT709; cfg.full_range = 0;
  nvvsr_ctx *ctx = nullptr;
  ok = nvvsr_create(&cfg, &ctx) == NVVSR_OK;
  T_CHECK(ok, "create q=%d: %s", quality, nvvsr_last_error());
  if (!ok) return tu::Plane();
  tu::InFrame in(lo, true);
  tu::OutFrame out(cfg.out_w, cfg.out_h, NVVSR_OUT_I420);
  T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
  nvvsr_destroy(ctx);
  return out.plane_copy(0);
}

int main() {
  if (int s = tu::skip_unless_usable("test_quality")) return s;

  const int W = 1280, H = 720;
  tu::Yuv420 gt = tu::render_scene(W, H, 7);
  tu::Yuv420 lo = tu::box_down2(gt);  // 640x360
  const int border = 6;               // ignore frame edges (padding behaviour differs between resamplers)

  tu::Plane bil = tu::resize(lo.y, W, H, 0);
  tu::Plane bic = tu::resize(lo.y, W, H, 1);
  double p_bil = tu::psnr(gt.y, bil, border), p_bic = tu::psnr(gt.y, bic, border);
  std::printf("  %-28s PSNR %6.2f dB   MAE %5.2f\n", "CPU bilinear", p_bil, tu::mae(gt.y, bil, border));
  std::printf("  %-28s PSNR %6.2f dB   MAE %5.2f\n", "CPU bicubic (Keys a=-0.5)", p_bic, tu::mae(gt.y, bic, border));

  struct Q { int q; const char *name; } qs[] = {{NVVSR_QUALITY_BICUBIC, "NVVSR BICUBIC (q0)"}, {NVVSR_QUALITY_LOW, "NVVSR LOW (q1)"},
                                                {NVVSR_QUALITY_MEDIUM, "NVVSR MEDIUM (q2)"},   {NVVSR_QUALITY_HIGH, "NVVSR HIGH (q3)"},
                                                {NVVSR_QUALITY_ULTRA, "NVVSR ULTRA (q4)"},     {NVVSR_QUALITY_STREAMING_MEDIUM, "NVVSR STREAMING_MEDIUM"}};
  double p_gpu_bicubic = 0, p_high = 0;
  for (auto &q : qs) {
    bool ok;
    tu::Plane y = run_sr(lo, q.q, ok);
    if (!ok) continue;
    double p = tu::psnr(gt.y, y, border);
    std::printf("  %-28s PSNR %6.2f dB   MAE %5.2f   (vs CPU bilinear %+5.2f dB, vs CPU bicubic %+5.2f dB)\n", q.name, p,
                tu::mae(gt.y, y, border), p - p_bil, p - p_bic);
    if (q.q == NVVSR_QUALITY_BICUBIC) {
      p_gpu_bicubic = p;
      // pipeline sanity: the GPU bicubic path (via RGB and back) must be in the same ballpark as the CPU bicubic
      T_CHECK(std::fabs(p - p_bic) < 3.0, "GPU bicubic (%.2f dB) far from CPU bicubic (%.2f dB): conversion path suspect", p, p_bic);
    }
    if (q.q == NVVSR_QUALITY_HIGH) p_high = p;
    T_CHECK(p > 18.0, "%s: PSNR %.2f dB is implausibly low", q.name, p);
  }
  // Conservative: SR HIGH must not be much worse than plain bilinear on this content.
  T_CHECK(p_high >= p_bil - 1.0, "SR HIGH (%.2f dB) is worse than CPU bilinear (%.2f dB) by more than 1 dB", p_high, p_bil);
  std::printf("  summary: HIGH %+5.2f dB vs bilinear, %+5.2f dB vs bicubic, %+5.2f dB vs GPU bicubic\n", p_high - p_bil, p_high - p_bic,
              p_high - p_gpu_bicubic);
  return tu::finish("test_quality");
}
