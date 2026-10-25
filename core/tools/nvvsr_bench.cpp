// nvvsr_bench: per-stage timing of libnvvsr for one configuration.
//   nvvsr_bench 1920x1080 2560x1440 [-q high] [-n 200] [--in nv12|i420] [--out rgb24|rgba|i420]
//                                   [--pageable] [--matrix 601|709] [--full] [--fps 60]
// Needs NVVSR_LIBDIR (or ~/.config/nvvsr/libdir). "wall" = time of the whole nvvsr_process()
// call as a front-end sees it (timing flag off); the stage columns come from a second
// run with NVVSR_FLAG_TIMING (a stream sync after each stage).
#include <cstring>
#include <string>

#include "bench_core.h"

static bool parse_size(const char *s, int &w, int &h) { return std::sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0; }

static int parse_quality(const char *s) {
  struct { const char *n; int q; } tab[] = {{"auto", -1}, {"bicubic", 0}, {"low", 1}, {"medium", 2}, {"high", 3},
                                            {"ultra", 4}, {"smedium", 21}, {"sultra", 23}};
  for (auto &t : tab) if (!std::strcmp(s, t.n)) return t.q;
  return std::atoi(s);
}

static void usage() {
  std::fprintf(stderr,
               "usage: nvvsr_bench IWxIH OWxOH [-q auto|bicubic|low|medium|high|ultra|smedium|sultra|N] [-n ITERS]\n"
               "                   [--in nv12|i420] [--out rgb24|rgba|i420] [--pageable] [--matrix 601|709]\n"
               "                   [--full] [--fps F]\n");
}

int main(int argc, char **argv) {
  bench::Cfg c;
  if (argc < 3 || !parse_size(argv[1], c.iw, c.ih) || !parse_size(argv[2], c.ow, c.oh)) {
    usage();
    return 2;
  }
  for (int i = 3; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "-q") c.quality = parse_quality(next());
    else if (a == "-n") c.iters = std::max(1, std::atoi(next()));
    else if (a == "--in") c.in_format = !std::strcmp(next(), "i420") ? NVVSR_IN_I420 : NVVSR_IN_NV12;
    else if (a == "--out") {
      std::string o = next();
      c.out_format = o == "rgba" ? NVVSR_OUT_RGBA_CHUNKY : o == "i420" ? NVVSR_OUT_I420 : NVVSR_OUT_RGB24_PLANAR;
    } else if (a == "--pageable") c.pinned = false;
    else if (a == "--pinned") c.pinned = true;
    else if (a == "--matrix") c.matrix = std::atoi(next()) == 601 ? NVVSR_MATRIX_BT601 : NVVSR_MATRIX_BT709;
    else if (a == "--full") c.full_range = true;
    else if (a == "--fps") c.fps = std::atof(next());
    else { usage(); return 2; }
  }
  int pr = nvvsr_probe();
  if (pr != NVVSR_OK) {
    std::fprintf(stderr, "nvvsr_bench: SR unavailable: %s (%s)\n", nvvsr_strerror(pr), nvvsr_last_error());
    return 1;
  }
  std::printf("libnvvsr %s, %d iterations\n", nvvsr_version(), c.iters);
  bench::print_header();
  bench::Result r = bench::run(c);
  bench::print_row(c, r);
  return r.rc == NVVSR_OK ? 0 : 1;
}
