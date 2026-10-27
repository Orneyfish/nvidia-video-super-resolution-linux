// nvcontrol: the NV Control panel (GTK4 + libadwaita).
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#include "app.h"

namespace {

void usage(FILE *to) {
  std::fprintf(to, "usage: nvcontrol [-h]\n\nNV Control: NVIDIA GPU status, power and super resolution settings.\n");
}

}  // namespace

int main(int argc, char **argv) {
  std::optional<SmokeRequest> smoke;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
      usage(stdout);
      return 0;
    }
    if (!std::strcmp(argv[i], "--smoke") && i + 2 < argc) {  // hidden test hook: --smoke SECONDS RESULT_JSON
      char *end = nullptr;
      errno = 0;
      const double seconds = std::strtod(argv[i + 1], &end);
      if (errno || end == argv[i + 1] || *end || !(seconds > 0)) {
        std::fprintf(stderr, "nvcontrol: bad --smoke seconds: %s\n", argv[i + 1]);
        return 2;
      }
      smoke = SmokeRequest{seconds, argv[i + 2]};
      i += 2;
      continue;
    }
    std::fprintf(stderr, "nvcontrol: unrecognized argument: %s\n", argv[i]);
    usage(stderr);
    return 2;
  }
  App app;
  return app.run(smoke);
}
