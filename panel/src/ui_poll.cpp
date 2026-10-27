#include "ui_poll.h"

#include <gio/gio.h>

#include <memory>
#include <vector>

#include "gpu_sysfs.h"

namespace {

struct Work {
  std::function<void()> job, done;
};

// Runs job on a GLib worker thread, then done on the main loop (GTask completes through the main context's idle
// queue, so a slow nvidia-smi never blocks GTK). Neither function may throw.
void run_async(std::function<void()> job, std::function<void()> done) {
  Work *w = new Work{std::move(job), std::move(done)};  // owned by the task, which outlives the completion callback
  GTask *task = g_task_new(nullptr, nullptr,
                           [](GObject *, GAsyncResult *, gpointer d) { static_cast<Work *>(d)->done(); }, w);
  g_task_set_task_data(task, w, [](gpointer d) { delete static_cast<Work *>(d); });
  g_task_run_in_thread(task, [](GTask *t, gpointer, gpointer data, GCancellable *) {
    static_cast<Work *>(data)->job();
    g_task_return_boolean(t, TRUE);
  });
  g_object_unref(task);
}

}  // namespace

struct UiPoll::Core {
  Probe probe;
  std::vector<Listener> listeners;
  PollEvent last;
  bool stopped = false;
  PollEngine engine;

  static PollEngine::Env system_env() {
    PollEngine::Env env;
    const std::vector<PciGpu> gpus = find_nvidia_gpus();
    env.gpu_present = !gpus.empty();
    if (env.gpu_present) env.runtime = [gpu = gpus.front()] { return read_runtime(&gpu); };
    return env;
  }

  explicit Core(Probe p) : probe(std::move(p)), engine(system_env(), [this](const PollEvent &e) { publish(e); }) {}

  void publish(const PollEvent &e) {
    last = e;
    for (size_t i = 0; i < listeners.size(); ++i) {  // by index and by copy: a listener may add listeners
      const Listener fn = listeners[i];
      fn(e);
    }
  }
};

UiPoll::UiPoll(Probe probe) : core_(std::make_shared<Core>(std::move(probe))) { core_->engine.announce(); }

UiPoll::~UiPoll() { stop(); }

void UiPoll::add_listener(Listener fn) { core_->listeners.push_back(std::move(fn)); }

const PollEvent &UiPoll::last() const { return core_->last; }

void UiPoll::start() {
  if (timer_) return;
  core_->stopped = false;
  core_->engine.announce();
  // The hybrid mode never changes without a reboot-level switch: read once, off the main loop.
  auto mode = std::make_shared<std::optional<std::string>>();
  run_async([core = core_, mode] { *mode = core->engine.read_hybrid(); },
            [core = core_, mode] {
              if (!core->stopped) core->engine.set_hybrid(*mode);
            });
  timer_ = g_timeout_add_seconds(
      1, [](gpointer d) -> gboolean { static_cast<UiPoll *>(d)->tick_now(); return G_SOURCE_CONTINUE; }, this);
}

void UiPoll::stop() {
  if (timer_) g_source_remove(timer_);
  timer_ = 0;
  core_->stopped = true;
}

void UiPoll::tick_now() {
  if (core_->stopped) return;
  if (const auto req = core_->engine.tick(core_->probe())) start_query(*req);
}

void UiPoll::query_now(bool wake) {
  if (core_->stopped) return;
  if (const auto req = core_->engine.query_now(wake)) start_query(*req);
}

bool UiPoll::refresh() {
  if (core_->engine.asleep_now()) {
    tick_now();
    return false;
  }
  query_now(true);
  return true;
}

void UiPoll::start_query(const PollRequest &req) {
  auto res = std::make_shared<PollResult>();
  run_async([core = core_, req, res] { *res = core->engine.read(req); },
            [core = core_, res] {
              if (!core->stopped) core->engine.finish(*res);
            });
}
