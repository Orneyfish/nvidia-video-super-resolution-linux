// nvvsr_create / nvvsr_process / nvvsr_destroy must work on a thread with a small stack: hosts such as VLC run
// plugin code on 1 MiB stacks, and NVIDIA's NGX init (inside nvvsr_create) needs much more than that.
// Without the library's internal big-stack init thread this test dies with SIGSEGV.
#include <pthread.h>

#include <cstdio>
#include <cstdlib>

#include "test_util.h"

namespace {

struct Job {
  size_t stack;
  int create_rc = -100, process_rc = -100;
  bool ran = false;
};

void *run(void *p) {
  Job *j = static_cast<Job *>(p);
  const int W = 640, H = 360;
  tu::Yuv420 src = tu::render_scene(W, H, 3);
  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = W; cfg.in_h = H; cfg.out_w = 2 * W; cfg.out_h = 2 * H;
  cfg.quality = NVVSR_QUALITY_MEDIUM;
  nvvsr_ctx *ctx = nullptr;
  j->create_rc = nvvsr_create(&cfg, &ctx);
  if (j->create_rc == NVVSR_OK && ctx) {
    tu::InFrame in(src, true);
    tu::OutFrame out(2 * W, 2 * H, NVVSR_OUT_RGB24_PLANAR);
    j->process_rc = nvvsr_process(ctx, &in.img, &out.img);
    nvvsr_destroy(ctx);
  }
  j->ran = true;
  return nullptr;
}

}  // namespace

int main() {
  if (int s = tu::skip_unless_usable("test_smallstack")) return s;
  // 128 KiB is far below PTHREAD_STACK_MIN-using hosts' usual 1 MiB: it must work on 1 MiB, 256 KiB and 128 KiB.
  const size_t stacks[] = {1u << 20, 256u << 10, 128u << 10};
  for (size_t stack : stacks) {
    Job job;
    job.stack = stack;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    T_CHECK(pthread_attr_setstacksize(&attr, stack) == 0, "cannot set a %zu byte stack", stack);
    pthread_t th;
    T_CHECK(pthread_create(&th, &attr, run, &job) == 0, "cannot start a thread with a %zu byte stack", stack);
    pthread_join(th, nullptr);
    pthread_attr_destroy(&attr);
    std::printf("  %7zu byte stack: create %d, process %d\n", stack, job.create_rc, job.process_rc);
    T_CHECK(job.ran, "thread did not finish");
    T_CHECK(job.create_rc == NVVSR_OK, "nvvsr_create on a %zu byte stack: %d (%s)", stack, job.create_rc, nvvsr_strerror(job.create_rc));
    T_CHECK(job.process_rc == NVVSR_OK, "nvvsr_process on a %zu byte stack: %d", stack, job.process_rc);
  }
  return tu::finish("test_smallstack");
}
