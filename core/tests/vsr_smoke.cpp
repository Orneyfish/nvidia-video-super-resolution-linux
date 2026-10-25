// Phase 0 smoke test + benchmark for the VFX "VideoSuperRes" effect, using only
// the vendored Maxine headers and the libs from the nvidia-vfx wheel (no CUDA toolkit).
//
//   vsr_smoke IN_W IN_H OUT_W OUT_H QUALITY [ITERS] [--state]
//   QUALITY: 0 bicubic, 1 low, 2 medium, 3 high, 4 ultra, 21 streaming_medium, 23 streaming_ultra
//   --state: check whether the output of a frame depends on previously processed frames
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../third_party/maxine/nvCVImage.h"
#include "../third_party/maxine/nvVideoEffects.h"

#define CHECK(call)                                                              \
  do {                                                                           \
    NvCV_Status s_ = (call);                                                     \
    if (s_ != NVCV_SUCCESS) {                                                    \
      std::fprintf(stderr, "%s:%d %s -> %d (%s)\n", __FILE__, __LINE__, #call,   \
                   (int)s_, NvCV_GetErrorStringFromCode(s_));                    \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

// Exported by the SDK 1.3 libVideoFX.so but missing from the v0.7.6 public headers.
extern "C" NvCV_Status NvVFX_CudaStreamSynchronize(CUstream stream);

static const char *kSelector = "VideoSuperRes";
static const char *kQuality = "QualityLevel";

struct Effect {
  NvVFX_Handle h = nullptr;
  CUstream stream = nullptr;
  NvCVImage srcGpu{}, dstGpu{}, tmp{};
  unsigned iw, ih, ow, oh;

  Effect(unsigned iw_, unsigned ih_, unsigned ow_, unsigned oh_, unsigned quality)
      : iw(iw_), ih(ih_), ow(ow_), oh(oh_) {
    CHECK(NvVFX_CreateEffect(kSelector, &h));
    CHECK(NvVFX_CudaStreamCreate(&stream));
    CHECK(NvVFX_SetCudaStream(h, NVVFX_CUDA_STREAM, stream));
    CHECK(NvCVImage_Alloc(&srcGpu, iw, ih, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1));
    CHECK(NvCVImage_Alloc(&dstGpu, ow, oh, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1));
    CHECK(NvVFX_SetImage(h, NVVFX_INPUT_IMAGE, &srcGpu));
    CHECK(NvVFX_SetImage(h, NVVFX_OUTPUT_IMAGE, &dstGpu));
    CHECK(NvVFX_SetU32(h, kQuality, quality));
    CHECK(NvVFX_Load(h));
  }
  ~Effect() {
    if (h) NvVFX_DestroyEffect(h);
    NvCVImage_Dealloc(&srcGpu);
    NvCVImage_Dealloc(&dstGpu);
    NvCVImage_Dealloc(&tmp);
    if (stream) NvVFX_CudaStreamDestroy(stream);
  }
  void upload(const NvCVImage &cpu) { CHECK(NvCVImage_Transfer(&cpu, &srcGpu, 1.0f, stream, &tmp)); }
  void run() {
    CHECK(NvVFX_Run(h, 0));
    CHECK(NvVFX_CudaStreamSynchronize(stream));
  }
  void download(NvCVImage &cpu) {
    CHECK(NvCVImage_Transfer(&dstGpu, &cpu, 1.0f, stream, &tmp));
    CHECK(NvVFX_CudaStreamSynchronize(stream));
  }
};

// Synthetic frame with edges, gradients and fine texture so SR has something to do.
static void fill(NvCVImage &im, int seed) {
  for (unsigned y = 0; y < im.height; ++y) {
    unsigned char *p = (unsigned char *)im.pixels + (size_t)y * im.pitch;
    for (unsigned x = 0; x < im.width; ++x) {
      int chk = (((x + seed * 7) >> 4) ^ (y >> 4)) & 1;
      int tex = (int)(20 * std::sin(0.9 * x + seed) * std::sin(0.7 * y));
      int base = chk ? 200 : 40;
      p[4 * x + 0] = (unsigned char)std::fmin(255, std::fmax(0, base + tex));
      p[4 * x + 1] = (unsigned char)((x * 255) / im.width);
      p[4 * x + 2] = (unsigned char)((y * 255) / im.height);
      p[4 * x + 3] = 255;
    }
  }
}

static double ms_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

int main(int argc, char **argv) {
  if (argc < 6) {
    std::fprintf(stderr, "usage: %s IN_W IN_H OUT_W OUT_H QUALITY [ITERS] [--state]\n", argv[0]);
    return 2;
  }
  unsigned iw = atoi(argv[1]), ih = atoi(argv[2]), ow = atoi(argv[3]), oh = atoi(argv[4]);
  unsigned q = atoi(argv[5]);
  int iters = argc > 6 && argv[6][0] != '-' ? atoi(argv[6]) : 100;
  bool state = false;
  for (int i = 6; i < argc; ++i) state |= !std::strcmp(argv[i], "--state");

  NvCVImage srcA{}, srcB{}, outA{}, outB{};
  CHECK(NvCVImage_Alloc(&srcA, iw, ih, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU_PINNED, 1));
  CHECK(NvCVImage_Alloc(&srcB, iw, ih, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU_PINNED, 1));
  CHECK(NvCVImage_Alloc(&outA, ow, oh, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU_PINNED, 1));
  CHECK(NvCVImage_Alloc(&outB, ow, oh, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU_PINNED, 1));
  fill(srcA, 1);
  fill(srcB, 2);

  auto t0 = std::chrono::steady_clock::now();
  Effect fx(iw, ih, ow, oh, q);
  double load_ms = ms_since(t0);

  fx.upload(srcA);
  for (int i = 0; i < 5; ++i) fx.run();  // warmup

  // Time the SR call alone (sync run), then the full upload+run+download round trip.
  t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) fx.run();
  double run_ms = ms_since(t0) / iters;

  t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    fx.upload(i & 1 ? srcB : srcA);
    fx.run();
    fx.download(outA);
  }
  double rt_ms = ms_since(t0) / iters;

  // Output sanity: not constant, not all zero.
  fx.upload(srcA);
  fx.run();
  fx.download(outA);
  unsigned char mn = 255, mx = 0;
  double sum = 0;
  for (unsigned y = 0; y < oh; ++y) {
    const unsigned char *p = (const unsigned char *)outA.pixels + (size_t)y * outA.pitch;
    for (unsigned x = 0; x < ow * 4; ++x) {
      mn = std::fmin(mn, p[x]);
      mx = std::fmax(mx, p[x]);
      sum += p[x];
    }
  }

  std::printf("q=%-2u %ux%u -> %ux%u  load %.0f ms | SR %.2f ms | round trip %.2f ms | out min %u max %u mean %.1f",
              q, iw, ih, ow, oh, load_ms, run_ms, rt_ms, mn, mx, sum / ((double)ow * oh * 4));

  if (state) {
    // fx has just processed A (and many frames before). Process B on it, and on a fresh effect.
    fx.upload(srcB);
    fx.run();
    fx.download(outA);
    Effect fresh(iw, ih, ow, oh, q);
    fresh.upload(srcB);
    fresh.run();
    fresh.download(outB);
    long long diff = 0, maxd = 0;
    for (unsigned y = 0; y < oh; ++y) {
      const unsigned char *a = (const unsigned char *)outA.pixels + (size_t)y * outA.pitch;
      const unsigned char *b = (const unsigned char *)outB.pixels + (size_t)y * outB.pitch;
      for (unsigned x = 0; x < ow * 4; ++x) {
        long long d = std::abs((int)a[x] - (int)b[x]);
        diff += d;
        if (d > maxd) maxd = d;
      }
    }
    std::printf(" | state: mean|diff| vs fresh = %.4f (max %lld)", (double)diff / ((double)ow * oh * 4), maxd);
  }
  std::printf("\n");
  return 0;
}
