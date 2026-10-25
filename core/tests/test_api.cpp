// GPU API semantics: create/process/set_quality/timing/alloc_host/ABI sizes/bad frames.
#include <cstring>
#include <string>

#include "test_util.h"

// Output of one frame through a fresh context.
static std::vector<uint8_t> one_frame(const tu::Yuv420 &src, int ow, int oh, int quality, bool nv12 = true) {
  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = src.w; cfg.in_h = src.h; cfg.out_w = ow; cfg.out_h = oh; cfg.quality = quality;
  cfg.in_format = nv12 ? NVVSR_IN_NV12 : NVVSR_IN_I420;
  nvvsr_ctx *ctx = nullptr;
  T_CHECK_OK(nvvsr_create(&cfg, &ctx));
  std::vector<uint8_t> all;
  if (!ctx) return all;
  tu::InFrame in(src, nv12);
  tu::OutFrame out(ow, oh, NVVSR_OUT_RGB24_PLANAR);
  T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
  nvvsr_destroy(ctx);
  for (int k = 0; k < 3; ++k) all.insert(all.end(), out.buf[k].begin(), out.buf[k].begin() + (size_t)ow * oh);
  return all;
}

// NVVSR_CACHE=0: every context loads its own effect. Changing the quality with set_quality()
// (no NvVFX_Load) must give exactly what an effect loaded at that quality gives.
static int mode_nocache() {
  setenv("NVVSR_CACHE", "0", 1);
  if (int s = tu::skip_unless_usable("test_api nocache")) return s;
  tu::Yuv420 src = tu::render_scene(640, 360, 5);
  tu::InFrame in(src, true);
  const int qs[] = {NVVSR_QUALITY_BICUBIC, NVVSR_QUALITY_LOW, NVVSR_QUALITY_MEDIUM, NVVSR_QUALITY_HIGH,
                    NVVSR_QUALITY_ULTRA, NVVSR_QUALITY_STREAMING_MEDIUM, NVVSR_QUALITY_STREAMING_ULTRA};
  std::vector<std::vector<uint8_t>> ref;
  for (int q : qs) ref.push_back(one_frame(src, 1280, 720, q));  // loaded at q
  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = 640; cfg.in_h = 360; cfg.out_w = 1280; cfg.out_h = 720; cfg.quality = NVVSR_QUALITY_HIGH;  // loaded at HIGH
  nvvsr_ctx *ctx = nullptr;
  T_CHECK_OK(nvvsr_create(&cfg, &ctx));
  if (!ctx) return tu::finish("test_api nocache");
  for (size_t i = 0; i < ref.size(); ++i) {
    T_CHECK_OK(nvvsr_set_quality(ctx, qs[i]));
    tu::OutFrame out(1280, 720, NVVSR_OUT_RGB24_PLANAR);
    T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
    std::vector<uint8_t> all;
    for (int k = 0; k < 3; ++k) all.insert(all.end(), out.buf[k].begin(), out.buf[k].begin() + 1280 * 720);
    T_CHECK(all == ref[i], "set_quality(%d) without reload differs from an effect loaded at that quality", qs[i]);
  }
  nvvsr_destroy(ctx);
  return tu::finish("test_api nocache");
}

int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "nocache") return mode_nocache();
  if (int s = tu::skip_unless_usable("test_api")) return s;

  const int W = 640, H = 360;
  tu::Yuv420 src = tu::render_scene(W, H, 3);

  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = W; cfg.in_h = H; cfg.out_w = 2 * W; cfg.out_h = 2 * H;
  cfg.quality = NVVSR_QUALITY_AUTO;
  cfg.flags = NVVSR_FLAG_TIMING;
  nvvsr_ctx *ctx = nullptr;
  double t0 = tu::now_ms();
  T_CHECK_OK(nvvsr_create(&cfg, &ctx));
  std::printf("  create %.0f ms\n", tu::now_ms() - t0);
  if (!ctx) return tu::finish("test_api");
  T_CHECK(nvvsr_get_quality(ctx) == NVVSR_QUALITY_ULTRA, "AUTO at 360p should resolve to ULTRA, got %d", nvvsr_get_quality(ctx));

  tu::InFrame in(src, true);
  tu::OutFrame out(2 * W, 2 * H, NVVSR_OUT_RGB24_PLANAR);
  T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
  nvvsr_timing tm{};
  T_CHECK_OK(nvvsr_last_timing(ctx, &tm));
  std::printf("  timing: upload %.2f convert %.2f sr %.2f download %.2f total %.2f ms\n", tm.upload_ms, tm.convert_ms, tm.sr_ms,
              tm.download_ms, tm.total_ms);
  T_CHECK(tm.total_ms > 0 && tm.sr_ms > 0 && tm.upload_ms > 0 && tm.download_ms > 0 && tm.convert_ms > 0, "timing fields filled");
  T_CHECK(tm.upload_ms + tm.convert_ms + tm.sr_ms + tm.download_ms <= tm.total_ms * 1.05 + 0.1, "stage sum exceeds total");
  std::vector<uint8_t> first = out.buf[0];

  // --- set_quality: no-op when unchanged, reload when changed, output changes
  t0 = tu::now_ms();
  T_CHECK_OK(nvvsr_set_quality(ctx, nvvsr_get_quality(ctx)));
  double same_ms = tu::now_ms() - t0;
  T_CHECK(same_ms < 20.0, "set_quality to the current value must not reload (%.1f ms)", same_ms);
  t0 = tu::now_ms();
  T_CHECK_OK(nvvsr_set_quality(ctx, NVVSR_QUALITY_BICUBIC));
  std::printf("  set_quality(BICUBIC): %.0f ms (same value: %.2f ms)\n", tu::now_ms() - t0, same_ms);
  T_CHECK(nvvsr_get_quality(ctx) == NVVSR_QUALITY_BICUBIC, "quality not updated");
  tu::OutFrame out2(2 * W, 2 * H, NVVSR_OUT_RGB24_PLANAR);
  T_CHECK_OK(nvvsr_process(ctx, &in.img, &out2.img));
  T_CHECK(out2.buf[0] != first, "output identical after changing the quality");
  T_CHECK(nvvsr_set_quality(ctx, 7) == NVVSR_ERR_INVALID_ARG, "invalid quality accepted");
  T_CHECK(nvvsr_get_quality(ctx) == NVVSR_QUALITY_BICUBIC, "invalid set_quality changed the state");
  T_CHECK_OK(nvvsr_set_quality(ctx, NVVSR_QUALITY_AUTO));
  T_CHECK(nvvsr_get_quality(ctx) == NVVSR_QUALITY_ULTRA, "AUTO via set_quality");
  tu::OutFrame out3(2 * W, 2 * H, NVVSR_OUT_RGB24_PLANAR);
  T_CHECK_OK(nvvsr_process(ctx, &in.img, &out3.img));
  T_CHECK(out3.buf[0] == first, "stateless: output after going back to the same quality must equal the first one");

  // --- bad frames are rejected without touching the GPU, ctx stays usable
  nvvsr_image_in bad = in.img;
  bad.plane[1] = nullptr;
  T_CHECK(nvvsr_process(ctx, &bad, &out.img) == NVVSR_ERR_INVALID_ARG, "NULL UV plane");
  bad = in.img; bad.stride[0] = W - 1;
  T_CHECK(nvvsr_process(ctx, &bad, &out.img) == NVVSR_ERR_INVALID_ARG, "stride too small");
  bad = in.img; bad.stride[1] = 0;
  T_CHECK(nvvsr_process(ctx, &bad, &out.img) == NVVSR_ERR_INVALID_ARG, "stride 0");
  nvvsr_image_out badout = out.img;
  badout.plane[2] = nullptr;
  T_CHECK(nvvsr_process(ctx, &in.img, &badout) == NVVSR_ERR_INVALID_ARG, "NULL B plane");
  badout = out.img; badout.stride[1] = 100;
  T_CHECK(nvvsr_process(ctx, &in.img, &badout) == NVVSR_ERR_INVALID_ARG, "out stride too small");
  T_CHECK(nvvsr_process(ctx, nullptr, &out.img) == NVVSR_ERR_INVALID_ARG, "NULL in");
  T_CHECK(nvvsr_process(ctx, &in.img, nullptr) == NVVSR_ERR_INVALID_ARG, "NULL out");
  T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
  T_CHECK(out.buf[0] == first, "ctx output changed after rejected calls");
  nvvsr_destroy(ctx);

  // --- ABI: smaller (older) and larger (newer) struct_size
  nvvsr_config small_cfg = cfg;
  small_cfg.struct_size = offsetof(nvvsr_config, flags);  // oldest accepted layout: no flags/fps
  small_cfg.flags = 0xFFFFFFFF;  // beyond struct_size: must be ignored
  ctx = nullptr;
  T_CHECK_OK(nvvsr_create(&small_cfg, &ctx));
  if (ctx) {
    T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
    nvvsr_timing t2{};
    nvvsr_last_timing(ctx, &t2);
    T_CHECK(t2.sr_ms == 0.0 && t2.total_ms > 0.0, "flags beyond struct_size must be ignored (no stage timing)");
    nvvsr_destroy(ctx);
  }
  struct Big { nvvsr_config c; char future[64]; } big;
  std::memset(&big, 0xCC, sizeof big);
  big.c = cfg;
  big.c.struct_size = sizeof big;  // newer client with extra fields: ignored by this lib
  ctx = nullptr;
  T_CHECK_OK(nvvsr_create(&big.c, &ctx));
  nvvsr_destroy(ctx);

  // --- pinned memory helper
  uint8_t *pin = static_cast<uint8_t *>(nvvsr_alloc_host(8 << 20));
  T_CHECK(pin != nullptr, "alloc_host returned NULL");
  if (pin) {
    std::memset(pin, 7, 8 << 20);
    nvvsr_free_host(pin);
  }
  nvvsr_free_host(reinterpret_cast<void *>(0x1234));  // unknown pointer is ignored
  T_CHECK(nvvsr_alloc_host(0) == nullptr, "alloc_host(0)");

  // --- process into pinned buffers equals process into pageable buffers
  nvvsr_config c2 = cfg;
  c2.quality = NVVSR_QUALITY_MEDIUM;
  c2.flags = 0;
  T_CHECK_OK(nvvsr_create(&c2, &ctx));
  if (ctx) {
    size_t plane = (size_t)2 * W * 2 * H;
    uint8_t *pp = static_cast<uint8_t *>(nvvsr_alloc_host(plane * 3));
    T_CHECK(pp != nullptr, "pinned output");
    if (pp) {
      nvvsr_image_out po{};
      for (int k = 0; k < 3; ++k) { po.plane[k] = pp + k * plane; po.stride[k] = 2 * W; }
      tu::OutFrame pg(2 * W, 2 * H, NVVSR_OUT_RGB24_PLANAR);
      T_CHECK_OK(nvvsr_process(ctx, &in.img, &po));
      T_CHECK_OK(nvvsr_process(ctx, &in.img, &pg.img));
      bool same = true;
      for (int k = 0; k < 3; ++k) same &= std::memcmp(po.plane[k], pg.buf[k].data(), plane) == 0;
      T_CHECK(same, "pinned vs pageable output differ");
      nvvsr_free_host(pp);
    }
    nvvsr_destroy(ctx);
  }

  // --- effect recycling: a context built on a parked effect that served other shapes/qualities
  //     produces exactly what a freshly loaded effect produces.
  nvvsr_trim();  // empty the cache so the first context below really gets a new effect
  tu::Yuv420 sa = tu::render_scene(640, 360, 21), sb = tu::render_scene(854, 480, 22);
  std::vector<uint8_t> fresh_a = one_frame(sa, 1280, 720, NVVSR_QUALITY_HIGH);       // new effect
  std::vector<uint8_t> via_b = one_frame(sb, 2560, 1440, NVVSR_QUALITY_ULTRA);        // recycled, other shape + quality
  std::vector<uint8_t> again_a = one_frame(sa, 1280, 720, NVVSR_QUALITY_HIGH, false); // recycled again (I420 input)
  T_CHECK(!fresh_a.empty() && fresh_a == again_a, "recycled effect output differs from the fresh one");
  T_CHECK(!via_b.empty(), "recycled context at another shape failed");
  // creating a context from a recycled effect is cheap
  {
    nvvsr_config cfg;
    nvvsr_config_init(&cfg);
    cfg.in_w = 640; cfg.in_h = 360; cfg.out_w = 1280; cfg.out_h = 720; cfg.quality = NVVSR_QUALITY_HIGH;
    double t = tu::now_ms();
    nvvsr_ctx *c = nullptr;
    T_CHECK_OK(nvvsr_create(&cfg, &c));
    double ms = tu::now_ms() - t;
    std::printf("  create from a recycled effect: %.1f ms\n", ms);
    T_CHECK(ms < 60.0, "recycled create took %.1f ms", ms);
    nvvsr_destroy(c);
  }
  nvvsr_trim();
  return tu::finish("test_api");
}
