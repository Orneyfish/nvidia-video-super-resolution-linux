/* Stand-in for libnvvsr.so in tests: same exported names, no GPU, no NVIDIA libraries. */
#include <stdlib.h>
static const char *last = "";
const char *nvvsr_version(void) { return "9.9.9-fake"; }
int nvvsr_api_version(void) { return 1; }
int nvvsr_probe(void) {
  const char *rc = getenv("FAKE_PROBE_RC");
  if (!getenv("NVVSR_LIBDIR")) { last = "NVVSR_LIBDIR is not set"; return -3; }
  if (rc && atoi(rc)) { last = "fake: no CUDA device"; return atoi(rc); }
  last = "";
  return 0;
}
const char *nvvsr_strerror(int s) { return s == 0 ? "ok" : s == -5 ? "no NVIDIA GPU or CUDA device" : s == -3 ? "no library directory" : "error"; }
const char *nvvsr_last_error(void) { return last; }
