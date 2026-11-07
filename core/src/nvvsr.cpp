// libnvvsr: public C API implementation (see include/nvvsr.h).
#include <pthread.h>
#include "../include/nvvsr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#include "loader.h"

using nvvsr::api;
using nvvsr::map_nvcv;
using nvvsr::set_error;

namespace {

// ---------------------------------------------------------------------------
// NvCVImage helpers. The vendored header gives NvCVImage inline C++ constructors that
// call NvCVImage_Alloc/Dealloc by name; libnvvsr must not reference those symbols at
// link time, so images live in a union that never runs them and are filled through the
// dlsym'ed function table instead.
// ---------------------------------------------------------------------------
union RawImg {
  NvCVImage im;
  RawImg() { std::memset(static_cast<void *>(&im), 0, sizeof im); }
  ~RawImg() {}
};

// An owning GPU/host image (NvCVImage_Alloc / NvCVImage_Dealloc).
class Img {
 public:
  Img() = default;
  Img(const Img &) = delete;
  Img &operator=(const Img &) = delete;
  ~Img() { release(); }

  int alloc(unsigned w, unsigned h, NvCVImage_PixelFormat fmt, NvCVImage_ComponentType type, unsigned layout,
            unsigned mem, unsigned align, const char *what) {
    release();
    int rc = map_nvcv(api().ImgAlloc(&u_.im, w, h, fmt, type, layout, mem, align), what);
    if (rc == NVVSR_OK) allocated_ = true;
    else std::memset(static_cast<void *>(&u_.im), 0, sizeof u_.im);
    return rc;
  }
  void release() {
    if (allocated_) {
      api().ImgDealloc(&u_.im);
      allocated_ = false;
    }
    std::memset(static_cast<void *>(&u_.im), 0, sizeof u_.im);
  }
  NvCVImage *get() { return &u_.im; }
  const NvCVImage *get() const { return &u_.im; }

 private:
  RawImg u_;
  bool allocated_ = false;
};

using Clock = std::chrono::steady_clock;
double ms_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

bool valid_quality(int q) { return q == 0 || q == 1 || q == 2 || q == 3 || q == 4 || q == 21 || q == 23; }

int resolve_matrix(const nvvsr_config &c) {
  if (c.matrix == NVVSR_MATRIX_BT601 || c.matrix == NVVSR_MATRIX_BT709) return c.matrix;
  return (c.in_w >= 1280 || c.in_h > 576) ? NVVSR_MATRIX_BT709 : NVVSR_MATRIX_BT601;
}

const char *kSelector = "VideoSuperRes";  // not in the vendored headers
const char *kQualityParam = "QualityLevel";
const char *kStrengthParam = "Strength";

}  // namespace

// ---------------------------------------------------------------------------
// Engine = one SDK effect handle + its CUDA stream. Engines are recycled instead of
// destroyed (see nvvsr.h, "Effect cache"), because NvVFX in SDK 1.3 leaks:
//   * ~12.5 MiB of HOST memory on every NvVFX_Load, and
//   * ~215 MiB of GPU memory (weights + workspace) for every effect that was alive at the
//     same time as another one, when it is destroyed.
// Measured with raw-SDK programs (no libnvvsr); reproduced by `test_stress leak_nocache` and
// `test_stress vram` (NVVSR_CACHE=0 shows the uncached behaviour). A recycled
// engine needs neither Load nor a new handle: quality and image size can be changed on a
// loaded effect (verified bit-identical to a fresh effect).
// ---------------------------------------------------------------------------
struct Engine {
  NvVFX_Handle fx = nullptr;
  CUstream stream = nullptr;
  bool loaded = false;
  int in_w = 0, in_h = 0, out_w = 0, out_h = 0;  // shape served last (cache lookup hint)
};

void engine_destroy(Engine *e) {
  if (!e) return;
  const nvvsr::Api &a = api();
  if (e->stream) a.CudaStreamSynchronize(e->stream);
  if (e->fx) a.DestroyEffect(e->fx);
  if (e->stream) a.CudaStreamDestroy(e->stream);
  delete e;
}

class EngineCache {
 public:
  static EngineCache &get() {
    static EngineCache *c = new EngineCache();  // never destroyed: CUDA may already be gone at exit
    return *c;
  }
  size_t max_size() const { return max_; }
  Engine *take(const nvvsr_config &c) {
    std::lock_guard<std::mutex> lk(mu_);
    int best = -1;
    for (size_t i = 0; i < parked_.size(); ++i) {
      const Engine *e = parked_[i];
      if (e->in_w == c.in_w && e->in_h == c.in_h && e->out_w == c.out_w && e->out_h == c.out_h) { best = (int)i; break; }
      if (best < 0) best = (int)i;
    }
    if (best < 0) return nullptr;
    Engine *e = parked_[best];
    parked_.erase(parked_.begin() + best);
    return e;
  }
  bool has_shape(const nvvsr_config &c) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const Engine *e : parked_)
      if (e->in_w == c.in_w && e->in_h == c.in_h && e->out_w == c.out_w && e->out_h == c.out_h) return true;
    return false;
  }
  // true if the engine was parked, false if the caller must destroy it
  bool put(Engine *e) {
    std::lock_guard<std::mutex> lk(mu_);
    if (parked_.size() >= max_) return false;
    parked_.push_back(e);
    return true;
  }
  size_t trim() {
    std::vector<Engine *> victims;
    {
      std::lock_guard<std::mutex> lk(mu_);
      victims.swap(parked_);
    }
    for (Engine *e : victims) engine_destroy(e);
    return victims.size();
  }

 private:
  EngineCache() {
    const char *off = std::getenv("NVVSR_CACHE");
    if (off && off[0] == '0') max_ = 0;
    const char *mx = std::getenv("NVVSR_CACHE_MAX");
    if (mx && *mx) max_ = (size_t)std::max(0, std::atoi(mx));
  }
  std::mutex mu_;
  std::vector<Engine *> parked_;
  size_t max_ = 8;
};

// The SDK allocates its workspace lazily inside NvVFX_Run and does not survive a failed cudaMalloc
// there: it ends in "illegal memory access", a sticky CUDA error that poisons the whole process.
// So refuse up front when the free VRAM cannot hold the (measured) cost of another context:
//   effect workspace ~ 250 B per input pixel (480p: 153 MiB, 1080p: 457 MiB, 4K: ~2 GB) unless a
//   parked effect already holds this shape, plus our frame buffers, plus a safety margin.
int check_vram(const nvvsr_config &c) {
  const nvvsr::Api &a = api();
  if (!a.MemGetInfo) return NVVSR_OK;
  size_t free_b = 0, total_b = 0;
  if (a.MemGetInfo(&free_b, &total_b) != 0) return NVVSR_OK;  // cannot tell; let the SDK try
  const double ip = (double)c.in_w * c.in_h, op = (double)c.out_w * c.out_h;
  double need = ip * 1.5 + ip * 4 + op * 4 + op * 3;       // our buffers
  if (!EngineCache::get().has_shape(c)) need += ip * 300;  // effect workspace + weights
  need += 128.0 * 1024 * 1024;                             // margin (driver, cuDNN/NGX scratch, other apps)
  if (need > (double)free_b) {
    set_error("not enough free GPU memory: ~%.0f MiB needed, %.0f MiB free of %.0f MiB", need / 1048576.0, free_b / 1048576.0,
              total_b / 1048576.0);
    return NVVSR_ERR_OUT_OF_MEMORY;
  }
  return NVVSR_OK;
}

int engine_acquire(const nvvsr_config &c, Engine **out) {
  *out = EngineCache::get().take(c);
  if (*out) return NVVSR_OK;
  const nvvsr::Api &a = api();
  std::unique_ptr<Engine> e(new Engine());
  int rc = map_nvcv(a.CreateEffect(kSelector, &e->fx), "NvVFX_CreateEffect(VideoSuperRes)");
  if (rc != NVVSR_OK) { e->fx = nullptr; return rc; }
  rc = map_nvcv(a.CudaStreamCreate(&e->stream), "NvVFX_CudaStreamCreate");
  if (rc != NVVSR_OK) { e->stream = nullptr; engine_destroy(e.release()); return rc; }
  rc = map_nvcv(a.SetCudaStream(e->fx, NVVFX_CUDA_STREAM, e->stream), "NvVFX_SetCudaStream");
  if (rc != NVVSR_OK) { engine_destroy(e.release()); return rc; }
  *out = e.release();
  return NVVSR_OK;
}

// `healthy`: the engine completed its last frame (or setup) without error.
void engine_release(Engine *e, bool healthy) {
  if (!e) return;
  const nvvsr::Api &a = api();
  if (healthy && e->loaded) {
    a.CudaStreamSynchronize(e->stream);
    a.SetImage(e->fx, NVVFX_INPUT_IMAGE, nullptr);  // drop the dangling descriptors of freed buffers
    a.SetImage(e->fx, NVVFX_OUTPUT_IMAGE, nullptr);
    if (EngineCache::get().put(e)) return;
  }
  engine_destroy(e);
}

// ---------------------------------------------------------------------------
// the context
// ---------------------------------------------------------------------------
struct nvvsr_ctx {
  nvvsr_config cfg{};
  int quality = 0;

  Engine *eng = nullptr;  // effect + stream, borrowed from / returned to the engine cache
  NvVFX_Handle fx = nullptr;
  CUstream stream = nullptr;
  bool healthy = true;    // false after a failure that may have left the effect in a bad state

  // GPU buffers, allocated once. A future 2-stream pipeline would give each in-flight
  // frame its own copy of this group (and its own effect handle bound to it).
  Img yuvIn;     // YUV420 8-bit, NV12 or I420 layout, in_w x in_h
  Img rgbaIn;    // chunky RGBA8, in_w x in_h  (SR input)
  Img rgbaOut;   // chunky RGBA8, out_w x out_h (SR output)
  Img planarOut; // planar RGB8, out_w x out_h (RGB24_PLANAR only)
  Img yuvOut;    // YUV420 I420, out_w x out_h (I420 output only)
  Img tmp;       // scratch image for Transfer (stays empty for the paths used here)

  unsigned char *yIn = nullptr, *uIn = nullptr, *vIn = nullptr;
  int yInPitch = 0, cInPitch = 0;
  unsigned char *yOut = nullptr, *uOut = nullptr, *vOut = nullptr;
  int yOutPitch = 0, cOutPitch = 0;

  nvvsr_timing timing{};

  ~nvvsr_ctx() { teardown(); }

  int init(const nvvsr_config &c);
  int process(const nvvsr_image_in &in, const nvvsr_image_out &out);
  int set_quality(int q);

 private:
  void teardown() {
    if (stream) api().CudaStreamSynchronize(stream);
    // Buffers go first (the effect must not point at freed memory), then the engine is
    // recycled (or destroyed if it is suspect / the cache is full or disabled).
    yuvIn.release();
    rgbaIn.release();
    rgbaOut.release();
    planarOut.release();
    yuvOut.release();
    tmp.release();
    engine_release(eng, healthy);
    eng = nullptr;
    fx = nullptr;
    stream = nullptr;
  }

  int sync(const char *what) { return map_nvcv(api().CudaStreamSynchronize(stream), what); }

  int warmup() {
    int rc = map_nvcv(api().Run(fx, 1), "NvVFX_Run (warm-up)");
    int src = sync("stream sync (warm-up)");
    return rc != NVVSR_OK ? rc : src;
  }

  // Copy a block of bytes between a host plane and a GPU plane, both viewed as 8-bit
  // single-channel images (NvCVImage_Transfer degenerates to cudaMemcpy2DAsync).
  int copy_plane(bool to_gpu, void *host, int host_stride, void *dev, int dev_pitch, unsigned wbytes, unsigned rows,
                 const char *what) {
    RawImg h, d;
    const nvvsr::Api &a = api();
    int rc = map_nvcv(a.ImgInit(&h.im, wbytes, rows, host_stride, host, NVCV_Y, NVCV_U8, NVCV_CHUNKY, NVCV_CPU), what);
    if (rc != NVVSR_OK) return rc;
    rc = map_nvcv(a.ImgInit(&d.im, wbytes, rows, dev_pitch, dev, NVCV_Y, NVCV_U8, NVCV_CHUNKY, NVCV_GPU), what);
    if (rc != NVVSR_OK) return rc;
    return to_gpu ? map_nvcv(a.ImgTransfer(&h.im, &d.im, 1.0f, stream, tmp.get()), what)
                  : map_nvcv(a.ImgTransfer(&d.im, &h.im, 1.0f, stream, tmp.get()), what);
  }

  int fill_alpha(Img &gpu_rgba, unsigned w, unsigned h) {
    // Pre-fill the whole image with 0xFF through a one-off host copy, so alpha is 255
    // and stays so (YUV->RGBA conversion and the effect leave alpha alone / write 255).
    Img host;
    int rc = host.alloc(w, h, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_CPU, 1, "alloc alpha staging");
    if (rc != NVVSR_OK) return rc;
    std::memset(host.get()->pixels, 0xFF, (size_t)host.get()->pitch * h);
    rc = map_nvcv(api().ImgTransfer(host.get(), gpu_rgba.get(), 1.0f, stream, tmp.get()), "upload alpha");
    if (rc != NVVSR_OK) return rc;
    return sync("sync alpha");
  }
};

int nvvsr_ctx::init(const nvvsr_config &c) {
  const nvvsr::Api &a = api();
  cfg = c;
  cfg.matrix = resolve_matrix(c);
  quality = c.quality == NVVSR_QUALITY_AUTO ? nvvsr_auto_quality(c.in_h, c.fps) : c.quality;

  const unsigned iw = (unsigned)c.in_w, ih = (unsigned)c.in_h, ow = (unsigned)c.out_w, oh = (unsigned)c.out_h;
  const bool nv12 = c.in_format == NVVSR_IN_NV12;
  int rc;

  rc = check_vram(c);
  if (rc != NVVSR_OK) return rc;
  rc = engine_acquire(c, &eng);
  if (rc != NVVSR_OK) return rc;
  fx = eng->fx;
  stream = eng->stream;
  healthy = true;  // cleared when an SDK call on the effect fails

  // Input side: host YUV planes -> GPU YUV420 image -> GPU RGBA.
  rc = yuvIn.alloc(iw, ih, NVCV_YUV420, NVCV_U8, nv12 ? NVCV_NV12 : NVCV_I420, NVCV_GPU, 0, "alloc input YUV");
  if (rc != NVVSR_OK) return rc;
  const unsigned char colorspace = (unsigned char)((cfg.matrix == NVVSR_MATRIX_BT709 ? NVCV_709 : NVCV_601) |
                                                   (c.full_range ? NVCV_FULL_RANGE : NVCV_VIDEO_RANGE) | NVCV_CHROMA_COSITED);
  yuvIn.get()->colorspace = colorspace;
  {
    int ypb, cpb;
    rc = map_nvcv(a.ImgGetYUVPointers(yuvIn.get(), &yIn, &uIn, &vIn, &ypb, &cpb, &yInPitch, &cInPitch), "GetYUVPointers(in)");
    if (rc != NVVSR_OK) return rc;
  }
  rc = rgbaIn.alloc(iw, ih, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1, "alloc SR input");
  if (rc != NVVSR_OK) return rc;
  rc = rgbaOut.alloc(ow, oh, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1, "alloc SR output");
  if (rc != NVVSR_OK) return rc;

  // Output side.
  if (c.out_format == NVVSR_OUT_RGB24_PLANAR) {
    rc = planarOut.alloc(ow, oh, NVCV_RGB, NVCV_U8, NVCV_PLANAR, NVCV_GPU, 0, "alloc output planar RGB");
    if (rc != NVVSR_OK) return rc;
  } else if (c.out_format == NVVSR_OUT_I420) {
    rc = yuvOut.alloc(ow, oh, NVCV_YUV420, NVCV_U8, NVCV_I420, NVCV_GPU, 0, "alloc output YUV");
    if (rc != NVVSR_OK) return rc;
    yuvOut.get()->colorspace = colorspace;
    int ypb, cpb;
    rc = map_nvcv(a.ImgGetYUVPointers(yuvOut.get(), &yOut, &uOut, &vOut, &ypb, &cpb, &yOutPitch, &cOutPitch),
                  "GetYUVPointers(out)");
    if (rc != NVVSR_OK) return rc;
  }

  rc = fill_alpha(rgbaIn, iw, ih);
  if (rc != NVVSR_OK) return rc;
  rc = fill_alpha(rgbaOut, ow, oh);
  if (rc != NVVSR_OK) return rc;

  rc = map_nvcv(a.SetImage(fx, NVVFX_INPUT_IMAGE, rgbaIn.get()), "NvVFX_SetImage(src)");
  if (rc == NVVSR_OK) rc = map_nvcv(a.SetImage(fx, NVVFX_OUTPUT_IMAGE, rgbaOut.get()), "NvVFX_SetImage(dst)");
  if (rc == NVVSR_OK) rc = map_nvcv(a.SetU32(fx, kQualityParam, (unsigned)quality), "NvVFX_SetU32(QualityLevel)");
  if (rc == NVVSR_OK) {
    const float strength = c.strength < 0.f ? 0.4f : c.strength;
    // Strength is not meaningful in every mode; a rejection is not fatal.
    (void)a.SetF32(fx, kStrengthParam, strength);
    if (!eng->loaded) {  // a recycled engine is already loaded: no reload, no host-memory leak
      rc = map_nvcv(a.Load(fx), "NvVFX_Load");
      if (rc == NVVSR_OK) eng->loaded = true;
    }
  }
  // One frame (white, from fill_alpha) so every lazily allocated SDK buffer exists before create returns:
  // an out-of-memory condition then surfaces here and not in the middle of playback.
  if (rc == NVVSR_OK) rc = warmup();
  if (rc != NVVSR_OK) {
    healthy = false;
    return rc;
  }
  eng->in_w = c.in_w; eng->in_h = c.in_h; eng->out_w = c.out_w; eng->out_h = c.out_h;
  return NVVSR_OK;
}

int nvvsr_ctx::set_quality(int q) {
  if (q == NVVSR_QUALITY_AUTO) q = nvvsr_auto_quality(cfg.in_h, cfg.fps);
  if (!valid_quality(q)) {
    set_error("invalid quality %d", q);
    return NVVSR_ERR_INVALID_ARG;
  }
  if (q == quality) return NVVSR_OK;
  const nvvsr::Api &a = api();
  // No NvVFX_Load: the effect dispatches on QualityLevel at Run time (verified: the output is
  // bit-identical to a freshly loaded effect), and every Load leaks host memory in SDK 1.3.
  // A higher tier may allocate more workspace on its first frame, so check the free VRAM first and
  // run that first frame now, where a failure can still be reported (and undone).
  if (a.MemGetInfo) {
    size_t free_b = 0, total_b = 0;
    double need = (double)cfg.in_w * cfg.in_h * 300 + 128.0 * 1024 * 1024;
    if (a.MemGetInfo(&free_b, &total_b) == 0 && need > (double)free_b) {
      set_error("not enough free GPU memory to switch quality: ~%.0f MiB needed, %.0f MiB free", need / 1048576.0,
                free_b / 1048576.0);
      return NVVSR_ERR_OUT_OF_MEMORY;
    }
  }
  int rc = map_nvcv(a.SetU32(fx, kQualityParam, (unsigned)q), "NvVFX_SetU32(QualityLevel)");
  if (rc == NVVSR_OK) rc = warmup();
  if (rc == NVVSR_OK) {
    quality = q;
    return NVVSR_OK;
  }
  std::string why = nvvsr::last_error();
  if (a.SetU32(fx, kQualityParam, (unsigned)quality) != NVCV_SUCCESS) healthy = false;
  nvvsr::set_error_str(why);
  return rc;
}

int nvvsr_ctx::process(const nvvsr_image_in &in, const nvvsr_image_out &out) {
  const nvvsr::Api &a = api();
  const bool timing_on = (cfg.flags & NVVSR_FLAG_TIMING) != 0;
  const unsigned iw = (unsigned)cfg.in_w, ih = (unsigned)cfg.in_h, ow = (unsigned)cfg.out_w, oh = (unsigned)cfg.out_h;
  const bool nv12 = cfg.in_format == NVVSR_IN_NV12;

  timing = nvvsr_timing{};
  const auto t_start = Clock::now();
  auto t_mark = t_start;
  auto lap = [&](double &field) -> int {
    if (!timing_on) return NVVSR_OK;
    int r = sync("stream sync (timing)");
    auto now = Clock::now();
    field += ms_between(t_mark, now);
    t_mark = now;
    return r;
  };

  int rc = NVVSR_OK;
  // Everything below is queued on one stream; there is a single synchronisation at the end.
  do {
    // 1. upload
    rc = copy_plane(true, const_cast<uint8_t *>(in.plane[0]), in.stride[0], yIn, yInPitch, iw, ih, "upload Y");
    if (rc != NVVSR_OK) break;
    if (nv12) {
      rc = copy_plane(true, const_cast<uint8_t *>(in.plane[1]), in.stride[1], uIn, cInPitch, iw, ih / 2, "upload UV");
    } else {
      rc = copy_plane(true, const_cast<uint8_t *>(in.plane[1]), in.stride[1], uIn, cInPitch, iw / 2, ih / 2, "upload U");
      if (rc == NVVSR_OK)
        rc = copy_plane(true, const_cast<uint8_t *>(in.plane[2]), in.stride[2], vIn, cInPitch, iw / 2, ih / 2, "upload V");
    }
    if (rc != NVVSR_OK) break;
    if ((rc = lap(timing.upload_ms)) != NVVSR_OK) break;

    // 2. YUV -> RGBA
    double conv_in = 0, conv_out = 0;
    rc = map_nvcv(a.ImgTransfer(yuvIn.get(), rgbaIn.get(), 1.0f, stream, tmp.get()), "convert YUV->RGBA");
    if (rc != NVVSR_OK) break;
    if ((rc = lap(conv_in)) != NVVSR_OK) break;

    // 3. super resolution
    rc = map_nvcv(a.Run(fx, 1), "NvVFX_Run");
    if (rc != NVVSR_OK) break;
    if ((rc = lap(timing.sr_ms)) != NVVSR_OK) break;

    // 4. RGBA -> output format
    switch (cfg.out_format) {
      case NVVSR_OUT_RGB24_PLANAR:
        rc = map_nvcv(a.ImgTransfer(rgbaOut.get(), planarOut.get(), 1.0f, stream, tmp.get()), "convert RGBA->planar RGB");
        break;
      case NVVSR_OUT_I420:
        rc = map_nvcv(a.ImgTransfer(rgbaOut.get(), yuvOut.get(), 1.0f, stream, tmp.get()), "convert RGBA->I420");
        break;
      default:
        break;  // RGBA_CHUNKY: download straight from the SR output
    }
    if (rc != NVVSR_OK) break;
    if ((rc = lap(conv_out)) != NVVSR_OK) break;
    timing.convert_ms = conv_in + conv_out;

    // 5. download
    switch (cfg.out_format) {
      case NVVSR_OUT_RGB24_PLANAR: {
        const NvCVImage *p = planarOut.get();
        for (int k = 0; k < 3 && rc == NVVSR_OK; ++k) {
          void *dev = static_cast<unsigned char *>(p->pixels) + (size_t)k * p->pitch * oh;
          rc = copy_plane(false, out.plane[k], out.stride[k], dev, p->pitch, ow, oh, "download plane");
        }
        break;
      }
      case NVVSR_OUT_RGBA_CHUNKY:
        rc = copy_plane(false, out.plane[0], out.stride[0], rgbaOut.get()->pixels, rgbaOut.get()->pitch, ow * 4, oh,
                        "download RGBA");
        break;
      case NVVSR_OUT_I420:
        rc = copy_plane(false, out.plane[0], out.stride[0], yOut, yOutPitch, ow, oh, "download Y");
        if (rc == NVVSR_OK) rc = copy_plane(false, out.plane[1], out.stride[1], uOut, cOutPitch, ow / 2, oh / 2, "download U");
        if (rc == NVVSR_OK) rc = copy_plane(false, out.plane[2], out.stride[2], vOut, cOutPitch, ow / 2, oh / 2, "download V");
        break;
      default:
        rc = NVVSR_ERR_INTERNAL;
    }
    if (rc != NVVSR_OK) break;
  } while (false);

  // The only synchronisation of the normal path (also on failure: no GPU work may
  // outlive this call, because the host buffers belong to the caller).
  int src = sync("stream sync");
  if (rc == NVVSR_OK) rc = src;
  if (timing_on && rc == NVVSR_OK) {
    timing.download_ms = ms_between(t_mark, Clock::now());
  }
  timing.total_ms = ms_between(t_start, Clock::now());
  if (rc != NVVSR_OK && rc != NVVSR_ERR_INVALID_ARG) healthy = false;  // do not recycle the effect after a GPU error
  return rc;
}

// ---------------------------------------------------------------------------
// validation
// ---------------------------------------------------------------------------
namespace {

int check_config(const nvvsr_config *cfg, nvvsr_config &c) {
  if (!cfg) {
    set_error("config is NULL");
    return NVVSR_ERR_INVALID_ARG;
  }
  const size_t min_size = offsetof(nvvsr_config, flags);
  if (cfg->struct_size < min_size) {
    set_error("config.struct_size %u too small (call nvvsr_config_init)", (unsigned)cfg->struct_size);
    return NVVSR_ERR_INVALID_ARG;
  }
  nvvsr_config_init(&c);
  std::memcpy(&c, cfg, cfg->struct_size < sizeof c ? cfg->struct_size : sizeof c);
  c.struct_size = (uint32_t)sizeof c;

  if (c.in_w <= 0 || c.in_h <= 0 || c.out_w <= 0 || c.out_h <= 0) {
    set_error("sizes must be positive (%dx%d -> %dx%d)", c.in_w, c.in_h, c.out_w, c.out_h);
    return NVVSR_ERR_INVALID_ARG;
  }
  if (c.in_w < 16 || c.in_h < 16 || c.in_w > 8192 || c.in_h > 8192 || c.out_w > 16384 || c.out_h > 16384) {
    set_error("size out of range: input 16..8192, output up to 16384 (%dx%d -> %dx%d)", c.in_w, c.in_h, c.out_w, c.out_h);
    return NVVSR_ERR_UNSUPPORTED;
  }
  if ((c.in_w & 1) || (c.in_h & 1)) {
    set_error("odd input size %dx%d is not supported (4:2:0 needs even dimensions)", c.in_w, c.in_h);
    return NVVSR_ERR_UNSUPPORTED;
  }
  if (c.out_w < c.in_w || c.out_h < c.in_h || c.out_w > 4 * c.in_w || c.out_h > 4 * c.in_h) {
    set_error("output %dx%d must be between 1x and 4x the input %dx%d", c.out_w, c.out_h, c.in_w, c.in_h);
    return NVVSR_ERR_UNSUPPORTED;
  }
  if (c.quality != NVVSR_QUALITY_AUTO && !valid_quality(c.quality)) {
    set_error("invalid quality %d", c.quality);
    return NVVSR_ERR_INVALID_ARG;
  }
  if (std::isnan(c.strength) || c.strength > 1.0f) {
    set_error("strength must be in 0..1 (or negative for the default)");
    return NVVSR_ERR_INVALID_ARG;
  }
  switch (c.in_format) {
    case NVVSR_IN_NV12:
    case NVVSR_IN_I420:
      break;
    case NVVSR_IN_P010:
    case NVVSR_IN_I42010:
      set_error("10-bit input formats are not implemented yet");
      return NVVSR_ERR_UNSUPPORTED;
    default:
      set_error("invalid input format %d", c.in_format);
      return NVVSR_ERR_INVALID_ARG;
  }
  switch (c.out_format) {
    case NVVSR_OUT_RGB24_PLANAR:
    case NVVSR_OUT_RGBA_CHUNKY:
      break;
    case NVVSR_OUT_I420:
      if ((c.out_w & 1) || (c.out_h & 1)) {
        set_error("I420 output needs even output dimensions (%dx%d)", c.out_w, c.out_h);
        return NVVSR_ERR_UNSUPPORTED;
      }
      break;
    case NVVSR_OUT_RGB48_PLANAR:
    case NVVSR_OUT_I42010:
      set_error("16-bit/10-bit output formats are not implemented yet");
      return NVVSR_ERR_UNSUPPORTED;
    default:
      set_error("invalid output format %d", c.out_format);
      return NVVSR_ERR_INVALID_ARG;
  }
  switch (c.matrix) {
    case NVVSR_MATRIX_AUTO:
    case NVVSR_MATRIX_BT601:
    case NVVSR_MATRIX_BT709:
      break;
    case NVVSR_MATRIX_BT2020:
      set_error("BT.2020 is not implemented yet");
      return NVVSR_ERR_UNSUPPORTED;
    default:
      set_error("invalid matrix %d", c.matrix);
      return NVVSR_ERR_INVALID_ARG;
  }
  c.full_range = c.full_range ? 1 : 0;
  return NVVSR_OK;
}

int check_plane(const void *p, int stride, size_t row_bytes, const char *name) {
  if (!p) {
    set_error("%s plane pointer is NULL", name);
    return NVVSR_ERR_INVALID_ARG;
  }
  if (stride <= 0 || (size_t)stride < row_bytes) {
    set_error("%s stride %d is smaller than the row size %zu", name, stride, row_bytes);
    return NVVSR_ERR_INVALID_ARG;
  }
  return NVVSR_OK;
}

int check_frames(const nvvsr_config &c, const nvvsr_image_in *in, const nvvsr_image_out *out) {
  int rc;
  const size_t iw = (size_t)c.in_w, ow = (size_t)c.out_w;
  if ((rc = check_plane(in->plane[0], in->stride[0], iw, "input Y")) != NVVSR_OK) return rc;
  if (c.in_format == NVVSR_IN_NV12) {
    if ((rc = check_plane(in->plane[1], in->stride[1], iw, "input UV")) != NVVSR_OK) return rc;
  } else {
    if ((rc = check_plane(in->plane[1], in->stride[1], iw / 2, "input U")) != NVVSR_OK) return rc;
    if ((rc = check_plane(in->plane[2], in->stride[2], iw / 2, "input V")) != NVVSR_OK) return rc;
  }
  switch (c.out_format) {
    case NVVSR_OUT_RGB24_PLANAR:
      for (int k = 0; k < 3; ++k)
        if ((rc = check_plane(out->plane[k], out->stride[k], ow, "output plane")) != NVVSR_OK) return rc;
      break;
    case NVVSR_OUT_RGBA_CHUNKY:
      if ((rc = check_plane(out->plane[0], out->stride[0], ow * 4, "output RGBA")) != NVVSR_OK) return rc;
      break;
    case NVVSR_OUT_I420:
      if ((rc = check_plane(out->plane[0], out->stride[0], ow, "output Y")) != NVVSR_OK) return rc;
      if ((rc = check_plane(out->plane[1], out->stride[1], ow / 2, "output U")) != NVVSR_OK) return rc;
      if ((rc = check_plane(out->plane[2], out->stride[2], ow / 2, "output V")) != NVVSR_OK) return rc;
      break;
  }
  return NVVSR_OK;
}

#define NVVSR_GUARD_BEGIN try {
#define NVVSR_GUARD_END                                   \
  }                                                       \
  catch (const std::bad_alloc &) {                        \
    set_error("host memory exhausted");                   \
    return NVVSR_ERR_OUT_OF_MEMORY;                       \
  }                                                       \
  catch (const std::exception &e) {                       \
    set_error("internal error: %s", e.what());            \
    return NVVSR_ERR_INTERNAL;                            \
  }                                                       \
  catch (...) {                                           \
    set_error("internal error (unknown exception)");      \
    return NVVSR_ERR_INTERNAL;                            \
  }

std::mutex g_host_mu;
// Never destroyed: a block the caller forgot to free must not touch CUDA during static destruction.
std::unordered_map<void *, std::unique_ptr<Img>> &host_blocks() {
  static auto *m = new std::unordered_map<void *, std::unique_ptr<Img>>();
  return *m;
}

}  // namespace

// ---------------------------------------------------------------------------
// exported C API
// ---------------------------------------------------------------------------
extern "C" {

NVVSR_API const char *nvvsr_strerror(int status) {
  switch (status) {
    case NVVSR_OK: return "ok";
    case NVVSR_ERR_INVALID_ARG: return "invalid argument";
    case NVVSR_ERR_UNSUPPORTED: return "unsupported configuration";
    case NVVSR_ERR_NO_LIBDIR: return "NVVSR_LIBDIR / ~/.config/nvvsr/libdir not configured";
    case NVVSR_ERR_LIBRARY: return "nvidia-vfx libraries missing or failed to load";
    case NVVSR_ERR_NO_GPU: return "no usable NVIDIA GPU or driver";
    case NVVSR_ERR_DRIVER: return "NVIDIA driver too old";
    case NVVSR_ERR_OUT_OF_MEMORY: return "out of memory";
    case NVVSR_ERR_EFFECT: return "video effect (SDK) call failed";
    case NVVSR_ERR_INTERNAL: return "internal error";
    default: return "unknown status";
  }
}

NVVSR_API const char *nvvsr_last_error(void) { return nvvsr::last_error(); }

NVVSR_API const char *nvvsr_version(void) { return "0.1.0"; }
NVVSR_API int nvvsr_api_version(void) { return NVVSR_API_VERSION; }

NVVSR_API void nvvsr_config_init(nvvsr_config *cfg) {
  if (!cfg) return;
  std::memset(cfg, 0, sizeof *cfg);
  cfg->struct_size = (uint32_t)sizeof *cfg;
  cfg->quality = NVVSR_QUALITY_AUTO;
  cfg->strength = -1.0f;
  cfg->in_format = NVVSR_IN_NV12;
  cfg->out_format = NVVSR_OUT_RGB24_PLANAR;
  cfg->matrix = NVVSR_MATRIX_AUTO;
  cfg->full_range = 0;
  cfg->flags = 0;
  cfg->fps = 0.0;
}

NVVSR_API int nvvsr_auto_quality(int in_h, double fps) {
  int q = in_h <= 540 ? NVVSR_QUALITY_ULTRA : (in_h <= 800 ? NVVSR_QUALITY_HIGH : NVVSR_QUALITY_MEDIUM);
  if (fps >= 50.0 && in_h > 1200) q = NVVSR_QUALITY_LOW;
  if (fps >= 100.0) {
    if (q == NVVSR_QUALITY_ULTRA) q = NVVSR_QUALITY_HIGH;
    else if (q == NVVSR_QUALITY_HIGH) q = NVVSR_QUALITY_MEDIUM;
    else if (q == NVVSR_QUALITY_MEDIUM) q = NVVSR_QUALITY_LOW;
  }
  return q;
}

NVVSR_API int nvvsr_probe(void) {
  NVVSR_GUARD_BEGIN
  return nvvsr::ensure_init();
  NVVSR_GUARD_END
}

namespace {

// NVIDIA's NGX initialisation (inside the first model load of a context) overflows thread stacks of 1 MiB or less
// and kills the host process with SIGSEGV; plugin hosts such as VLC run plugin code on exactly such stacks. So the
// whole initialisation runs on a short-lived thread with a stack that is large enough, whatever thread the caller is.
constexpr size_t kInitStackBytes = 8u << 20;

struct InitJob {
  nvvsr_ctx *ctx;
  const nvvsr_config *cfg;
  int rc;
  std::string err;
};

void *init_thread(void *p) {
  InitJob *j = static_cast<InitJob *>(p);
  try {
    j->rc = j->ctx->init(*j->cfg);
    j->err = nvvsr::last_error();
  } catch (const std::bad_alloc &) {
    j->rc = NVVSR_ERR_OUT_OF_MEMORY;
    j->err = "host memory exhausted";
  } catch (const std::exception &e) {
    j->rc = NVVSR_ERR_INTERNAL;
    j->err = std::string("internal error: ") + e.what();
  } catch (...) {
    j->rc = NVVSR_ERR_INTERNAL;
    j->err = "internal error (unknown exception)";
  }
  return nullptr;
}

int init_on_big_stack(nvvsr_ctx *ctx, const nvvsr_config &c) {
  InitJob job{ctx, &c, NVVSR_ERR_INTERNAL, std::string()};
  pthread_attr_t attr;
  pthread_t th;
  bool started = false;
  if (pthread_attr_init(&attr) == 0) {
    started = pthread_attr_setstacksize(&attr, kInitStackBytes) == 0 && pthread_create(&th, &attr, init_thread, &job) == 0;
    pthread_attr_destroy(&attr);
  }
  if (!started) {  // never init on the caller's stack: NGX segfaults on stacks <= 1 MiB, which is why this thread exists
    set_error("cannot start the init thread");
    return NVVSR_ERR_INTERNAL;
  }
  pthread_join(th, nullptr);
  if (job.rc != NVVSR_OK) nvvsr::set_error_str(job.err);  // the error text is thread-local: hand it to the caller
  return job.rc;
}

}  // namespace

NVVSR_API int nvvsr_create(const nvvsr_config *cfg, nvvsr_ctx **out) {
  NVVSR_GUARD_BEGIN
  if (!out) {
    set_error("out pointer is NULL");
    return NVVSR_ERR_INVALID_ARG;
  }
  *out = nullptr;
  nvvsr_config c;
  int rc = check_config(cfg, c);  // before touching the GPU
  if (rc != NVVSR_OK) return rc;
  rc = nvvsr::ensure_init();
  if (rc != NVVSR_OK) return rc;
  std::unique_ptr<nvvsr_ctx> ctx(new (std::nothrow) nvvsr_ctx());
  if (!ctx) {
    set_error("host memory exhausted");
    return NVVSR_ERR_OUT_OF_MEMORY;
  }
  rc = init_on_big_stack(ctx.get(), c);
  if (rc != NVVSR_OK) return rc;  // ~nvvsr_ctx releases whatever was allocated
  *out = ctx.release();
  return NVVSR_OK;
  NVVSR_GUARD_END
}

NVVSR_API int nvvsr_process(nvvsr_ctx *ctx, const nvvsr_image_in *in, const nvvsr_image_out *out) {
  NVVSR_GUARD_BEGIN
  if (!ctx || !in || !out) {
    set_error("NULL argument");
    return NVVSR_ERR_INVALID_ARG;
  }
  int rc = check_frames(ctx->cfg, in, out);
  if (rc != NVVSR_OK) return rc;
  return ctx->process(*in, *out);
  NVVSR_GUARD_END
}

NVVSR_API int nvvsr_set_quality(nvvsr_ctx *ctx, int quality) {
  NVVSR_GUARD_BEGIN
  if (!ctx) {
    set_error("ctx is NULL");
    return NVVSR_ERR_INVALID_ARG;
  }
  if (quality != NVVSR_QUALITY_AUTO && !valid_quality(quality)) {
    set_error("invalid quality %d", quality);
    return NVVSR_ERR_INVALID_ARG;
  }
  return ctx->set_quality(quality);
  NVVSR_GUARD_END
}

NVVSR_API int nvvsr_get_quality(const nvvsr_ctx *ctx) {
  if (!ctx) return NVVSR_ERR_INVALID_ARG;
  return ctx->quality;
}

NVVSR_API int nvvsr_last_timing(const nvvsr_ctx *ctx, nvvsr_timing *out) {
  if (!ctx || !out) {
    set_error("NULL argument");
    return NVVSR_ERR_INVALID_ARG;
  }
  *out = ctx->timing;
  return NVVSR_OK;
}

NVVSR_API void nvvsr_destroy(nvvsr_ctx *ctx) {
  try {
    delete ctx;
  } catch (...) {
  }
}

NVVSR_API void nvvsr_trim(void) {
  try {
    if (nvvsr::ensure_init() == NVVSR_OK) EngineCache::get().trim();
  } catch (...) {
  }
}

NVVSR_API void *nvvsr_alloc_host(size_t bytes) {
  try {
    if (bytes == 0 || bytes > 0xFFFFFFFFull || nvvsr::ensure_init() != NVVSR_OK) return nullptr;
    std::unique_ptr<Img> img(new Img());
    if (img->alloc((unsigned)bytes, 1, NVCV_Y, NVCV_U8, NVCV_CHUNKY, NVCV_CPU_PINNED, 1, "alloc pinned host memory") !=
        NVVSR_OK)
      return nullptr;
    void *p = img->get()->pixels;
    std::lock_guard<std::mutex> lk(g_host_mu);
    host_blocks()[p] = std::move(img);
    return p;
  } catch (...) {
    return nullptr;
  }
}

NVVSR_API void nvvsr_free_host(void *ptr) {
  if (!ptr) return;
  try {
    std::unique_ptr<Img> victim;
    {
      std::lock_guard<std::mutex> lk(g_host_mu);
      auto it = host_blocks().find(ptr);
      if (it == host_blocks().end()) return;
      victim = std::move(it->second);
      host_blocks().erase(it);
    }
    victim.reset();
  } catch (...) {
  }
}

}  // extern "C"
