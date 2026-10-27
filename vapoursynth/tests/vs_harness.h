// Header-only helpers to test the nvvsr plugin through the VapourSynth C API: a core from the system
// libvapoursynth (no VSScript, no Python), plugin loading from an explicit path, test clips (std.BlankClip,
// plus a small pattern filter), plugin calls, synchronous frame fetches, frame properties. Everything is
// RAII; errors come back as strings. Destroy every Map/Node/Frame before the Env (declare the Env first).
#pragma once

#include <VSHelper4.h>
#include <VapourSynth4.h>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace vst {

inline std::string format_name(const VSAPI *api, const VSVideoFormat &f) {
  char name[32] = "unknown";
  api->getVideoFormatName(&f, name);
  return name;
}

class Node;

// Owning VSMap (call arguments / results). The setters chain; `append` adds to an array value. They are
// const (a Map is a handle) so that a temporary can be built in the argument list of a call.
class Map {
 public:
  Map(const VSAPI *api, VSMap *m) : api_(api), m_(m) {}
  Map(Map &&o) noexcept : api_(o.api_), m_(o.m_) { o.m_ = nullptr; }
  Map(const Map &) = delete;
  Map &operator=(const Map &) = delete;
  ~Map() {
    if (m_) api_->freeMap(m_);
  }
  VSMap *get() const { return m_; }
  const Map &i(const char *key, int64_t v, bool append = false) const {
    api_->mapSetInt(m_, key, v, append ? maAppend : maReplace);
    return *this;
  }
  const Map &f(const char *key, double v, bool append = false) const {
    api_->mapSetFloat(m_, key, v, append ? maAppend : maReplace);
    return *this;
  }
  const Map &s(const char *key, const std::string &v, bool append = false) const {
    api_->mapSetData(m_, key, v.data(), (int)v.size(), dtUtf8, append ? maAppend : maReplace);
    return *this;
  }
  const Map &node(const char *key, const Node &n, bool append = false) const;  // takes its own reference

 private:
  const VSAPI *api_;
  VSMap *m_;
};

class Frame {
 public:
  Frame() = default;
  Frame(const VSAPI *api, const VSFrame *f) : api_(api), f_(f) {}
  Frame(Frame &&o) noexcept : api_(o.api_), f_(o.f_) { o.f_ = nullptr; }
  Frame &operator=(Frame &&o) noexcept {
    if (this != &o) {
      reset();
      api_ = o.api_;
      f_ = o.f_;
      o.f_ = nullptr;
    }
    return *this;
  }
  Frame(const Frame &) = delete;
  Frame &operator=(const Frame &) = delete;
  ~Frame() { reset(); }
  explicit operator bool() const { return f_ != nullptr; }

  int width(int plane = 0) const { return api_->getFrameWidth(f_, plane); }
  int height(int plane = 0) const { return api_->getFrameHeight(f_, plane); }
  std::string format() const { return format_name(api_, *api_->getVideoFrameFormat(f_)); }
  std::string describe() const { return std::to_string(width()) + "x" + std::to_string(height()) + " " + format(); }
  const uint8_t *plane(int p) const { return api_->getReadPtr(f_, p); }
  ptrdiff_t stride(int p) const { return api_->getStride(f_, p); }
  // Frame properties: false when the key is missing or of another type.
  bool prop_int(const char *key, int64_t *v) const {
    int err = 0;
    *v = api_->mapGetInt(api_->getFramePropertiesRO(f_), key, 0, &err);
    return !err;
  }
  bool prop_float(const char *key, double *v) const {
    int err = 0;
    *v = api_->mapGetFloat(api_->getFramePropertiesRO(f_), key, 0, &err);
    return !err;
  }

 private:
  void reset() {
    if (f_) api_->freeFrame(f_);
    f_ = nullptr;
  }
  const VSAPI *api_ = nullptr;
  const VSFrame *f_ = nullptr;
};

class Node {
 public:
  Node() = default;
  Node(const VSAPI *api, VSNode *n) : api_(api), n_(n) {}
  Node(Node &&o) noexcept : api_(o.api_), n_(o.n_) { o.n_ = nullptr; }
  Node &operator=(Node &&o) noexcept {
    if (this != &o) {
      reset();
      api_ = o.api_;
      n_ = o.n_;
      o.n_ = nullptr;
    }
    return *this;
  }
  Node(const Node &) = delete;
  Node &operator=(const Node &) = delete;
  ~Node() { reset(); }
  explicit operator bool() const { return n_ != nullptr; }
  VSNode *get() const { return n_; }

  const VSVideoInfo *info() const { return api_->getVideoInfo(n_); }
  // "WxH FORMAT" of the clip (RGB24, YUV420P8, Gray8, ...).
  std::string describe() const {
    const VSVideoInfo *vi = info();
    return std::to_string(vi->width) + "x" + std::to_string(vi->height) + " " + format_name(api_, vi->format);
  }
  // Renders frame n and waits for it; a null Frame and *err set on failure.
  Frame frame(int n, std::string *err) const {
    char buf[1024] = "";
    const VSFrame *f = api_->getFrame(n, n_, buf, (int)sizeof buf);
    if (!f && err) *err = buf;
    return Frame(api_, f);
  }

 private:
  void reset() {
    if (n_) api_->freeNode(n_);
    n_ = nullptr;
  }
  const VSAPI *api_ = nullptr;
  VSNode *n_ = nullptr;
};

inline const Map &Map::node(const char *key, const Node &n, bool append) const {
  api_->mapSetNode(m_, key, n.get(), append ? maAppend : maReplace);
  return *this;
}

// Outcome of a plugin call: empty error = success; `clip` holds the "clip" result if there was one.
struct Result {
  std::string error;
  Node clip;
  explicit operator bool() const { return error.empty(); }
  // Frame n of the clip; a null Frame and *err set (to this error, or the render error) on failure.
  Frame frame(int n, std::string *err) const {
    if (!error.empty()) {
      *err = error;
      return Frame();
    }
    return clip.frame(n, err);
  }
};

// A filter that overwrites the planes of a YUV420P8 clip with a deterministic pattern (std.Expr
// has no pixel coordinates, so this is a small filter of our own: frame properties are kept).
namespace detail {
struct PatternData {
  VSNode *src;
  bool neutral;
};
inline const VSFrame *VS_CC pattern_get_frame(int n, int reason, void *inst, void **, VSFrameContext *ctx, VSCore *core,
                                              const VSAPI *api) {
  auto *d = static_cast<PatternData *>(inst);
  if (reason == arInitial) {
    api->requestFrameFilter(n, d->src, ctx);
    return nullptr;
  }
  if (reason != arAllFramesReady) return nullptr;
  const VSFrame *src = api->getFrameFilter(n, d->src, ctx);
  VSFrame *dst = api->copyFrame(src, core);
  api->freeFrame(src);
  for (int p = 0; p < 3; ++p) {
    uint8_t *base = api->getWritePtr(dst, p);
    const ptrdiff_t stride = api->getStride(dst, p);
    for (int y = 0; y < api->getFrameHeight(dst, p); ++y)
      for (int x = 0; x < api->getFrameWidth(dst, p); ++x) {
        int v = 128;
        if (p == 0) v = std::min(235, std::max(16, 16 + (x * 7 + y * 3 + n * 11) % 220 + 12 * ((x / 16 + y / 16) % 2)));
        else if (!d->neutral) v = p == 1 ? 64 + (x * 3 + n) % 128 : 64 + (y * 5 + 2 * n) % 128;
        base[(ptrdiff_t)y * stride + x] = (uint8_t)v;
      }
  }
  return dst;
}
inline void VS_CC pattern_free(void *inst, VSCore *, const VSAPI *api) {
  auto *d = static_cast<PatternData *>(inst);
  api->freeNode(d->src);
  delete d;
}
}  // namespace detail

// Test source clip (std.BlankClip, optionally patterned).
struct Spec {
  int w = 1280, h = 720;
  uint32_t format = pfYUV420P8;  // VSPresetFormat
  int length = 10;
  bool pattern = false;   // deterministic non-flat picture (YUV420P8 only)
  bool neutral = false;   // pattern with neutral chroma
  bool variable = false;  // variable size: 2 frames of 640x360 spliced in front of the clip
};

class Env {
 public:
  const VSAPI *api;
  VSCore *core;

  Env() : api(getVapourSynthAPI(VAPOURSYNTH_API_VERSION)), core(api ? api->createCore(ccfDisableAutoLoading) : nullptr) {
    // ctest passes the configure-time wheel directory here; a NVVSR_LIBDIR set by the caller wins.
    if (const char *d = std::getenv("NVVSR_LIBDIR_DEFAULT")) setenv("NVVSR_LIBDIR", d, 0);
  }
  ~Env() {
    if (core) api->freeCore(core);
  }
  Env(const Env &) = delete;
  Env &operator=(const Env &) = delete;
  explicit operator bool() const { return core != nullptr; }

  Map map() const { return Map(api, api->createMap()); }

  // ns.fn(args) -> its "clip" result. Unknown plugin/function and VapourSynth errors are reported too.
  Result call(const char *ns, const char *fn, const Map &args) const {
    VSPlugin *p = api->getPluginByNamespace(ns, core);
    if (!p) return {std::string("no plugin with namespace '") + ns + "'", Node()};
    if (!api->getPluginFunctionByName(fn, p)) return {std::string(ns) + "." + fn + " does not exist", Node()};
    Map out(api, api->invoke(p, fn, args.get()));
    if (const char *e = api->mapGetError(out.get())) return {e, Node()};
    int err = 0;
    VSNode *n = api->mapGetNode(out.get(), "clip", 0, &err);
    return {"", err ? Node() : Node(api, n)};
  }

  // ns.fn(args + clip=prev.clip), or prev's error passed on, so calls chain without a check per step.
  Result then(const Result &prev, const char *ns, const char *fn, const Map &args) const {
    if (!prev) return {prev.error, Node()};
    return call(ns, fn, args.node("clip", prev.clip));
  }

  bool load_plugin(const std::string &path, std::string *err) const {
    Result r = call("std", "LoadPlugin", map().s("path", path));
    if (!r && err) *err = r.error;
    return (bool)r;
  }
  // The plugin under test: the file named by NVVSR_PLUGIN (ctest sets it).
  bool load_nvvsr(std::string *err) const {
    const char *p = std::getenv("NVVSR_PLUGIN");
    if (!p || !*p) {
      if (err) *err = "NVVSR_PLUGIN is not set";
      return false;
    }
    return load_plugin(p, err);
  }

  Result test_clip(const Spec &s) const {
    Map a = map();
    a.i("width", s.w).i("height", s.h).i("format", s.format).i("length", s.length).i("fpsnum", 24).i("fpsden", 1);
    if (s.format == pfYUV420P8) a.f("color", 90).f("color", 128, true).f("color", 128, true);
    Result r = call("std", "BlankClip", a);
    if (r && s.pattern) {
      if (s.format != pfYUV420P8) return {"Spec.pattern needs YUV420P8", Node()};
      VSNode *src = api->addNodeRef(r.clip.get());
      const VSFilterDependency dep = {src, rpStrictSpatial};
      VSNode *pn = api->createVideoFilter2("Pattern", api->getVideoInfo(src), detail::pattern_get_frame, detail::pattern_free,
                                           fmParallel, &dep, 1, new detail::PatternData{src, s.neutral}, core);
      r = {"", Node(api, pn)};
    }
    if (r && s.variable) {
      Result small = call("std", "BlankClip", map().node("clip", r.clip).i("width", 640).i("height", 360).i("length", 2));
      if (!small) return small;
      r = call("std", "Splice", map().node("clips", small.clip).node("clips", r.clip, true).i("mismatch", 1));
    }
    return r;
  }
};

// ---- BEGIN async frame requests (test_concurrency) -------------------------------------------------------------------
namespace detail {
struct AsyncState {
  const VSAPI *api;
  VSNode *node;
  int total;
  std::function<void(int, Frame &&)> sink;
  std::mutex mu;
  std::condition_variable cv;
  int next = 0, inflight = 0;
  bool stop = false;
  std::string err;

  int reserve() {  // the next frame number to request, or -1; counted as in flight
    std::lock_guard<std::mutex> lk(mu);
    if (stop || next >= total) return -1;
    ++inflight;
    return next++;
  }
};
inline void VS_CC async_done(void *ud, const VSFrame *f, int n, VSNode *, const char *msg) {
  auto *s = static_cast<AsyncState *>(ud);
  std::string e;
  if (f) s->sink(n, Frame(s->api, f));
  else e = std::string("frame ") + std::to_string(n) + ": " + (msg && *msg ? msg : "no frame and no message");
  {
    std::lock_guard<std::mutex> lk(s->mu);
    if (!e.empty()) {
      if (s->err.empty()) s->err = e;
      s->stop = true;
    }
  }
  const int m = s->reserve();  // before the decrement, so `inflight` cannot reach 0 while frames are left
  if (m >= 0) s->api->getFrameAsync(m, s->node, async_done, s);
  std::lock_guard<std::mutex> lk(s->mu);
  --s->inflight;
  s->cv.notify_all();  // under the lock: the waiter may free the state as soon as it can take it
}
}  // namespace detail

// Renders frames 0..count-1 of `clip` with `depth` getFrameAsync requests in flight (like `vspipe --requests`),
// and waits for all of them. `sink(n, frame)` runs on VapourSynth threads, concurrently and in any order: write to
// a slot of your own per n. Returns "" or the first error (after it no new request is made and the ones in flight
// are drained). A deadlock in the filter blocks this call: guard it with a watchdog.
inline std::string render_async(const Env &env, const Node &clip, int count, int depth,
                                std::function<void(int, Frame &&)> sink) {
  if (!clip) return "no clip";
  const int len = env.api->getVideoInfo(clip.get())->numFrames;
  if (count < 1 || count > len || depth < 1)
    return "render_async: " + std::to_string(count) + " frames at depth " + std::to_string(depth) + " of a " + std::to_string(len) + "-frame clip";
  detail::AsyncState s;
  s.api = env.api;
  s.node = clip.get();
  s.total = count;
  s.sink = std::move(sink);
  for (int i = 0; i < depth; ++i) {
    const int n = s.reserve();
    if (n < 0) break;
    env.api->getFrameAsync(n, s.node, detail::async_done, &s);
  }
  std::unique_lock<std::mutex> lk(s.mu);
  s.cv.wait(lk, [&] { return s.inflight == 0; });
  return s.err;
}
// ---- END async frame requests ------------------------------------------------------------------------------------------

}  // namespace vst
