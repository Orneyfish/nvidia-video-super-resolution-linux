// Test 6: timing report. Prints per-stage timings (upload / convert / SR / download) for the
// configurations of interest and compares them with the Phase 0 SR-only numbers. This is the real
// per-frame cost for a caller that has host-memory frames. Sanity bounds only, no tight assertions.
#include "bench_core.h"
#include "test_util.h"

struct Case { int iw, ih, ow, oh, q; double phase0_sr_ms; };

int main() {
  if (int s = tu::skip_unless_usable("test_timing")) return s;

  const Case cases[] = {
      {1920, 1080, 2560, 1440, NVVSR_QUALITY_MEDIUM, 3.56}, {1920, 1080, 2560, 1440, NVVSR_QUALITY_HIGH, 8.16},
      {1280, 720, 2560, 1440, NVVSR_QUALITY_HIGH, 3.68},    {1280, 720, 2560, 1440, NVVSR_QUALITY_ULTRA, 5.12},
      {854, 480, 2560, 1440, NVVSR_QUALITY_ULTRA, 2.76},
  };
  std::printf("  libnvvsr %s; all rows NV12 -> RGB24_PLANAR; 'sum' = upload+convert+SR+download; 'wall' = whole call\n", nvvsr_version());
  bench::print_header();
  for (const Case &c : cases)
    for (bool pinned : {true, false}) {
      bench::Cfg b;
      b.iw = c.iw; b.ih = c.ih; b.ow = c.ow; b.oh = c.oh; b.quality = c.q; b.iters = 200; b.pinned = pinned;
      bench::Result r = bench::run(b);
      bench::print_row(b, r);
      T_CHECK(r.rc == NVVSR_OK, "bench failed: %s [%s]", nvvsr_strerror(r.rc), nvvsr_last_error());
      if (r.rc != NVVSR_OK) continue;
      std::printf("      -> SR stage %.2f ms (Phase 0 SR-only %.2f ms), overhead of upload+convert+download %.2f ms\n", r.stage.sr_ms,
                  c.phase0_sr_ms, r.stage.upload_ms + r.stage.convert_ms + r.stage.download_ms);
      T_CHECK(r.wall_mean < 60.0, "implausibly slow: %.1f ms/frame", r.wall_mean);
      T_CHECK(r.stage.sr_ms < c.phase0_sr_ms * 2.0 + 2.0, "SR stage %.2f ms much slower than Phase 0 (%.2f)", r.stage.sr_ms, c.phase0_sr_ms);
    }
  return tu::finish("test_timing");
}
