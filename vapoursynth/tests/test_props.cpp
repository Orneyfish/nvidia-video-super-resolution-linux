// Section 3b of run.sh as C++ (the checks of props_check.py): the frame properties the plugin sets on its
// output, the colour selection from arguments and from _Matrix/_ColorSpace/_Range, the per-frame colour
// cache, and a concurrency stress run. Needs NVVSR_PLUGIN (ctest sets it) and the NVIDIA libraries.
#include <algorithm>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

#include "vs_check.h"
#include "vs_harness.h"

using vst::Env;
using vst::Map;

namespace {

const int64_t kMissing = INT64_MIN;

// An int frame property, or kMissing (also for a null frame or a property of another type).
int64_t iprop(const vst::Frame &f, const char *key) {
  int64_t v = 0;
  return (f && f.prop_int(key, &v)) ? v : kMissing;
}
// A float frame property, or NaN.
double fprop(const vst::Frame &f, const char *key) {
  double v = 0;
  return (f && f.prop_float(key, &v)) ? v : NAN;
}
std::string desc(const vst::Frame &f) { return f ? f.describe() : "no frame"; }

// Planes of a frame (the first `planes` of them) back to back, rows without padding. Empty for a null frame.
std::string bytes_of(const vst::Frame &f, int planes) {
  std::string s;
  if (!f) return s;
  for (int p = 0; p < planes; ++p)
    for (int y = 0; y < f.height(p); ++y)
      s.append(reinterpret_cast<const char *>(f.plane(p) + (ptrdiff_t)y * f.stride(p)), (size_t)f.width(p));
  return s;
}
// All three planes of frame n; empty if the frame does not render (every comparison has a non-empty baseline).
std::string payload(const vst::Result &r, int n) {
  std::string err;
  return bytes_of(r.frame(n, &err), 3);
}

// 640x360 YUV420P8 with strongly coloured chroma (so the matrix matters), 2 frames at 24 fps, with the
// properties props_check.py sets plus the given int properties.
vst::Result source(const Env &env, std::initializer_list<std::pair<const char *, int64_t>> props = {}) {
  Map bc = env.map();
  bc.i("width", 640).i("height", 360).i("format", pfYUV420P8).i("length", 2).i("fpsnum", 24).i("fpsden", 1)
      .f("color", 100).f("color", 90, true).f("color", 170, true);
  vst::Result clip = env.call("std", "BlankClip", bc);
  Map sp = env.map();
  sp.i("_DurationNum", 1001).i("_DurationDen", 24000).f("_AbsoluteTime", 0.5).i("_FieldBased", 0);
  for (const auto &kv : props) sp.i(kv.first, kv.second);
  return env.then(clip, "std", "SetFrameProps", sp);
}

vst::Result interleave(const Env &env, const vst::Result &a, const vst::Result &b) {
  if (!a) return {a.error, vst::Node()};
  if (!b) return {b.error, vst::Node()};
  return env.call("std", "Interleave", env.map().node("clips", a.clip).node("clips", b.clip, true));
}

// nvvsr.Upscale to 1280x720 with the given options (unset ones are not passed).
struct Opt {
  const char *format = nullptr;
  const char *matrix = nullptr;
  int range = -1;
  const char *quality = "bicubic";
  int pool = -1;
};
vst::Result up(const Env &env, const vst::Result &src, const Opt &o = Opt()) {
  Map a = env.map();
  a.i("width", 1280).i("height", 720).s("quality", o.quality);
  if (o.format) a.s("format", o.format);
  if (o.matrix) a.s("matrix", o.matrix);
  if (o.range >= 0) a.i("range", o.range);
  if (o.pool >= 0) a.i("pool", o.pool);
  return env.then(src, "nvvsr", "Upscale", a);
}

// Every frame of `node` (0..count-1), rendered with at most `inflight` requests outstanding (VapourSynth's
// async API, as the stress run needs), each as all three planes.
struct Stream {
  const Env *env = nullptr;
  std::mutex m;
  std::condition_variable cv;
  std::vector<std::string> out;
  std::string error;
  int done = 0;
};

void VS_CC on_frame(void *ud, const VSFrame *f, int n, VSNode *, const char *err) {
  auto *s = static_cast<Stream *>(ud);
  std::string bytes;
  if (f) bytes = bytes_of(vst::Frame(s->env->api, f), 3);
  std::lock_guard<std::mutex> lock(s->m);
  if (f) s->out[n] = bytes;
  else if (s->error.empty()) s->error = err ? err : "frame failed";
  ++s->done;
  s->cv.notify_all();
}

// Renders frames 0..count-1 of `node`; the frames are in `s.out` once it returns.
void render_all(const Env &env, VSNode *node, int count, int inflight, Stream *s) {
  s->env = &env;
  s->out.assign((size_t)count, std::string());
  for (int n = 0; n < count; ++n) {
    {
      std::unique_lock<std::mutex> lock(s->m);
      s->cv.wait(lock, [&] { return n - s->done < inflight; });
    }
    env.api->getFrameAsync(n, node, on_frame, s);
  }
  std::unique_lock<std::mutex> lock(s->m);
  s->cv.wait(lock, [&] { return s->done == count; });
}

}  // namespace

int main() {
  Env env;  // first: it must outlive every Map, Node and Frame below
  if (!env) {
    std::printf("FAIL cannot create a VapourSynth core\n");
    return 1;
  }
  std::string err;
  if (!vt::check(env.load_nvvsr(&err), "plugin loads", err)) return vt::finish("test_props");

  std::printf("== 3b. frame properties and colour selection\n");
  // --- RGB output properties ---
  {
    vst::Result r = up(env, source(env, {{"_Matrix", 1}, {"_Range", 0}, {"_ChromaLocation", 1}, {"_Transfer", 1}}));
    vst::Frame f = r.frame(1, &err);
    vt::check(desc(f) == "1280x720 RGB24", "rgb: format RGB24 1280x720", desc(f));
    vt::check(iprop(f, "_Matrix") == 0 && iprop(f, "_Range") == 1, "rgb: _Matrix=0 _Range=full",
              "_Matrix=" + std::to_string(iprop(f, "_Matrix")) + " _Range=" + std::to_string(iprop(f, "_Range")));
    vt::check(iprop(f, "_ChromaLocation") == kMissing, "rgb: _ChromaLocation dropped",
              "_ChromaLocation=" + std::to_string(iprop(f, "_ChromaLocation")));
    vt::check(iprop(f, "_DurationNum") == 1001 && iprop(f, "_DurationDen") == 24000, "rgb: durations copied",
              "num=" + std::to_string(iprop(f, "_DurationNum")) + " den=" + std::to_string(iprop(f, "_DurationDen")));
    vt::check(fprop(f, "_AbsoluteTime") == 0.5 && iprop(f, "_FieldBased") == 0 && iprop(f, "_Transfer") == 1,
              "rgb: other props kept", "_AbsoluteTime=" + std::to_string(fprop(f, "_AbsoluteTime")) +
                                           " _Transfer=" + std::to_string(iprop(f, "_Transfer")));
    vst::Result plain = up(env, source(env));
    const vst::Node &node = plain.clip;
    vt::check(node && node.info()->fpsNum == 24 && node.info()->fpsDen == 1 && node.info()->numFrames == 2,
              "rgb: clip fps/length copied",
              node ? std::to_string(node.info()->fpsNum) + "/" + std::to_string(node.info()->fpsDen) + " " +
                         std::to_string(node.info()->numFrames) + " frames"
                   : plain.error);
  }

  // --- YUV output properties ---
  {
    Opt yuv;
    yuv.format = "yuv420p8";
    vst::Frame f = up(env, source(env, {{"_Matrix", 5}, {"_Range", 1}, {"_ChromaLocation", 1}}), yuv).frame(1, &err);
    vt::check(desc(f) == "1280x720 YUV420P8", "yuv: format YUV420P8 1280x720", desc(f));
    vt::check(iprop(f, "_Matrix") == 5 && iprop(f, "_Range") == 1, "yuv: matrix and range follow the source",
              "_Matrix=" + std::to_string(iprop(f, "_Matrix")) + " _Range=" + std::to_string(iprop(f, "_Range")));
    vt::check(iprop(f, "_ChromaLocation") == 0, "yuv: _ChromaLocation=0 (left)",
              "_ChromaLocation=" + std::to_string(iprop(f, "_ChromaLocation")));
    vt::check(iprop(f, "_DurationNum") == 1001 && iprop(f, "_DurationDen") == 24000, "yuv: durations copied",
              "num=" + std::to_string(iprop(f, "_DurationNum")) + " den=" + std::to_string(iprop(f, "_DurationDen")));
    vst::Frame g = up(env, source(env), yuv).frame(0, &err);
    vt::check(iprop(g, "_Matrix") == 6 && iprop(g, "_Range") == 0, "yuv: no props -> _Matrix 6 (SD size rule), limited",
              "_Matrix=" + std::to_string(iprop(g, "_Matrix")) + " _Range=" + std::to_string(iprop(g, "_Range")));
  }

  // --- colour selection: arguments, _Matrix, _ColorSpace (the name mpv uses), _Range ---
  Opt m709, m601;
  m709.matrix = "709";
  m601.matrix = "601";
  const std::string p709 = payload(up(env, source(env), m709), 1);
  const std::string p601 = payload(up(env, source(env), m601), 1);
  vt::check(!p709.empty() && !p601.empty(), "matrix: the 709 and 601 outputs render", "empty payload");
  vt::check(p709 != p601, "matrix: 601 and 709 differ on coloured input", "identical output");
  vt::check(payload(up(env, source(env, {{"_Matrix", 1}})), 1) == p709, "matrix: _Matrix=1 equals matrix=709", "");
  vt::check(payload(up(env, source(env, {{"_Matrix", 5}})), 1) == p601, "matrix: _Matrix=5 equals matrix=601", "");
  vt::check(payload(up(env, source(env, {{"_Matrix", 6}})), 1) == p601, "matrix: _Matrix=6 equals matrix=601", "");
  vt::check(payload(up(env, source(env, {{"_ColorSpace", 1}})), 1) == p709, "matrix: mpv's _ColorSpace=1 equals matrix=709", "");
  vt::check(payload(up(env, source(env, {{"_ColorSpace", 6}})), 1) == p601, "matrix: mpv's _ColorSpace=6 equals matrix=601", "");
  vt::check(payload(up(env, source(env)), 1) == p601, "matrix: no property, 640x360 -> BT.601 by size", "");
  {
    Opt o601;
    o601.matrix = "601";
    vt::check(payload(up(env, source(env, {{"_Matrix", 1}}), o601), 1) == p601, "matrix: argument beats the property", "");
  }

  Opt full709, lim709;
  full709.matrix = lim709.matrix = "709";
  full709.range = 0;
  lim709.range = 1;
  const std::string full = payload(up(env, source(env), full709), 1);
  const std::string lim = payload(up(env, source(env), lim709), 1);
  vt::check(!full.empty() && !lim.empty(), "range: the full and limited outputs render", "empty payload");
  vt::check(full != lim, "range: full and limited differ", "identical output");
  vt::check(payload(up(env, source(env, {{"_Range", 1}}), m709), 1) == full, "range: _Range=1 (full) equals range=0", "");
  vt::check(payload(up(env, source(env, {{"_Range", 0}}), m709), 1) == lim, "range: _Range=0 (limited) equals range=1", "");
  vt::check(payload(up(env, source(env), m709), 1) == lim, "range: default is limited", "");
  {
    Opt o = full709;
    vt::check(payload(up(env, source(env, {{"_Range", 0}}), o), 1) == full, "range: argument beats the property", "");
  }

  // --- the per-frame colour cache: frames with different properties in one clip ---
  {
    vst::Result mixed = up(env, interleave(env, source(env, {{"_Matrix", 1}}), source(env, {{"_Matrix", 5}})));
    const std::string w709 = bytes_of(up(env, source(env, {{"_Matrix", 1}})).frame(0, &err), 1);
    const std::string w601 = bytes_of(up(env, source(env, {{"_Matrix", 5}})).frame(0, &err), 1);
    bool ok = !w709.empty() && !w601.empty();
    for (int n = 0; n < 4 && ok; ++n) {  // 709, 601, 709, 601
      std::string got = bytes_of(mixed.frame(n, &err), 1);
      ok = got == (n % 2 == 0 ? w709 : w601);
    }
    vt::check(ok, "matrix: alternating per-frame properties re-create the context correctly", err);
  }

  // --- stress: alternating matrices (forces contexts to be re-created) under heavy concurrency ---
  {
    vst::Result base = env.then(interleave(env, source(env, {{"_Matrix", 1}}), source(env, {{"_Matrix", 5}})), "std", "Loop",
                                env.map().i("times", 60));
    Opt medium;
    medium.quality = "medium";
    const std::string want[2] = {payload(up(env, source(env, {{"_Matrix", 1}}), medium), 0),
                                 payload(up(env, source(env, {{"_Matrix", 5}}), medium), 0)};
    for (int pool : {1, 2, 4}) {
      Opt o = medium;
      o.pool = pool;
      vst::Result stressed = up(env, base, o);
      Stream s;
      if (stressed) render_all(env, stressed.clip.get(), 240, 8, &s);
      int bad = 0;
      for (int n = 0; n < (int)s.out.size(); ++n) bad += s.out[n] != want[n % 2] || want[n % 2].empty();
      const bool ok = stressed && s.error.empty() && bad == 0 && s.out.size() == 240;
      vt::check(ok, "stress: 240 alternating-matrix frames, pool=" + std::to_string(pool) + ", 8 requests in flight",
                (stressed ? "" : stressed.error) + s.error + " " + std::to_string(bad) + " of " +
                    std::to_string(s.out.size()) + " frames differ");
    }
  }

  return vt::finish("test_props");
}
