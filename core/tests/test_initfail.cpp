// If nvvsr_create cannot start its internal init thread it must fail cleanly: initialising on the caller's stack is
// the crash that thread exists to avoid (NGX segfaults on stacks <= 1 MiB). pthread_create is interposed here
// (the executable's symbol wins over libc's for libnvvsr's PLT calls) to fail once with EAGAIN.
#include <dlfcn.h>
#include <pthread.h>

#include <cerrno>

#include "test_util.h"

static bool g_fail_next = false;

extern "C" int pthread_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
  static auto real = reinterpret_cast<int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *)>(
      dlsym(RTLD_NEXT, "pthread_create"));
  if (g_fail_next) {
    g_fail_next = false;
    return EAGAIN;
  }
  return real(th, attr, fn, arg);
}

int main() {
  if (int s = tu::skip_unless_usable("test_initfail")) return s;
  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = 640; cfg.in_h = 360; cfg.out_w = 1280; cfg.out_h = 720;
  cfg.quality = NVVSR_QUALITY_MEDIUM;
  nvvsr_ctx *ctx = nullptr;
  g_fail_next = true;
  int rc = nvvsr_create(&cfg, &ctx);
  T_CHECK(!g_fail_next, "nvvsr_create never tried to start a thread");
  T_CHECK(rc == NVVSR_ERR_INTERNAL && !ctx, "create with no init thread: rc %d, ctx %p", rc, (void *)ctx);
  std::printf("  no init thread: rc %d (%s)\n", rc, nvvsr_last_error());
  rc = nvvsr_create(&cfg, &ctx);  // the library is still usable afterwards
  T_CHECK_OK(rc);
  if (rc == NVVSR_OK) nvvsr_destroy(ctx);
  return tu::finish("test_initfail");
}
