// Concurrency of core.nvvsr.Upscale: C++ port of section 4 of run.sh ("results independent of pool size and
// request depth; no deadlock"). Frames are requested asynchronously (vst::render_async, getFrameAsync), several in
// flight, like vspipe --requests. Part 1: 24 frames give the same output at pool/requests 1/1, 2/4, 3/8 and 4/4.
// Part 2 (new): 240 frames whose _Matrix alternates between BT.709 and BT.601, 8 requests in flight, at pool 1, 2
// and 4: every frame wants a different colour setting than its neighbour, so the pool keeps recolouring contexts.
// A watchdog thread aborts the whole test with a message when a phase hangs. These are functional checks, not
// benchmarks: no timings are reported. Needs NVVSR_PLUGIN (ctest sets it) and the NVIDIA libraries (NVVSR_LIBDIR).
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "vs_check.h"
#include "vs_harness.h"

using vst::Env;
using vst::Spec;

// Ends the process (exit status 1) when it is not destroyed within `seconds`: a deadlocked render never returns, and
// ctest's own timeout would not say which phase hung.
class Watchdog {
 public:
  Watchdog(int seconds, std::string what)
      : thread_([this, seconds, what] {
          std::unique_lock<std::mutex> lk(mu_);
          if (cv_.wait_for(lk, std::chrono::seconds(seconds), [this] { return done_; })) return;
          std::printf("FAIL deadlock? %s did not finish within %d s, aborting the test\n", what.c_str(), seconds);
          std::printf("test_concurrency: aborted by the watchdog after %d checks, %d failed\n", vt::checks(), vt::failures() + 1);
          std::fflush(stdout);
          _exit(1);
        }) {}
  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      done_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }
  Watchdog(const Watchdog &) = delete;
  Watchdog &operator=(const Watchdog &) = delete;

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread thread_;  // last: it starts using the members above at once
};

// FNV-1a over the samples of all planes (row padding excluded); 0 never comes out for a real frame.
static uint64_t hash_frame(const vst::Frame &f) {
  uint64_t h = 1469598103934665603ull;
  for (int p = 0; p < 3; ++p)
    for (int y = 0; y < f.height(p); ++y) {
      const uint8_t *row = f.plane(p) + (ptrdiff_t)y * f.stride(p);
      for (int x = 0; x < f.width(p); ++x) h = (h ^ row[x]) * 1099511628211ull;
    }
  return h;
}

static std::string hex(uint64_t v) {
  char buf[20];
  std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)v);
  return buf;
}

struct Render {
  std::string err;                // "" when all frames arrived
  std::vector<uint64_t> hashes;   // per frame number
  std::string first;              // describe() of frame 0
  uint64_t all() const {          // one hash for the whole sequence, in frame order
    uint64_t h = 1469598103934665603ull;
    for (uint64_t v : hashes)
      for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 0xff)) * 1099511628211ull;
    return h;
  }
  bool complete() const {
    for (uint64_t v : hashes)
      if (!v) return false;
    return err.empty() && !hashes.empty();
  }
};

// All `frames` frames of up.clip, `depth` requests in flight, under a watchdog.
static Render render(const Env &env, const vst::Result &up, int frames, int depth, const std::string &what, int timeout_s) {
  Render r;
  r.hashes.assign((size_t)frames, 0);
  if (!up) {
    r.err = up.error;
    return r;
  }
  Watchdog wd(timeout_s, what);
  r.err = vst::render_async(env, up.clip, frames, depth, [&r](int n, vst::Frame &&f) {
    r.hashes[(size_t)n] = hash_frame(f);
    if (n == 0) r.first = f.describe();
  });
  return r;
}

static vst::Result upscale(const Env &env, const vst::Result &src, int w, int h, int pool) {
  return env.then(src, "nvvsr", "Upscale", env.map().i("width", w).i("height", h).s("quality", "medium").i("pool", pool));
}

int main() {
  Env env;  // first: it must outlive every Map, Node and Frame below
  if (!env) {
    std::printf("FAIL cannot create a VapourSynth core\n");
    return 1;
  }
  std::string err;
  if (!vt::check(env.load_nvvsr(&err), "plugin loads", err)) return vt::finish("test_concurrency");

  std::printf("== 4. concurrency: results independent of pool size and request depth; no deadlock\n");
  {  // 24 frames of a patterned 1080p clip, MEDIUM, 1080p -> 1440p
    Spec s;
    s.w = 1920;
    s.h = 1080;
    s.length = 24;
    s.pattern = true;
    const vst::Result src = env.test_clip(s);
    if (!vt::check((bool)src, "24-frame 1080p test clip", src.error)) return vt::finish("test_concurrency");
    const int configs[4][2] = {{1, 1}, {2, 4}, {3, 8}, {4, 4}};  // pool, requests in flight
    uint64_t want = 0;
    std::string want_cfg;
    for (const auto &c : configs) {
      const int pool = c[0], depth = c[1];
      const std::string cfg = "pool=" + std::to_string(pool) + " requests=" + std::to_string(depth);
      const vst::Result up = upscale(env, src, 2560, 1440, pool);
      const Render r = render(env, up, 24, depth, "24 frames at " + cfg, 240);
      const std::string name = "24 frames 1080p->1440p MEDIUM, " + cfg;
      if (!vt::check(r.complete() && r.first == "2560x1440 RGB24", name + ": all frames arrive, 2560x1440 RGB24",
                     r.err + " first='" + r.first + "'"))
        continue;
      if (want_cfg.empty()) {
        want = r.all();
        want_cfg = cfg;
        vt::check(want != 0, name + ": reference output hash " + hex(want));
      } else {
        vt::check(r.all() == want, name + ": output identical to " + want_cfg + " (" + hex(r.all()) + ")",
                  "hash " + hex(r.all()) + " != " + hex(want));
      }
    }
  }

  std::printf("== 4b. stress: 240 frames alternating BT.709 / BT.601, 8 requests in flight, pool 1, 2 and 4\n");
  {  // frame 2k is picture k tagged _Matrix=1 (BT.709), frame 2k+1 the same picture tagged _Matrix=6 (BT.601)
    Spec s;
    s.w = 1280;
    s.h = 720;
    s.length = 120;
    s.pattern = true;
    const vst::Result src = env.test_clip(s);
    const vst::Result a = env.then(src, "std", "SetFrameProps", env.map().i("_Matrix", 1));
    const vst::Result b = env.then(src, "std", "SetFrameProps", env.map().i("_Matrix", 6));
    vst::Result alt = a && b ? env.call("std", "Interleave", env.map().node("clips", a.clip).node("clips", b.clip, true))
                             : vst::Result{a ? b.error : a.error, vst::Node()};
    if (!vt::check((bool)alt, "alternating-matrix test clip (Interleave of two SetFrameProps)", alt.error))
      return vt::finish("test_concurrency");
    Render ref;
    for (int pool : {1, 2, 4}) {
      const std::string name = "240 frames 709/601 alternating, pool=" + std::to_string(pool) + " requests=8";
      const vst::Result up = upscale(env, alt, 1920, 1080, pool);
      Render r = render(env, up, 240, 8, name, 300);
      if (!vt::check(r.complete() && r.first == "1920x1080 RGB24", name + ": no error, no deadlock, all frames arrive",
                     r.err + " first='" + r.first + "'"))
        continue;
      if (ref.hashes.empty()) {
        int same = 0;  // the same picture must come out differently when tagged 709 and 601
        for (size_t k = 0; k + 1 < r.hashes.size(); k += 2) same += r.hashes[k] == r.hashes[k + 1];
        vt::check(same == 0, "709 and 601 frames of the same picture differ (both matrices are honoured)",
                  std::to_string(same) + " of 120 pairs are identical");
        ref = std::move(r);
      } else {
        vt::check(r.hashes == ref.hashes, name + ": every frame identical to pool=1",
                  "sequence hash " + hex(r.all()) + " != " + hex(ref.all()));
      }
    }
  }

  return vt::finish("test_concurrency");
}
