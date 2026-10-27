// nvvsr VapourSynth plugin (API 4): GPU super resolution of 8-bit 4:2:0 video via libnvvsr.
//
//   core.nvvsr.Upscale(clip, width, height, quality, strength, matrix, range, pool, fps, format)
//   core.nvvsr.Auto(clip, display_w, display_h, dar_w, dar_h, fps, options)
//
// Input  : constant-format YUV420P8, even dimensions.
// Output : RGB24 (planar R,G,B, full range) by default, or YUV420P8 with format="yuv420p8" (same
//          matrix and range as the input; this is what mpv's --vf=vapoursynth accepts, it cannot
//          take RGB). Size width x height (default: the input size).
// Threads: fmParallel; a small pool of libnvvsr contexts (one per frame in flight). Contexts are
//          created lazily (the first one in Upscale(), so GPU/SDK problems surface as an ordinary
//          VapourSynth error there) and are freed with the filter.
// Errors : nothing here throws into VapourSynth or aborts; a failed frame becomes a frame error.
//          Upscale raises on bad arguments, unsupported input and unavailable SR/GPU; Auto is the caller that
//          turns every such case into pass-through.
//
// Auto   : the decision logic of mpv/vsr.vpy. Returns Upscale(clip, format="yuv420p8") sized to fit the display
//          (scale = min(display_w/dar_w, display_h/dar_h, 4), dar defaulting to the clip size, even sizes, never
//          below the source), or the input clip itself with one "nvvsr: ..., pass-through" line on stderr when the
//          clip is not constant 8-bit 4:2:0, the display size is unknown (arguments, else env NVVSR_DISPLAY=WxH),
//          nothing would be gained, `options` ("key=value;key=value": quality, strength, pool, matrix, range) is
//          malformed, or Upscale fails. It never raises.
//
// Only the public header core/include/nvvsr.h of libnvvsr is used.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "VSHelper4.h"
#include "VapourSynth4.h"
#include "nvvsr.h"

namespace {

// One context already saturates the GPU (measured 1080p->1440p MEDIUM: pool 1/2/3 = 200/189/187 fps) and each
// extra context costs ~250 MiB of VRAM, so the default is 1. Larger pools only help to hide stalls.
constexpr int kDefaultPool = 1;
constexpr int kMaxPool = 4;  // each extra context costs VRAM (frame buffers + SDK workspace)

// One libnvvsr context plus the colour settings it was created with (matrix and range are
// part of nvvsr_config, so a clip whose frame properties change them needs a re-create).
struct Slot {
  nvvsr_ctx *ctx = nullptr;
  bool busy = false;  // in use by a thread (also while it is being created)
  bool dead = false;  // creation failed while other slots work: the pool shrinks
  int matrix = -1;
  int full_range = -1;
};

struct Filter {
  VSNode *node = nullptr;
  VSVideoInfo vi{};  // output clip
  int in_w = 0, in_h = 0, out_w = 0, out_h = 0;
  int quality = NVVSR_QUALITY_AUTO;
  float strength = -1.f;
  double fps = 0;
  int forced_matrix = -1;  // nvvsr_matrix, or -1: from the _Matrix frame property
  int forced_range = -1;   // 0 = limited, 1 = full, or -1: from the _Range frame property
  int pool_size = kDefaultPool;
  bool out_yuv = false;  // format="yuv420p8" instead of RGB24

  std::mutex mu;
  std::condition_variable cv;
  std::vector<Slot> slots;
  ~Filter() {
    for (Slot &s : slots) nvvsr_destroy(s.ctx);  // parks the SDK effect for reuse (see nvvsr.h); NULL is fine
  }
#ifdef NVVSR_VS_TESTHOOK
  std::atomic<int> fail_after{-1};  // test only: fail every frame once this many were processed
  std::atomic<int> processed{0};
#endif
};

std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return buf;
}

std::string status_text(int rc) {
  std::string s = nvvsr_strerror(rc);
  const char *detail = nvvsr_last_error();
  if (detail && *detail) s += std::string(" (") + detail + ")";
  return s;
}

nvvsr_config make_config(const Filter &d, int matrix, int full_range) {
  nvvsr_config c;
  nvvsr_config_init(&c);
  c.in_w = d.in_w;
  c.in_h = d.in_h;
  c.out_w = d.out_w;
  c.out_h = d.out_h;
  c.quality = d.quality;
  c.strength = d.strength;
  c.in_format = NVVSR_IN_I420;  // VapourSynth YUV420P8 is planar Y,U,V
  c.out_format = d.out_yuv ? NVVSR_OUT_I420 : NVVSR_OUT_RGB24_PLANAR;
  c.matrix = matrix;
  c.full_range = full_range;
  c.fps = d.fps;
  return c;
}

int create_ctx(const Filter &d, int matrix, int full_range, nvvsr_ctx **out, std::string *err) {
  const nvvsr_config cfg = make_config(d, matrix, full_range);
  int rc = nvvsr_create(&cfg, out);
  if (rc != NVVSR_OK) {
    *out = nullptr;
    *err = "nvvsr_create failed: " + status_text(rc);
  }
  return rc;
}

// Colour settings of one frame: the arguments override the frame properties. The matrix comes from
// `_Matrix`, else from `_ColorSpace` (the older name, which mpv sets): 1 = BT.709, 5/6 = BT.601;
// anything else (unspecified, BT.2020, ...) selects the size rule of nvvsr (709 from 1280 wide / 576 high).
// The range comes from `_Range`, else from the older, inverted `_ColorRange`.
// *matrix_prop is the VapourSynth `_Matrix` value to report on a YUV output frame.
void frame_color(const Filter &d, const VSMap *props, const VSAPI *vsapi, int *matrix, int *full_range, int *matrix_prop) {
  int err = 0;
  int64_t m = -1;
  if (d.forced_matrix >= 0) {
    *matrix = d.forced_matrix;
  } else {
    m = vsapi->mapGetInt(props, "_Matrix", 0, &err);
    if (err) m = vsapi->mapGetInt(props, "_ColorSpace", 0, &err);
    *matrix = err ? NVVSR_MATRIX_AUTO : m == 1 ? NVVSR_MATRIX_BT709 : (m == 5 || m == 6) ? NVVSR_MATRIX_BT601 : NVVSR_MATRIX_AUTO;
    if (err) m = -1;
  }
  const bool is709 = *matrix == NVVSR_MATRIX_BT709 || (*matrix == NVVSR_MATRIX_AUTO && (d.in_w >= 1280 || d.in_h > 576));
  *matrix_prop = (m == 1 || m == 5 || m == 6) ? (int)m : is709 ? 1 : 6;
  if (d.forced_range >= 0) {
    *full_range = d.forced_range;
  } else {
    // R81 and later: `_Range` (1 = full, 0 = limited). Older: `_ColorRange` (0 = full, 1 = limited), which
    // is also what mpv sets. Limited when absent.
    int64_t r = vsapi->mapGetInt(props, "_Range", 0, &err);
    if (err) {
      r = vsapi->mapGetInt(props, "_ColorRange", 0, &err);
      r = (r == 0) ? 1 : 0;
    }
    *full_range = (!err && r == 1) ? 1 : 0;
  }
}

// Take a context that matches (matrix, full_range); blocks while all of them are busy.
// Returns the slot index, or -1 with *err set when no context can be had at all.
int acquire(Filter &d, int matrix, int full_range, std::string *err) {
  std::unique_lock<std::mutex> lk(d.mu);
  for (;;) {
    int exact = -1, other = -1, empty = -1;
    for (size_t i = 0; i < d.slots.size(); ++i) {
      Slot &s = d.slots[i];
      if (s.dead) continue;
      if (s.busy) continue;
      if (s.ctx && s.matrix == matrix && s.full_range == full_range) {
        if (exact < 0) exact = (int)i;
      } else if (s.ctx) {
        if (other < 0) other = (int)i;
      } else if (empty < 0) {
        empty = (int)i;
      }
    }
    if (exact >= 0) {
      d.slots[exact].busy = true;
      return exact;
    }
    // A free slot to (re)build: prefer an empty one (new context), else recolour an idle one.
    const int build = empty >= 0 ? empty : other;
    if (build < 0) {  // everything is busy (or being built): wait for a release
      d.cv.wait(lk);
      continue;
    }
    Slot &s = d.slots[build];
    s.busy = true;
    nvvsr_ctx *old = s.ctx;
    s.ctx = nullptr;
    lk.unlock();
    if (old) nvvsr_destroy(old);
    nvvsr_ctx *fresh = nullptr;
    std::string why;
    const int rc = create_ctx(d, matrix, full_range, &fresh, &why);
    lk.lock();
    if (rc == NVVSR_OK) {
      s.ctx = fresh;
      s.matrix = matrix;
      s.full_range = full_range;
      return build;  // stays busy
    }
    s.busy = false;
    bool others = false;
    for (size_t i = 0; i < d.slots.size(); ++i)
      if ((int)i != build && !d.slots[i].dead && (d.slots[i].ctx || d.slots[i].busy)) others = true;
    if (!others) {  // nothing to fall back on: report the failure (the next frame retries)
      d.cv.notify_all();
      *err = why;
      return -1;
    }
    s.dead = true;  // the pool shrinks; the remaining contexts carry on
    std::fprintf(stderr, "nvvsr: pool shrunk, extra context failed: %s\n", why.c_str());
    d.cv.notify_all();
  }
}

void release(Filter &d, int idx) {
  {
    std::lock_guard<std::mutex> lk(d.mu);
    d.slots[idx].busy = false;
  }
  d.cv.notify_all();
}

const VSFrame *VS_CC getFrame(int n, int activationReason, void *instanceData, void **, VSFrameContext *frameCtx,
                              VSCore *core, const VSAPI *vsapi) {
  Filter *d = static_cast<Filter *>(instanceData);
  if (activationReason == arInitial) {
    vsapi->requestFrameFilter(n, d->node, frameCtx);
    return nullptr;
  }
  if (activationReason != arAllFramesReady) return nullptr;

  const VSFrame *src = vsapi->getFrameFilter(n, d->node, frameCtx);
  VSFrame *dst = nullptr;
  try {
    if (vsapi->getFrameWidth(src, 0) != d->in_w || vsapi->getFrameHeight(src, 0) != d->in_h) {
      vsapi->setFilterError("Upscale: input frame size changed", frameCtx);
      vsapi->freeFrame(src);
      return nullptr;
    }
    dst = vsapi->newVideoFrame(&d->vi.format, d->out_w, d->out_h, src, core);  // copies the frame props too

    nvvsr_image_in in{};
    nvvsr_image_out out{};
    for (int p = 0; p < 3; ++p) {
      in.plane[p] = vsapi->getReadPtr(src, p);
      in.stride[p] = (int32_t)vsapi->getStride(src, p);
      out.plane[p] = vsapi->getWritePtr(dst, p);
      out.stride[p] = (int32_t)vsapi->getStride(dst, p);
    }

    int matrix = NVVSR_MATRIX_AUTO, full_range = 0, matrix_prop = 0;
    frame_color(*d, vsapi->getFramePropertiesRO(src), vsapi, &matrix, &full_range, &matrix_prop);

    std::string why;
    const int idx = acquire(*d, matrix, full_range, &why);
    if (idx < 0) {
      vsapi->setFilterError(("Upscale: " + why).c_str(), frameCtx);
      vsapi->freeFrame(src);
      vsapi->freeFrame(dst);
      return nullptr;
    }
    int rc;
#ifdef NVVSR_VS_TESTHOOK
    if (d->fail_after.load() >= 0 && d->processed.fetch_add(1) >= d->fail_after.load()) rc = NVVSR_ERR_INTERNAL;
    else
#endif
    rc = nvvsr_process(d->slots[idx].ctx, &in, &out);  // the slot is ours until release()
    if (rc != NVVSR_OK) why = "nvvsr_process failed: " + status_text(rc);
    release(*d, idx);
    if (rc != NVVSR_OK) {
      vsapi->setFilterError(("Upscale: " + why).c_str(), frameCtx);
      vsapi->freeFrame(src);
      vsapi->freeFrame(dst);
      return nullptr;
    }

    VSMap *props = vsapi->getFramePropertiesRW(dst);
    if (d->out_yuv) {  // same colour space as the source; libnvvsr writes left-sited (MPEG-2) chroma
      vsapi->mapSetInt(props, "_Matrix", matrix_prop, maReplace);
      vsapi->mapSetInt(props, "_Range", full_range ? 1 : 0, maReplace);
      vsapi->mapSetInt(props, "_ChromaLocation", 0, maReplace);
    } else {
      vsapi->mapSetInt(props, "_Matrix", 0, maReplace);  // RGB
      vsapi->mapSetInt(props, "_Range", 1, maReplace);   // full range
      vsapi->mapDeleteKey(props, "_ChromaLocation");         // no chroma subsampling any more
    }
    vsapi->freeFrame(src);
    return dst;
  } catch (const std::exception &e) {
    vsapi->setFilterError((std::string("Upscale: internal error: ") + e.what()).c_str(), frameCtx);
  } catch (...) {
    vsapi->setFilterError("Upscale: internal error", frameCtx);
  }
  vsapi->freeFrame(src);
  vsapi->freeFrame(dst);  // NULL is accepted
  return nullptr;
}

void VS_CC freeFilter(void *instanceData, VSCore *, const VSAPI *vsapi) {
  Filter *d = static_cast<Filter *>(instanceData);
  vsapi->freeNode(d->node);
  delete d;
}

bool iequals(const std::string &a, const char *b) {
  size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}

bool parse_quality(const std::string &s, int *q) {
  static const struct { const char *name; int q; } tab[] = {
      {"auto", NVVSR_QUALITY_AUTO},       {"bicubic", NVVSR_QUALITY_BICUBIC},
      {"low", NVVSR_QUALITY_LOW},         {"medium", NVVSR_QUALITY_MEDIUM},
      {"high", NVVSR_QUALITY_HIGH},       {"ultra", NVVSR_QUALITY_ULTRA},
      {"streaming_medium", NVVSR_QUALITY_STREAMING_MEDIUM}, {"streaming_ultra", NVVSR_QUALITY_STREAMING_ULTRA}};
  for (const auto &t : tab)
    if (iequals(s, t.name)) {
      *q = t.q;
      return true;
    }
  return false;
}

bool parse_matrix(const std::string &s, int *m) {
  if (iequals(s, "auto")) *m = -1;  // from the frame properties
  else if (iequals(s, "709") || iequals(s, "bt709")) *m = NVVSR_MATRIX_BT709;
  else if (iequals(s, "601") || iequals(s, "bt601") || iequals(s, "470bg") || iequals(s, "170m")) *m = NVVSR_MATRIX_BT601;
  else return false;
  return true;
}

// Nearest even integer to x (x > 0).
int even_round(double x) { return std::max(2, 2 * (int)std::lround(x / 2.0)); }

void VS_CC upscaleCreate(const VSMap *in, VSMap *out, void *, VSCore *core, const VSAPI *vsapi) {
  VSNode *node = vsapi->mapGetNode(in, "clip", 0, nullptr);
  auto fail = [&](const std::string &msg) {
    vsapi->mapSetError(out, ("Upscale: " + msg).c_str());
    vsapi->freeNode(node);
  };
  std::unique_ptr<Filter> fd;  // freed on every early return until VapourSynth takes it over
  try {
    const VSVideoInfo *ivi = vsapi->getVideoInfo(node);
    const VSVideoFormat &f = ivi->format;
    if (!vsh::isConstantVideoFormat(ivi)) return fail("the input must have a constant format and size");
    if (f.colorFamily != cfYUV || f.sampleType != stInteger || f.bitsPerSample != 8 || f.subSamplingW != 1 || f.subSamplingH != 1) {
      char name[32] = "unknown";
      vsapi->getVideoFormatName(&f, name);
      return fail(format("unsupported input format %s: only YUV420P8 is supported", name));
    }
    if ((ivi->width & 1) || (ivi->height & 1)) return fail(format("the input size %dx%d must be even", ivi->width, ivi->height));

    int err = 0;
    int64_t w = vsapi->mapGetInt(in, "width", 0, &err);
    if (err) w = 0;
    int64_t h = vsapi->mapGetInt(in, "height", 0, &err);
    if (err) h = 0;
    if (w < 0 || h < 0 || w > 16384 || h > 16384) return fail("width and height must be between 0 and 16384");

    fd.reset(new Filter());
    Filter *d = fd.get();
    d->in_w = ivi->width;
    d->in_h = ivi->height;
    if (w == 0 && h == 0) {
      d->out_w = d->in_w;
      d->out_h = d->in_h;
    } else if (h == 0) {
      d->out_w = (int)w;
      d->out_h = even_round((double)w * d->in_h / d->in_w);
    } else if (w == 0) {
      d->out_h = (int)h;
      d->out_w = even_round((double)h * d->in_w / d->in_h);
    } else {
      d->out_w = (int)w;
      d->out_h = (int)h;
    }
    if (d->out_w < d->in_w || d->out_h < d->in_h)
      return fail(format("downscaling is not supported (%dx%d -> %dx%d)", d->in_w, d->in_h, d->out_w, d->out_h));
    if (d->out_w > 4 * d->in_w || d->out_h > 4 * d->in_h)
      return fail(format("at most 4x upscaling is supported (%dx%d -> %dx%d)", d->in_w, d->in_h, d->out_w, d->out_h));

    const char *s = vsapi->mapGetData(in, "quality", 0, &err);
    if (!err && s) {
      int len = vsapi->mapGetDataSize(in, "quality", 0, nullptr);
      if (!parse_quality(std::string(s, (size_t)std::max(len, 0)), &d->quality))
        return fail("quality must be auto, bicubic, low, medium, high, ultra, streaming_medium or streaming_ultra");
    }
    double st = vsapi->mapGetFloat(in, "strength", 0, &err);
    if (!err) {
      if (!(st >= 0.0 && st <= 1.0)) return fail("strength must be between 0 and 1");
      d->strength = (float)st;
    }
    s = vsapi->mapGetData(in, "matrix", 0, &err);
    if (!err && s) {
      int len = vsapi->mapGetDataSize(in, "matrix", 0, nullptr);
      if (!parse_matrix(std::string(s, (size_t)std::max(len, 0)), &d->forced_matrix))
        return fail("matrix must be auto, 709 or 601");
    }
    int64_t range = vsapi->mapGetInt(in, "range", 0, &err);
    if (!err) {
      if (range != 0 && range != 1) return fail("range must be 0 (full) or 1 (limited)");
      d->forced_range = range == 0 ? 1 : 0;  // internal: 1 = full
    }
    s = vsapi->mapGetData(in, "format", 0, &err);
    if (!err && s) {
      std::string fmt(s, (size_t)std::max(vsapi->mapGetDataSize(in, "format", 0, nullptr), 0));
      if (iequals(fmt, "yuv420p8")) d->out_yuv = true;
      else if (!iequals(fmt, "rgb24")) return fail("format must be rgb24 or yuv420p8");
      if (d->out_yuv && ((d->out_w & 1) || (d->out_h & 1))) return fail("format yuv420p8 needs an even output size");
    }
    int64_t pool = vsapi->mapGetInt(in, "pool", 0, &err);
    if (!err) {
      if (pool < 1 || pool > kMaxPool) return fail(format("pool must be between 1 and %d", kMaxPool));
      d->pool_size = (int)pool;
    }
    d->fps = vsapi->mapGetFloat(in, "fps", 0, &err);
    if (err) d->fps = ivi->fpsDen ? (double)ivi->fpsNum / (double)ivi->fpsDen : 0.0;
    if (d->fps < 0) return fail("fps must not be negative");

    // Is SR usable at all here (SDK libraries, driver, GPU)? Then build the first context now so
    // that a problem is reported as an ordinary error of this call, not in the middle of playback.
    if (nvvsr_api_version() != NVVSR_API_VERSION)
      return fail(format("libnvvsr API version %d does not match the plugin (%d)", nvvsr_api_version(), NVVSR_API_VERSION));
    int rc = nvvsr_probe();
    if (rc != NVVSR_OK) return fail("super resolution is unavailable: " + status_text(rc));

    d->slots.resize((size_t)d->pool_size);
    {
      std::string why;
      const int m0 = d->forced_matrix >= 0 ? d->forced_matrix : NVVSR_MATRIX_AUTO;
      const int r0 = d->forced_range >= 0 ? d->forced_range : 0;
      Slot &s0 = d->slots[0];
      if (create_ctx(*d, m0, r0, &s0.ctx, &why) != NVVSR_OK) return fail(why);
      s0.matrix = m0;
      s0.full_range = r0;
    }

    VSVideoFormat of;
    const bool have_fmt = d->out_yuv ? vsapi->queryVideoFormat(&of, cfYUV, stInteger, 8, 1, 1, core)
                                     : vsapi->queryVideoFormat(&of, cfRGB, stInteger, 8, 0, 0, core);
    if (!have_fmt) return fail("cannot query the output format");
    d->vi = *ivi;
    d->vi.format = of;
    d->vi.width = d->out_w;
    d->vi.height = d->out_h;
    d->node = node;
#ifdef NVVSR_VS_TESTHOOK
    if (const char *e = std::getenv("NVVSR_VS_FAIL_AFTER")) d->fail_after = std::atoi(e);
#endif

    const VSFilterDependency deps[] = {{node, rpStrictSpatial}};
    vsapi->createVideoFilter(out, "Upscale", &d->vi, getFrame, freeFilter, fmParallel, deps, 1, d, core);
    fd.release();  // owned by VapourSynth from here on (freeFilter)
  } catch (const std::exception &e) {
    fd.reset();
    vsapi->mapSetError(out, (std::string("Upscale: internal error: ") + e.what()).c_str());
    vsapi->freeNode(node);
  } catch (...) {
    fd.reset();
    vsapi->mapSetError(out, "Upscale: internal error");
    vsapi->freeNode(node);
  }
}

// ---- Auto: the decision logic of mpv/vsr.vpy -------------------------------------------------------------------------

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

// Whole-string numbers (surrounding blanks allowed); strtol saturates, which is what the pool clamp wants.
bool parse_int(const std::string &text, long *v) {
  const std::string s = trim(text);
  char *end = nullptr;
  *v = std::strtol(s.c_str(), &end, 10);
  return !s.empty() && *end == '\0';
}

bool parse_real(const std::string &text, double *v) {
  const std::string s = trim(text);
  char *end = nullptr;
  *v = std::strtod(s.c_str(), &end);
  return !s.empty() && *end == '\0';
}

struct MapFree {
  const VSAPI *api;
  void operator()(VSMap *m) const { api->freeMap(m); }
};
using MapPtr = std::unique_ptr<VSMap, MapFree>;

// Returns a new reference to the upscaled clip, or NULL with *why set (the caller passes `node` through).
VSNode *auto_upscale(const VSMap *in, VSNode *node, VSCore *core, const VSAPI *vsapi, std::string *why) {
  auto fail = [&](std::string msg) -> VSNode * {
    *why = std::move(msg);
    return nullptr;
  };
  const VSVideoInfo *vi = vsapi->getVideoInfo(node);
  const VSVideoFormat &f = vi->format;
  if (!vsh::isConstantVideoFormat(vi)) return fail("variable format/size");
  if (f.colorFamily != cfYUV || f.sampleType != stInteger || f.bitsPerSample != 8 || f.subSamplingW != 1 || f.subSamplingH != 1) {
    char name[32] = "unknown";
    vsapi->getVideoFormatName(&f, name);
    return fail(format("%s is not 8-bit 4:2:0", name));
  }

  auto geti = [&](const char *key) {
    int err = 0;
    const int64_t v = vsapi->mapGetInt(in, key, 0, &err);
    return err ? (int64_t)0 : v;
  };
  int64_t dw = geti("display_w"), dh = geti("display_h");
  if (dw <= 0 || dh <= 0) {  // the player cannot tell (e.g. --vo=null): NVVSR_DISPLAY="WxH"
    dw = dh = 0;
    const char *env = std::getenv("NVVSR_DISPLAY");
    const std::string e = env ? env : "";
    const size_t x = e.find('x');
    long w = 0, h = 0;
    if (x != std::string::npos && parse_int(e.substr(0, x), &w) && parse_int(e.substr(x + 1), &h)) {
      dw = w;
      dh = h;
    }
  }
  if (dw <= 0 || dh <= 0) return fail("display size unknown (set NVVSR_DISPLAY=WxH)");
  // Scale the pixel grid by the factor that fits the displayed picture (anamorphic sources included).
  const int64_t aw = geti("dar_w"), ah = geti("dar_h");
  const double vw = aw > 0 ? (double)aw : vi->width, vh = ah > 0 ? (double)ah : vi->height;
  const double scale = std::min({(double)dw / vw, (double)dh / vh, 4.0});
  const int out_w = std::max(even_round(vi->width * scale), vi->width);
  const int out_h = std::max(even_round(vi->height * scale), vi->height);
  if (out_w == vi->width && out_h == vi->height)
    return fail(format("nothing to gain on a %lldx%lld display from a %dx%d source", (long long)dw, (long long)dh, vi->width, vi->height));

  // options: "key=value;key=value" (',' also separates). Later items replace earlier ones.
  int err = 0;
  const char *od = vsapi->mapGetData(in, "options", 0, &err);
  std::string text = !err && od ? std::string(od, (size_t)std::max(vsapi->mapGetDataSize(in, "options", 0, nullptr), 0)) : "";
  std::replace(text.begin(), text.end(), ',', ';');
  std::map<std::string, std::string> opts;
  for (size_t pos = 0; pos <= text.size();) {
    size_t end = text.find(';', pos);
    if (end == std::string::npos) end = text.size();
    const std::string item = trim(text.substr(pos, end - pos));
    pos = end + 1;
    const size_t eq = item.find('=');
    if (eq != std::string::npos) {
      std::string key = trim(item.substr(0, eq));
      for (char &c : key) c = (char)std::tolower((unsigned char)c);
      opts[key] = trim(item.substr(eq + 1));
    } else if (!item.empty()) {
      std::fprintf(stderr, "nvvsr: ignoring option item '%s' (expected key=value)\n", item.c_str());
    }
  }

  MapPtr args(vsapi->createMap(), MapFree{vsapi});
  VSMap *a = args.get();
  vsapi->mapSetNode(a, "clip", node, maReplace);
  vsapi->mapSetInt(a, "width", out_w, maReplace);
  vsapi->mapSetInt(a, "height", out_h, maReplace);
  vsapi->mapSetData(a, "format", "yuv420p8", -1, dtUtf8, maReplace);  // mpv accepts YUV output only
  const double fps = vsapi->mapGetFloat(in, "fps", 0, &err);
  if (!err && fps > 0) vsapi->mapSetFloat(a, "fps", fps, maReplace);
  long n = 0;
  double x = 0;
  std::string unknown;
  for (const auto &kv : opts) {
    const std::string &k = kv.first, &v = kv.second;
    if (k == "quality" || k == "matrix") {  // validated by Upscale
      vsapi->mapSetData(a, k.c_str(), v.data(), (int)v.size(), dtUtf8, maReplace);
    } else if (k == "strength") {
      if (!parse_real(v, &x)) return fail("strength '" + v + "' is not a number");
      vsapi->mapSetFloat(a, "strength", x, maReplace);
    } else if (k == "pool") {
      if (!parse_int(v, &n)) return fail("pool '" + v + "' is not an integer");
      vsapi->mapSetInt(a, "pool", std::min(std::max(n, 1L), (long)kMaxPool), maReplace);
    } else if (k == "range") {
      if (!parse_int(v, &n)) return fail("range '" + v + "' is not an integer");
      vsapi->mapSetInt(a, "range", n, maReplace);
    } else {
      unknown += (unknown.empty() ? "" : ", ") + k;
    }
  }
  if (!unknown.empty()) std::fprintf(stderr, "nvvsr: ignoring unknown options: %s\n", unknown.c_str());

  MapPtr res(vsapi->createMap(), MapFree{vsapi});
  upscaleCreate(a, res.get(), nullptr, core, vsapi);  // all argument checks and the GPU/SDK probe live there
  if (const char *e = vsapi->mapGetError(res.get())) return fail(e);
  VSNode *up = vsapi->mapGetNode(res.get(), "clip", 0, &err);
  if (err || !up) return fail("Upscale returned no clip");
  return up;
}

// Auto never raises: whatever is unsuitable or fails, the result is the input clip and one line on stderr says why.
void VS_CC autoCreate(const VSMap *in, VSMap *out, void *, VSCore *core, const VSAPI *vsapi) {
  VSNode *node = vsapi->mapGetNode(in, "clip", 0, nullptr);
  std::string why;
  VSNode *up = nullptr;
  try {
    up = auto_upscale(in, node, core, vsapi, &why);
  } catch (const std::exception &e) {
    why = std::string("internal error: ") + e.what();
  } catch (...) {
    why = "internal error";
  }
  if (up) {
    vsapi->freeNode(node);
  } else {
    std::fprintf(stderr, "nvvsr: %s, pass-through\n", why.c_str());
    up = node;
  }
  vsapi->mapConsumeNode(out, "clip", up, maReplace);
}

}  // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
  vspapi->configPlugin("com.nvvsr.upscale", "nvvsr", "NVIDIA RTX video super resolution", VS_MAKE_VERSION(0, 1),
                       VAPOURSYNTH_API_VERSION, 0, plugin);
  vspapi->registerFunction("Upscale",
                           "clip:vnode;width:int:opt;height:int:opt;quality:data:opt;strength:float:opt;"
                           "matrix:data:opt;range:int:opt;pool:int:opt;fps:float:opt;format:data:opt;",
                           "clip:vnode;", upscaleCreate, nullptr, plugin);
  vspapi->registerFunction("Auto",
                           "clip:vnode;display_w:int:opt;display_h:int:opt;dar_w:int:opt;dar_h:int:opt;fps:float:opt;options:data:opt;",
                           "clip:vnode;", autoCreate, nullptr, plugin);
}
