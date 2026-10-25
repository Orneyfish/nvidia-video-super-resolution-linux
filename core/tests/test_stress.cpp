// Test 5: stress / leaks.
//   test_stress seq    create+process+destroy 100 contexts sequentially; GPU/host memory must not grow
//   test_stress par    two rounds of 4 contexts x 4 threads x 200 frames concurrently; every output must be
//                      bit-identical to a single-threaded reference of the same input; memory must not grow
//                      from round 1 to round 2
//   test_stress leak_nocache  same cycle as seq with the effect cache OFF (NVVSR_CACHE=0): shows the raw
//                      SDK leak (informational, never fails on memory growth)
//   test_stress vram   per-context VRAM cost (nvidia-smi), cold-start cost, informational
//   test_stress oom    (NVVSR_TEST_OOM=1 only) create huge contexts until the GPU is full: must fail cleanly
// GPU memory is read with `nvidia-smi` (device-wide; tolerated if absent -> checks are skipped).
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "test_util.h"

static uint64_t hash_planes(const tu::OutFrame &o) {
  uint64_t h = 1469598103934665603ull;
  for (int k = 0; k < 3; ++k) {
    if (!o.img.plane[k]) continue;
    for (int y = 0; y < o.plane_h[k]; ++y) {
      const uint8_t *row = &o.buf[k][(size_t)y * o.img.stride[k]];
      int x = 0;
      for (; x + 8 <= o.plane_w[k]; x += 8) {
        uint64_t v;
        std::memcpy(&v, row + x, 8);
        h = (h ^ v) * 1099511628211ull;
        h ^= h >> 29;
      }
      for (; x < o.plane_w[k]; ++x) h = (h ^ row[x]) * 1099511628211ull;
    }
  }
  return h;
}

static nvvsr_config cfg_720(int quality = NVVSR_QUALITY_HIGH) {
  nvvsr_config c;
  nvvsr_config_init(&c);
  c.in_w = 1280; c.in_h = 720; c.out_w = 2560; c.out_h = 1440; c.quality = quality;
  return c;
}

static void report_mem(const char *label, long gpu, long rss) {
  std::printf("  %-34s GPU %ld MiB (nvidia-smi)  host RSS %ld MiB\n", label, gpu, rss);
}

static int mode_seq() {
  tu::Yuv420 src = tu::render_scene(1280, 720, 1);
  tu::InFrame in(src, true);
  nvvsr_config cfg = cfg_720();

  uint64_t ref = 0;
  {  // warm-up cycle: pays the one-time costs (CUDA context, lazily loaded kernels)
    nvvsr_ctx *ctx = nullptr;
    T_CHECK_OK(nvvsr_create(&cfg, &ctx));
    if (!ctx) return tu::finish("test_stress seq");
    tu::OutFrame out(2560, 1440, NVVSR_OUT_RGB24_PLANAR);
    T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
    ref = hash_planes(out);
    nvvsr_destroy(ctx);
  }
  long gpu0 = tu::gpu_used_mib(), rss0 = tu::rss_mib();
  report_mem("after warm-up cycle (baseline)", gpu0, rss0);
  const int N = 100;
  int mismatches = 0;
  long rss_settled = -1;  // host RSS once the allocator and the effect cache have warmed up
  double t0 = tu::now_ms();
  for (int i = 0; i < N; ++i) {
    nvvsr_ctx *ctx = nullptr;
    int rc = nvvsr_create(&cfg, &ctx);
    if (rc != NVVSR_OK) {
      T_CHECK_OK(rc);
      break;
    }
    tu::OutFrame out(2560, 1440, NVVSR_OUT_RGB24_PLANAR);
    T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
    if (hash_planes(out) != ref) ++mismatches;
    nvvsr_destroy(ctx);
    if ((i + 1) % 25 == 0) {
      char label[64];
      std::snprintf(label, sizeof label, "after %d create/process/destroy", i + 1);
      report_mem(label, tu::gpu_used_mib(), tu::rss_mib());
      if (i + 1 == 25) rss_settled = tu::rss_mib();
    }
  }
  double total = tu::now_ms() - t0;
  long gpu1 = tu::gpu_used_mib(), rss1 = tu::rss_mib();
  report_mem("final", gpu1, rss1);
  std::printf("  %d contexts in %.1f s (%.0f ms per create+process+destroy)\n", N, total / 1000.0, total / N);
  T_CHECK(mismatches == 0, "%d of %d contexts produced a different output for the same input", mismatches, N);
  if (gpu0 >= 0 && gpu1 >= 0) T_CHECK(gpu1 - gpu0 <= 100, "GPU memory grew by %ld MiB", gpu1 - gpu0);
  else std::printf("  nvidia-smi unavailable: GPU growth check skipped\n");
  // A leak grows with every cycle; first-use costs (allocator arenas, sanitizer shadow/quarantine)
  // are paid in the first cycles. So judge growth after cycle 25, and keep only a loose absolute cap.
  if (rss_settled >= 0 && rss1 >= 0)
    T_CHECK(rss1 - rss_settled <= 50, "host RSS still growing: +%ld MiB over the last 75 cycles", rss1 - rss_settled);
  if (rss0 >= 0 && rss1 >= 0) T_CHECK(rss1 - rss0 <= 300, "host RSS grew by %ld MiB", rss1 - rss0);
  return tu::finish("test_stress seq");
}

struct Barrier {
  std::mutex m;
  std::condition_variable cv;
  int count, waiting = 0, gen = 0;
  explicit Barrier(int n) : count(n) {}
  void wait() {
    std::unique_lock<std::mutex> lk(m);
    int g = gen;
    if (++waiting == count) { waiting = 0; ++gen; cv.notify_all(); }
    else cv.wait(lk, [&] { return g != gen; });
  }
};

struct ParCtx {
  nvvsr_config cfg;
  std::vector<std::unique_ptr<tu::InFrame>> ins;
  std::vector<uint64_t> ref;
};

// One round: T threads, each creates its own context (concurrently), runs FRAMES frames and checks every
// output hash against the single-threaded reference, then all contexts are destroyed.
static void par_round(ParCtx &pc, int T, int FRAMES, int round, double &fps, long &gpu_peak) {
  Barrier created(T + 1), done(T + 1);
  std::atomic<int> mismatches{0}, errors{0};
  std::vector<std::string> err_text(T);
  std::vector<std::thread> threads;
  for (int t = 0; t < T; ++t)
    threads.emplace_back([&, t] {
      nvvsr_ctx *ctx = nullptr;
      int rc = nvvsr_create(&pc.cfg, &ctx);  // concurrent creation
      if (rc != NVVSR_OK) { ++errors; err_text[t] = std::string("create: ") + nvvsr_last_error(); }
      created.wait();
      if (ctx) {
        tu::OutFrame out(2560, 1440, NVVSR_OUT_RGB24_PLANAR);
        for (int f = 0; f < FRAMES; ++f) {
          rc = nvvsr_process(ctx, &pc.ins[t]->img, &out.img);
          if (rc != NVVSR_OK) { ++errors; err_text[t] = std::string("process: ") + nvvsr_last_error(); break; }
          if (hash_planes(out) != pc.ref[t]) ++mismatches;
        }
      }
      done.wait();  // keep contexts alive while main samples memory
      nvvsr_destroy(ctx);
    });
  created.wait();
  double t_start = tu::now_ms();
  done.wait();
  double secs = (tu::now_ms() - t_start) / 1000.0;
  gpu_peak = tu::gpu_used_mib();
  for (auto &th : threads) th.join();
  fps = T * FRAMES / secs;
  std::printf("  round %d: %d threads x %d frames in %.2f s = %.0f frames/s aggregate (%.1f ms per frame per thread, incl. hashing)\n",
              round, T, FRAMES, secs, fps, secs * 1000.0 / FRAMES);
  for (int t = 0; t < T; ++t) if (!err_text[t].empty()) std::printf("  thread %d: %s\n", t, err_text[t].c_str());
  T_CHECK(errors == 0, "round %d: %d errors in worker threads", round, errors.load());
  T_CHECK(mismatches == 0, "round %d: %d frames differ from the single-threaded reference (cross-context interference!)", round,
          mismatches.load());
}

static int mode_par() {
  const int T = 4, FRAMES = 200;
  ParCtx pc;
  pc.cfg = cfg_720();
  std::vector<tu::Yuv420> srcs;
  for (int t = 0; t < T; ++t) {
    srcs.push_back(tu::render_scene(1280, 720, 11 + 5 * t));
    pc.ins.emplace_back(new tu::InFrame(srcs.back(), true));
  }
  pc.ref.resize(T);
  {  // single-threaded reference of every input
    nvvsr_ctx *ctx = nullptr;
    T_CHECK_OK(nvvsr_create(&pc.cfg, &ctx));
    if (!ctx) return tu::finish("test_stress par");
    for (int t = 0; t < T; ++t) {
      tu::OutFrame out(2560, 1440, NVVSR_OUT_RGB24_PLANAR);
      T_CHECK_OK(nvvsr_process(ctx, &pc.ins[t]->img, &out.img));
      pc.ref[t] = hash_planes(out);
    }
    for (int t = 1; t < T; ++t) T_CHECK(pc.ref[t] != pc.ref[0], "inputs %d and 0 produce the same output: test inputs not distinct", t);
    nvvsr_destroy(ctx);
  }
  report_mem("baseline (1 parked effect)", tu::gpu_used_mib(), tu::rss_mib());

  // Round 1 creates the 4 SDK effects (one-time cost), round 2 settles allocator arenas; the leak
  // criterion is the growth from the end of round 2 to the end of round 3.
  double fps = 0;
  long peak = 0, gpu[4] = {0, 0, 0, 0}, rss[4] = {0, 0, 0, 0};
  for (int r = 1; r <= 3; ++r) {
    par_round(pc, T, FRAMES, r, fps, peak);
    gpu[r] = tu::gpu_used_mib();
    rss[r] = tu::rss_mib();
    char label[64];
    std::snprintf(label, sizeof label, "round %d: 4 live contexts (peak)", r);
    report_mem(label, peak, -1);
    std::snprintf(label, sizeof label, "after round %d (all destroyed)", r);
    report_mem(label, gpu[r], rss[r]);
  }
  if (gpu[2] >= 0 && gpu[3] >= 0) T_CHECK(gpu[3] - gpu[2] <= 100, "GPU memory grew by %ld MiB between rounds 2 and 3", gpu[3] - gpu[2]);
  else std::printf("  nvidia-smi unavailable: GPU growth check skipped\n");
  if (rss[2] >= 0 && rss[3] >= 0) T_CHECK(rss[3] - rss[2] <= 100, "host RSS grew by %ld MiB between rounds 2 and 3", rss[3] - rss[2]);
  return tu::finish("test_stress par");
}

static int mode_vram() {
  // Effect cache off, so that every number below is "what one more context costs while it is alive".
  setenv("NVVSR_CACHE", "0", 1);
  long cold = tu::gpu_used_mib();
  report_mem("cold (before the first context)", cold, tu::rss_mib());
  struct C { int iw, ih, ow, oh, q; const char *name; };
  const C cs[] = {{854, 480, 2560, 1440, NVVSR_QUALITY_ULTRA, "480p->1440p ULTRA"},   {1280, 720, 2560, 1440, NVVSR_QUALITY_HIGH, "720p->1440p HIGH"},
                  {1920, 1080, 2560, 1440, NVVSR_QUALITY_MEDIUM, "1080p->1440p MEDIUM"}, {1920, 1080, 2560, 1440, NVVSR_QUALITY_HIGH, "1080p->1440p HIGH"},
                  {1920, 1080, 2560, 1440, NVVSR_QUALITY_ULTRA, "1080p->1440p ULTRA"}};
  bool first = true;
  for (const C &c : cs) {
    nvvsr_config cfg;
    nvvsr_config_init(&cfg);
    cfg.in_w = c.iw; cfg.in_h = c.ih; cfg.out_w = c.ow; cfg.out_h = c.oh; cfg.quality = c.q;
    tu::Yuv420 src = tu::render_scene(c.iw, c.ih, 1);
    tu::InFrame in(src, true);
    tu::OutFrame out(c.ow, c.oh, NVVSR_OUT_RGB24_PLANAR);
    // our own GPU buffers (nominal): YUV in + RGBA in + RGBA out + planar RGB out
    double buffers = ((double)c.iw * c.ih * 1.5 + (double)c.iw * c.ih * 4 + (double)c.ow * c.oh * 4 + (double)c.ow * c.oh * 3) / (1024.0 * 1024.0);
    long base = tu::gpu_used_mib();
    std::vector<nvvsr_ctx *> ctxs;
    long after1 = -1, after4 = -1;
    for (int i = 0; i < 4; ++i) {
      nvvsr_ctx *ctx = nullptr;
      T_CHECK_OK(nvvsr_create(&cfg, &ctx));
      if (!ctx) break;
      T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
      ctxs.push_back(ctx);
      if (i == 0) after1 = tu::gpu_used_mib();
    }
    after4 = tu::gpu_used_mib();
    if (first) {
      std::printf("  cold start cost (CUDA context + first context, %s): %ld MiB\n", c.name, after1 - cold);
      first = false;
    }
    if (base >= 0 && after1 >= 0 && ctxs.size() == 4) {
      double marginal = (after4 - after1) / 3.0;
      std::printf("  %-22s each context while alive: +%.0f MiB  (frame buffers %.0f MiB + SDK effect/workspace %.0f MiB); "
                  "first one (incl. one-time CUDA/NGX init if cold): +%ld MiB\n", c.name, marginal, buffers, marginal - buffers, after1 - base);
      T_CHECK(marginal > 50 && marginal < 1800, "per-context VRAM %.0f MiB looks wrong", marginal);
    } else {
      std::printf("  %-22s nvidia-smi unavailable\n", c.name);
    }
    for (auto *x : ctxs) nvvsr_destroy(x);
    long end = tu::gpu_used_mib();
    if (base >= 0 && end >= 0)
      std::printf("  %-22s after destroying the 4: +%ld MiB stay allocated (SDK leak, effects created while others lived)\n", c.name, end - base);
  }
  return tu::finish("test_stress vram");
}

// Same loop as `seq` but with the effect cache off: the raw SDK behaviour. Informational.
static int mode_leak_nocache() {
  setenv("NVVSR_CACHE", "0", 1);
  if (int s = tu::skip_unless_usable("test_stress leak_nocache")) return s;
  tu::Yuv420 src = tu::render_scene(1280, 720, 1);
  tu::InFrame in(src, true);
  nvvsr_config cfg = cfg_720();
  for (int warm = 0; warm < 1; ++warm) {
    nvvsr_ctx *ctx = nullptr;
    T_CHECK_OK(nvvsr_create(&cfg, &ctx));
    tu::OutFrame out(2560, 1440, NVVSR_OUT_RGB24_PLANAR);
    if (ctx) T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
    nvvsr_destroy(ctx);
  }
  long gpu0 = tu::gpu_used_mib(), rss0 = tu::rss_mib();
  report_mem("baseline after warm-up", gpu0, rss0);
  const int N = 20;
  double t0 = tu::now_ms();
  for (int i = 0; i < N; ++i) {
    nvvsr_ctx *ctx = nullptr;
    T_CHECK_OK(nvvsr_create(&cfg, &ctx));
    tu::OutFrame out(2560, 1440, NVVSR_OUT_RGB24_PLANAR);
    if (ctx) T_CHECK_OK(nvvsr_process(ctx, &in.img, &out.img));
    nvvsr_destroy(ctx);
  }
  double ms = (tu::now_ms() - t0) / N;
  long gpu1 = tu::gpu_used_mib(), rss1 = tu::rss_mib();
  report_mem("after cycles", gpu1, rss1);
  std::printf("  uncached: %.0f ms per create+process+destroy; host RSS +%.1f MiB per cycle, GPU %+ld MiB total (SDK: ~12.5 MiB host per NvVFX_Load)\n",
              ms, N ? (double)(rss1 - rss0) / N : 0.0, gpu1 - gpu0);
  return tu::finish("test_stress leak_nocache");
}

static int mode_oom() {
  const char *e = getenv("NVVSR_TEST_OOM");
  if (!e || std::string(e) != "1") {
    std::printf("test_stress oom: SKIP: set NVVSR_TEST_OOM=1 to fill the GPU on purpose\n");
    return 77;
  }
  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = 3840; cfg.in_h = 2160; cfg.out_w = 7680; cfg.out_h = 4320; cfg.quality = NVVSR_QUALITY_HIGH;
  tu::Yuv420 src = tu::render_scene(3840, 2160, 1);
  tu::InFrame in(src, true);
  tu::OutFrame out(7680, 4320, NVVSR_OUT_RGB24_PLANAR);
  std::vector<nvvsr_ctx *> ctxs;
  int fail_rc = NVVSR_OK;
  for (int i = 0; i < 24; ++i) {
    nvvsr_ctx *ctx = nullptr;
    int rc = nvvsr_create(&cfg, &ctx);
    if (rc != NVVSR_OK) {
      fail_rc = rc;
      T_CHECK(ctx == nullptr, "failed create left a context");
      std::printf("  create #%d failed (GPU used %ld MiB): %s -- %s\n", i, tu::gpu_used_mib(), nvvsr_strerror(rc), nvvsr_last_error());
      break;
    }
    rc = nvvsr_process(ctx, &in.img, &out.img);
    if (rc != NVVSR_OK) {
      fail_rc = rc;
      std::printf("  process on ctx #%d failed: %s -- %s\n", i, nvvsr_strerror(rc), nvvsr_last_error());
      nvvsr_destroy(ctx);
      break;
    }
    ctxs.push_back(ctx);
    std::printf("  ctx #%d ok, GPU used %ld MiB\n", i, tu::gpu_used_mib());
  }
  T_CHECK(fail_rc != NVVSR_OK, "GPU never ran out of memory with %zu 4K->8K contexts", ctxs.size());
  T_CHECK(fail_rc == NVVSR_ERR_OUT_OF_MEMORY, "expected a clean NVVSR_ERR_OUT_OF_MEMORY, got %d (%s)", fail_rc, nvvsr_strerror(fail_rc));
  for (auto *c : ctxs) nvvsr_destroy(c);
  // the library must still work after an OOM
  nvvsr_config small = cfg;
  small.in_w = 640; small.in_h = 360; small.out_w = 1280; small.out_h = 720;
  nvvsr_ctx *ctx = nullptr;
  T_CHECK_OK(nvvsr_create(&small, &ctx));
  if (ctx) {
    tu::Yuv420 s2 = tu::render_scene(640, 360, 1);
    tu::InFrame i2(s2, true);
    tu::OutFrame o2(1280, 720, NVVSR_OUT_RGB24_PLANAR);
    T_CHECK_OK(nvvsr_process(ctx, &i2.img, &o2.img));
    nvvsr_destroy(ctx);
  }
  return tu::finish("test_stress oom");
}

int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "seq";
  if (mode == "oom" && !getenv("NVVSR_TEST_OOM")) {
    std::printf("test_stress oom: SKIP: set NVVSR_TEST_OOM=1 to fill the GPU on purpose\n");
    return 77;
  }
  if (int s = tu::skip_unless_usable("test_stress")) return s;
  if (mode == "seq") return mode_seq();
  if (mode == "par") return mode_par();
  if (mode == "vram") return mode_vram();
  if (mode == "leak_nocache") return mode_leak_nocache();
  if (mode == "oom") return mode_oom();
  std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
  return 2;
}
