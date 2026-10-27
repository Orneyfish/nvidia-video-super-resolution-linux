// Output geometry and format of core.nvvsr.Upscale, and clean errors for unsupported input and bad
// arguments. Cases of the removed run.sh sections 2 and 2b (same expected values), plus a few
// checks that render frames (frame size, format and properties agree with the clip info).
// Needs NVVSR_PLUGIN (ctest sets it) and the NVIDIA libraries (NVVSR_LIBDIR).
#include <algorithm>
#include <set>

#include "vs_check.h"
#include "vs_harness.h"

using vst::Env;
using vst::Map;
using vst::Spec;

// vinfo: "WxH FORMAT" of Upscale's output clip, or "ERROR: <message>" when the call fails.
static std::string vinfo(const Env &env, const Spec &src, const Map &up_args) {
  vst::Result r = env.then(env.test_clip(src), "nvvsr", "Upscale", up_args);
  if (!r) return "ERROR: " + r.error;
  return r.clip ? r.clip.describe() : "ERROR: no clip returned";
}

// The call must fail cleanly (an error message, no crash) with `needle` in the message.
static void expect_error(const Env &env, const char *name, const char *needle, const Spec &src, const Map &up_args) {
  const std::string got = vinfo(env, src, up_args);
  vt::check(got.rfind("ERROR: ", 0) == 0 && got.find(needle) != std::string::npos, name,
            "got '" + got + "', expected an error containing '" + needle + "'");
}

// Min and max sample of plane p of an 8-bit frame.
static std::pair<int, int> plane_range(const vst::Frame &f, int p) {
  int lo = 255, hi = 0;
  for (int y = 0; y < f.height(p); ++y) {
    const uint8_t *row = f.plane(p) + (ptrdiff_t)y * f.stride(p);
    for (int x = 0; x < f.width(p); ++x) {
      lo = std::min<int>(lo, row[x]);
      hi = std::max<int>(hi, row[x]);
    }
  }
  return {lo, hi};
}

int main() {
  Env env;  // first: it must outlive every Map, Node and Frame below
  if (!env) {
    std::printf("FAIL cannot create a VapourSynth core\n");
    return 1;
  }
  std::string err;
  if (!vt::check(env.load_nvvsr(&err), "plugin loads", err)) return vt::finish("test_geometry");

  std::printf("== 2. output geometry and format\n");
  vt::check_eq("720p -> 2560x1440 RGB24", vinfo(env, {}, env.map().i("width", 2560).i("height", 1440)), "2560x1440 RGB24");
  vt::check_eq("width only (1280x720, width=1920) -> 1920x1080", vinfo(env, {}, env.map().i("width", 1920)), "1920x1080 RGB24");
  vt::check_eq("height only (1280x720, height=1080) -> 1920x1080", vinfo(env, {}, env.map().i("height", 1080)), "1920x1080 RGB24");
  vt::check_eq("derived size is rounded to even (854x480, width=1000 -> 1000x562)",
               vinfo(env, Spec{854, 480}, env.map().i("width", 1000)), "1000x562 RGB24");
  vt::check_eq("no size -> same size as the input (still processed)", vinfo(env, {}, env.map()), "1280x720 RGB24");
  vt::check_eq("format=yuv420p8 -> YUV420P8", vinfo(env, {}, env.map().i("height", 1440).s("format", "yuv420p8")), "2560x1440 YUV420P8");
  vt::check_eq("exactly 4x is accepted (640x360 -> 2560x1440)",
               vinfo(env, Spec{640, 360}, env.map().i("width", 2560).i("height", 1440)), "2560x1440 RGB24");
  std::set<std::string> pools;
  for (int p = 1; p <= 4; ++p) pools.insert(vinfo(env, {}, env.map().i("pool", p)));
  vt::check_eq("every pool size 1..4 is accepted", pools.size() == 1 ? *pools.begin() : "varied results", "1280x720 RGB24");

  std::printf("== 2b. unsupported input and bad arguments: clear errors, no crash\n");
  expect_error(env, "10-bit input is rejected", "unsupported input format YUV420P10", Spec{1280, 720, pfYUV420P10}, env.map());
  expect_error(env, "GRAY8 input is rejected", "unsupported input format Gray8", Spec{1280, 720, pfGray8}, env.map());
  expect_error(env, "YUV444P8 input is rejected", "unsupported input format YUV444P8", Spec{1280, 720, pfYUV444P8}, env.map());
  expect_error(env, "RGB24 input is rejected", "unsupported input format RGB24", Spec{1280, 720, pfRGB24}, env.map());
  Spec variable;
  variable.variable = true;
  expect_error(env, "variable-size input is rejected", "constant format and size", variable, env.map());
  expect_error(env, "downscaling is rejected", "downscaling is not supported", {}, env.map().i("width", 640).i("height", 360));
  expect_error(env, "more than 4x is rejected", "at most 4x", Spec{640, 360}, env.map().i("width", 2800).i("height", 1500));
  expect_error(env, "unknown quality is rejected", "quality must be", {}, env.map().s("quality", "superb"));
  expect_error(env, "pool=0 is rejected", "pool must be between 1 and 4", {}, env.map().i("pool", 0));
  expect_error(env, "pool=5 is rejected", "pool must be between 1 and 4", {}, env.map().i("pool", 5));
  expect_error(env, "strength=2 is rejected", "strength must be between 0 and 1", {}, env.map().f("strength", 2));
  expect_error(env, "unknown format is rejected", "format must be rgb24 or yuv420p8", {}, env.map().s("format", "nv12"));
  expect_error(env, "unknown matrix is rejected", "matrix must be", {}, env.map().s("matrix", "2020"));
  expect_error(env, "range=2 is rejected", "range must be 0 (full) or 1 (limited)", {}, env.map().i("range", 2));
  for (const char *q : {"auto", "bicubic", "low", "medium", "high", "ultra", "streaming_medium", "streaming_ultra"})
    vt::check_eq(std::string("quality=") + q + " is accepted", vinfo(env, {}, env.map().s("quality", q).f("strength", 0.5)), "1280x720 RGB24");

  std::printf("== 3. rendered frames agree with the clip info\n");
  {  // RGB output
    vst::Result up = env.then(env.test_clip({}), "nvvsr", "Upscale", env.map().i("width", 2560).i("height", 1440));
    vst::Frame f = up.frame(0, &err);
    if (vt::check((bool)f, "RGB24 output: frame 0 renders", err)) {
      int64_t matrix = -1, range = -1;
      vt::check_eq("RGB24 output: frame 0 is 2560x1440 RGB24", f.describe(), "2560x1440 RGB24");
      vt::check(f.prop_int("_Matrix", &matrix) && matrix == 0, "RGB24 output: _Matrix is 0 (RGB)", "_Matrix=" + std::to_string(matrix));
      vt::check(f.prop_int("_Range", &range) && range == 1, "RGB24 output: _Range is full (R81: 1 = full)", "_Range=" + std::to_string(range));
    }
  }
  {  // YUV output; properties of the source (here the frame duration) are carried over
    vst::Result src = env.then(env.test_clip({}), "std", "SetFrameProps", env.map().i("_DurationNum", 1001).i("_DurationDen", 24000));
    vst::Result up = env.then(src, "nvvsr", "Upscale", env.map().i("height", 1440).s("format", "yuv420p8"));
    vst::Frame f = up.frame(0, &err);
    if (vt::check((bool)f, "YUV420P8 output: frame 0 renders", err)) {
      int64_t num = -1, loc = -1;
      vt::check_eq("YUV420P8 output: frame 0 is 2560x1440 YUV420P8", f.describe(), "2560x1440 YUV420P8");
      vt::check(f.prop_int("_DurationNum", &num) && num == 1001, "YUV420P8 output: _DurationNum is carried over", "_DurationNum=" + std::to_string(num));
      vt::check(f.prop_int("_ChromaLocation", &loc) && loc == 0, "YUV420P8 output: _ChromaLocation is 0 (left)", "_ChromaLocation=" + std::to_string(loc));
    }
  }
  {  // pattern clips (what the later correctness tests feed in); 1282x722 has chroma 641x361 and padded strides
    Spec ps{1282, 722};
    ps.pattern = true;
    vst::Result c = env.test_clip(ps);
    vst::Frame f = c.frame(1, &err);
    if (vt::check((bool)f, "pattern clip: frame 1 renders", err)) {
      auto y = plane_range(f, 0), u = plane_range(f, 1);
      vt::check(y.first >= 16 && y.second <= 235 && y.second - y.first > 100, "pattern clip: Y varies within 16..235",
                std::to_string(y.first) + ".." + std::to_string(y.second));
      vt::check(u.first >= 64 && u.second < 192 && u.second - u.first > 50, "pattern clip: U varies within 64..191",
                std::to_string(u.first) + ".." + std::to_string(u.second));
    }
    ps.neutral = true;
    vst::Frame nf = env.test_clip(ps).frame(0, &err);
    const auto flat = std::make_pair(128, 128);
    vt::check(nf && plane_range(nf, 1) == flat && plane_range(nf, 2) == flat, "neutral pattern clip: chroma is 128", err);
    vst::Result up = env.then(c, "nvvsr", "Upscale", env.map().i("width", 2564).i("height", 1444));
    vst::Frame uf = up.frame(0, &err);
    vt::check_eq("pattern 1282x722 -> 2564x1444: frame 0 renders at that size", uf ? uf.describe() : "ERROR: " + err, "2564x1444 RGB24");
  }

  return vt::finish("test_geometry");
}
