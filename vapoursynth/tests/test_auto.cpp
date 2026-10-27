// core.nvvsr.Auto: the decision logic that used to live in mpv/vsr.vpy, as a plugin function. Every case of the
// Python upscale() is covered: it returns either the upscaled clip (YUV420P8, fitted to the display) or the input
// clip itself with exactly one "nvvsr: ..., pass-through" line on stderr. Needs NVVSR_PLUGIN (ctest sets it) and the
// NVIDIA libraries (NVVSR_LIBDIR). The bogus-libdir case runs in a child process (libnvvsr initialises once per
// process, so a failed init cannot be undone and must not poison the other cases).
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "vs_check.h"
#include "vs_harness.h"

using vst::Env;
using vst::Map;
using vst::Spec;

// Everything fn writes to stderr (fd 2, so the plugin's fprintf is caught too).
static std::string capture_stderr(const std::function<void()> &fn) {
  FILE *tf = std::tmpfile();
  if (!tf) {
    fn();
    return "";
  }
  std::fflush(stderr);
  const int saved = dup(2);
  dup2(fileno(tf), 2);
  fn();
  std::fflush(stderr);
  dup2(saved, 2);
  close(saved);
  std::rewind(tf);
  std::string text;
  char buf[512];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, tf)) > 0) text.append(buf, n);
  std::fclose(tf);
  return text;
}

struct Run {
  vst::Result src, out;
  std::string log;                 // all of stderr during the call
  std::vector<std::string> lines;  // the lines of it that start with "nvvsr: "
  bool same() const { return out.clip && out.clip.get() == src.clip.get(); }  // pass-through: the very same node
  std::string desc() const { return out && out.clip ? out.clip.describe() : "ERROR: " + out.error; }
  std::string why() const {
    std::string s = "'" + desc() + "' same=" + (same() ? "yes" : "no") + " stderr=[";
    for (const std::string &l : lines) s += l + "|";
    return s + "]";
  }
};

static Run run(const Env &env, const Spec &spec, const Map &args) {
  Run r;
  r.src = env.test_clip(spec);
  if (!r.src) {
    r.out.error = "test clip: " + r.src.error;
    return r;
  }
  r.log = capture_stderr([&] { r.out = env.call("nvvsr", "Auto", args.node("clip", r.src.clip)); });
  for (size_t pos = 0; pos < r.log.size();) {
    size_t end = r.log.find('\n', pos);
    if (end == std::string::npos) end = r.log.size();
    const std::string line = r.log.substr(pos, end - pos);
    if (line.rfind("nvvsr: ", 0) == 0) r.lines.push_back(line);
    pos = end + 1;
  }
  return r;
}

// A map with the display size (the player's report).
static Map disp(const Env &env, int w = 2560, int h = 1440) {
  Map m = env.map();
  m.i("display_w", w).i("display_h", h);
  return m;
}

// Auto upscaled: `want` is the output clip ("WxH YUV420P8"), a new node, and nothing was logged.
static void expect_up(const Env &env, const std::string &name, const Spec &src, const Map &args, const char *want) {
  Run r = run(env, src, args);
  vt::check(r.desc() == want && !r.same() && r.lines.empty(), name, "expected '" + std::string(want) + "' and no log, got " + r.why());
}

// Auto passed the clip through: the same node, exactly one "nvvsr: ... pass-through" line that mentions `needle`.
static void expect_pass(const Env &env, const std::string &name, const Spec &src, const Map &args, const char *needle) {
  Run r = run(env, src, args);
  const bool ok = r.out && r.same() && r.lines.size() == 1 && r.lines[0].find("pass-through") != std::string::npos &&
                  r.lines[0].find(needle) != std::string::npos;
  vt::check(ok, name, "expected the input clip and one line mentioning '" + std::string(needle) + "', got " + r.why());
}

static double mean_plane(const vst::Frame &f, int p) {
  double sum = 0;
  for (int y = 0; y < f.height(p); ++y) {
    const uint8_t *row = f.plane(p) + (ptrdiff_t)y * f.stride(p);
    for (int x = 0; x < f.width(p); ++x) sum += row[x];
  }
  return sum / ((double)f.width(p) * f.height(p));
}

static void min_max_plane(const vst::Frame &f, int p, int *lo, int *hi) {
  *lo = 255;
  *hi = 0;
  for (int y = 0; y < f.height(p); ++y) {
    const uint8_t *row = f.plane(p) + (ptrdiff_t)y * f.stride(p);
    for (int x = 0; x < f.width(p); ++x) {
      *lo = std::min<int>(*lo, row[x]);
      *hi = std::max<int>(*hi, row[x]);
    }
  }
}

// Child process: the SDK libraries cannot be found, so Auto must pass the clip through, and must not crash.
static int bogus_libdir_child(const Env &env) {
  std::string err;
  if (!vt::check(env.load_nvvsr(&err), "bogus libdir: plugin loads", err)) return vt::finish("test_auto (bogus libdir)");
  Run r = run(env, {}, disp(env));
  vt::check(r.out && r.same() && r.lines.size() == 1 && r.lines[0].find("pass-through") != std::string::npos,
            "bogus libdir: pass-through with one line", r.why());
  vst::Frame f = r.out.frame(0, &err);
  vt::check(f && f.describe() == "1280x720 YUV420P8", "bogus libdir: the passed-through clip still renders", err);
  return vt::finish("test_auto (bogus libdir)");
}

int main(int argc, char **argv) {
  Env env;  // first: it must outlive every Map, Node and Frame below
  if (!env) {
    std::printf("FAIL cannot create a VapourSynth core\n");
    return 1;
  }
  if (argc > 1 && std::strcmp(argv[1], "--bogus-libdir") == 0) return bogus_libdir_child(env);
  unsetenv("NVVSR_DISPLAY");  // the caller's session may have one
  std::string err;
  if (!vt::check(env.load_nvvsr(&err), "plugin loads", err)) return vt::finish("test_auto");

  std::printf("== 1. size: fitted to the display, aspect kept, even, 4x at most, never smaller than the source\n");
  expect_up(env, "720p on 2560x1440 -> 2560x1440", {}, disp(env), "2560x1440 YUV420P8");
  expect_up(env, "854x480 on 2560x1440 -> 2560x1438 (aspect wins over the display)", Spec{854, 480}, disp(env), "2560x1438 YUV420P8");
  expect_up(env, "720p on 1920x1200 (16:10) -> 1920x1080", {}, disp(env, 1920, 1200), "1920x1080 YUV420P8");
  expect_up(env, "720p on 1366x768 -> 1366x768 (odd rounding goes to even)", {}, disp(env, 1366, 768), "1366x768 YUV420P8");
  expect_up(env, "640x360 on 7680x4320 -> 4x cap 2560x1440", Spec{640, 360}, disp(env, 7680, 4320), "2560x1440 YUV420P8");
  expect_pass(env, "source equal to the display -> unchanged", Spec{2560, 1440}, disp(env), "nothing to gain");
  expect_pass(env, "source larger than the display -> unchanged", Spec{3840, 2160}, disp(env), "nothing to gain");
  expect_pass(env, "portrait display (720p on 1080x1920 would shrink) -> unchanged", {}, disp(env, 1080, 1920), "nothing to gain");
  expect_pass(env, "1920x1088 on 1080p: never smaller than the source -> unchanged", Spec{1920, 1088}, disp(env, 1920, 1080), "nothing to gain");
  expect_up(env, "anamorphic: 720x480 with dar 854x480 -> 2158x1438", Spec{720, 480}, disp(env).i("dar_w", 854).i("dar_h", 480),
            "2158x1438 YUV420P8");
  expect_up(env, "anamorphic: only dar_w given, dar_h defaults to the clip height", Spec{720, 480}, disp(env).i("dar_w", 854),
            "2158x1438 YUV420P8");
  expect_up(env, "no dar: 720x480 is fitted as a plain pixel grid -> 2160x1440", Spec{720, 480}, disp(env), "2160x1440 YUV420P8");
  expect_up(env, "dar 0 means unknown, like no dar", Spec{720, 480}, disp(env).i("dar_w", 0).i("dar_h", 0), "2160x1440 YUV420P8");

  std::printf("== 2. unsuitable input is passed through (the same clip) with one line\n");
  expect_pass(env, "10-bit 4:2:0", Spec{1280, 720, pfYUV420P10}, disp(env), "YUV420P10 is not 8-bit 4:2:0");
  expect_pass(env, "4:2:2", Spec{1280, 720, pfYUV422P8}, disp(env), "YUV422P8 is not 8-bit 4:2:0");
  expect_pass(env, "4:4:4", Spec{1280, 720, pfYUV444P8}, disp(env), "YUV444P8 is not 8-bit 4:2:0");
  expect_pass(env, "RGB24", Spec{1280, 720, pfRGB24}, disp(env), "RGB24 is not 8-bit 4:2:0");
  expect_pass(env, "Gray8", Spec{1280, 720, pfGray8}, disp(env), "Gray8 is not 8-bit 4:2:0");
  Spec variable;
  variable.variable = true;
  expect_pass(env, "variable size", variable, disp(env), "variable format/size");

  std::printf("== 3. display size: arguments, else NVVSR_DISPLAY, else pass-through\n");
  expect_pass(env, "no display at all -> unchanged", {}, env.map(), "display size unknown");
  expect_pass(env, "display_w without display_h -> unchanged", {}, env.map().i("display_w", 2560), "display size unknown");
  expect_pass(env, "display 0x0 -> unchanged", {}, disp(env, 0, 0), "display size unknown");
  setenv("NVVSR_DISPLAY", "2560x1440", 1);
  expect_up(env, "NVVSR_DISPLAY=2560x1440 is used when the player cannot tell", {}, env.map(), "2560x1440 YUV420P8");
  expect_up(env, "NVVSR_DISPLAY also stands in for 0x0", {}, disp(env, 0, 0), "2560x1440 YUV420P8");
  expect_up(env, "the player's display wins over NVVSR_DISPLAY", {}, disp(env, 1920, 1080), "1920x1080 YUV420P8");
  setenv("NVVSR_DISPLAY", "2560", 1);
  expect_pass(env, "NVVSR_DISPLAY=2560 (no 'x') -> unchanged", {}, env.map(), "display size unknown");
  setenv("NVVSR_DISPLAY", "widexhigh", 1);
  expect_pass(env, "NVVSR_DISPLAY=widexhigh -> unchanged", {}, env.map(), "display size unknown");
  setenv("NVVSR_DISPLAY", "0x0", 1);
  expect_pass(env, "NVVSR_DISPLAY=0x0 -> unchanged", {}, env.map(), "display size unknown");
  unsetenv("NVVSR_DISPLAY");

  std::printf("== 4. options\n");
  for (const char *q : {"auto", "bicubic", "low", "medium", "high", "ultra", "streaming_medium", "streaming_ultra", "HIGH"})
    expect_up(env, std::string("quality=") + q, {}, disp(env).s("options", std::string("quality=") + q), "2560x1440 YUV420P8");
  expect_up(env, "strength=0.5", {}, disp(env).s("options", "strength=0.5"), "2560x1440 YUV420P8");
  expect_up(env, "strength=1", {}, disp(env).s("options", "strength=1"), "2560x1440 YUV420P8");
  expect_up(env, "pool=2", {}, disp(env).s("options", "pool=2"), "2560x1440 YUV420P8");
  expect_up(env, "pool=99 is clamped to 4", {}, disp(env).s("options", "pool=99"), "2560x1440 YUV420P8");
  expect_up(env, "pool=0 is clamped to 1", {}, disp(env).s("options", "pool=0"), "2560x1440 YUV420P8");
  expect_up(env, "pool=-3 is clamped to 1", {}, disp(env).s("options", "pool=-3"), "2560x1440 YUV420P8");
  for (const char *m : {"auto", "709", "601"})
    expect_up(env, std::string("matrix=") + m, {}, disp(env).s("options", std::string("matrix=") + m), "2560x1440 YUV420P8");
  expect_up(env, "range=0", {}, disp(env).s("options", "range=0"), "2560x1440 YUV420P8");
  expect_up(env, "range=1", {}, disp(env).s("options", "range=1"), "2560x1440 YUV420P8");
  expect_up(env, "fps=60 is accepted", {}, disp(env).f("fps", 60), "2560x1440 YUV420P8");
  expect_up(env, "fps=-5 is ignored (not > 0), not an error", {}, disp(env).f("fps", -5), "2560x1440 YUV420P8");
  expect_up(env, "empty options", {}, disp(env).s("options", ""), "2560x1440 YUV420P8");
  expect_up(env, "',' separates too, keys are case-insensitive, blanks are trimmed", {},
            disp(env).s("options", " QUALITY = Low , Strength=0.5 ; pool = 2 ;"), "2560x1440 YUV420P8");
  expect_up(env, "a later item replaces an earlier one", {}, disp(env).s("options", "quality=bogus;quality=low"), "2560x1440 YUV420P8");
  {  // the options reach Upscale: its output frame properties show the forced matrix and range
    Run r = run(env, {}, disp(env).s("options", "matrix=601;range=0"));
    vst::Frame f = r.out.frame(0, &err);
    int64_t matrix = -1, range = -1;
    const bool props = f && f.prop_int("_Matrix", &matrix) && f.prop_int("_Range", &range);
    vt::check(props && matrix == 6 && range == 1, "matrix=601;range=0 reach Upscale (_Matrix 6, _Range 1 = full)",
              err + " _Matrix=" + std::to_string(matrix) + " _Range=" + std::to_string(range));
    Run r2 = run(env, {}, disp(env).s("options", "matrix=709;range=1"));
    vst::Frame f2 = r2.out.frame(0, &err);
    vt::check(f2 && f2.prop_int("_Matrix", &matrix) && f2.prop_int("_Range", &range) && matrix == 1 && range == 0,
              "matrix=709;range=1 reach Upscale (_Matrix 1, _Range 0 = limited)", err);
  }
  {  // unknown keys and malformed items are ignored, each with one line, and the clip is still upscaled
    Run r = run(env, {}, disp(env).s("options", "foo=1;quality=low;Bar=2"));
    vt::check(r.desc() == "2560x1440 YUV420P8" && r.lines.size() == 1 && r.lines[0].find("ignoring unknown options: bar, foo") != std::string::npos,
              "unknown keys are ignored with one line (sorted, lower case)", r.why());
    Run m = run(env, {}, disp(env).s("options", "quality;pool=2;;"));
    vt::check(m.desc() == "2560x1440 YUV420P8" && m.lines.size() == 1 && m.lines[0].find("ignoring option item 'quality'") != std::string::npos,
              "an item without '=' is ignored with one line (empty items silently)", m.why());
  }
  expect_pass(env, "strength=abc -> unchanged", {}, disp(env).s("options", "strength=abc"), "strength 'abc' is not a number");
  expect_pass(env, "strength=5 (Upscale refuses) -> unchanged", {}, disp(env).s("options", "strength=5"), "strength must be between 0 and 1");
  expect_pass(env, "strength=nan -> unchanged", {}, disp(env).s("options", "strength=nan"), "strength must be between 0 and 1");
  expect_pass(env, "pool=abc -> unchanged", {}, disp(env).s("options", "pool=abc"), "pool 'abc' is not an integer");
  expect_pass(env, "pool=1.5 -> unchanged", {}, disp(env).s("options", "pool=1.5"), "pool '1.5' is not an integer");
  expect_pass(env, "pool= (empty) -> unchanged", {}, disp(env).s("options", "pool="), "pool '' is not an integer");
  expect_pass(env, "range=x -> unchanged", {}, disp(env).s("options", "range=x"), "range 'x' is not an integer");
  expect_pass(env, "range=2 (Upscale refuses) -> unchanged", {}, disp(env).s("options", "range=2"), "range must be 0 (full) or 1 (limited)");
  expect_pass(env, "quality=superb (Upscale refuses) -> unchanged", {}, disp(env).s("options", "quality=superb"), "quality must be");
  expect_pass(env, "matrix=2020 (Upscale refuses) -> unchanged", {}, disp(env).s("options", "matrix=2020"), "matrix must be");
  expect_pass(env, "one bad item spoils the whole options string", {}, disp(env).s("options", "quality=low;strength=abc"), "strength");

  std::printf("== 5. rendered frames\n");
  {
    Spec ps{1280, 720};
    ps.pattern = true;
    Run r = run(env, ps, disp(env));
    vst::Frame in = r.src.frame(0, &err), up = r.out.frame(0, &err);
    if (vt::check(in && up, "pattern 720p: input and upscaled frame 0 render", err)) {
      vt::check_eq("pattern 720p: frame 0 is 2560x1440 YUV420P8", up.describe(), "2560x1440 YUV420P8");
      int lo, hi;
      min_max_plane(up, 0, &lo, &hi);
      vt::check(lo >= 5 && hi <= 250 && hi - lo > 100, "pattern 720p: Y is not flat and stays in range",
                std::to_string(lo) + ".." + std::to_string(hi));
      const double dy = std::fabs(mean_plane(up, 0) - mean_plane(in, 0));
      const double du = std::fabs(mean_plane(up, 1) - mean_plane(in, 1)), dv = std::fabs(mean_plane(up, 2) - mean_plane(in, 2));
      vt::check(dy < 6 && du < 6 && dv < 6, "pattern 720p: mean Y, U and V stay close to the source",
                std::to_string(dy) + " " + std::to_string(du) + " " + std::to_string(dv));
    }
    Run flat = run(env, {}, disp(env));  // BlankClip colour (90,128,128)
    vst::Frame ff = flat.out.frame(0, &err);
    if (vt::check((bool)ff, "flat 720p: upscaled frame 0 renders", err))
      vt::check(std::fabs(mean_plane(ff, 0) - 90) < 4 && std::fabs(mean_plane(ff, 1) - 128) < 3 && std::fabs(mean_plane(ff, 2) - 128) < 3,
                "flat 720p: the colour (90,128,128) survives",
                std::to_string(mean_plane(ff, 0)) + " " + std::to_string(mean_plane(ff, 1)) + " " + std::to_string(mean_plane(ff, 2)));
    Run pt = run(env, Spec{1280, 720, pfRGB24}, disp(env));
    vst::Frame pf = pt.out.frame(0, &err);
    vt::check(pf && pf.describe() == "1280x720 RGB24", "pass-through: the unchanged RGB24 clip still renders", err);
  }

  std::printf("== 6. SR/GPU unavailable (NVVSR_LIBDIR bogus, in a child process) -> pass-through, no crash\n");
  {
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
      setenv("NVVSR_LIBDIR", "/nonexistent/nvvsr-libdir", 1);
      execl("/proc/self/exe", "test_auto", "--bogus-libdir", (char *)nullptr);
      _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    vt::check(pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "bogus NVVSR_LIBDIR: the child passes all its checks",
              WIFEXITED(status) ? "exit " + std::to_string(WEXITSTATUS(status)) : "killed by a signal");
  }

  return vt::finish("test_auto");
}
