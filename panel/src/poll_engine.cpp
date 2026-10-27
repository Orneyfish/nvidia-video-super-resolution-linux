#include "poll_engine.h"

#include <exception>

PollEngine::PollEngine(Env env, Publish publish) : env_(std::move(env)), publish_(std::move(publish)) {}

void PollEngine::read_state() {
  runtime_ = env_.gpu_present ? env_.runtime() : RuntimeInfo{};
  state_ = gpu_state(runtime_.status);
  power_ = env_.power_source();
}

PollEvent PollEngine::event(PollEvent::Kind kind) const {
  PollEvent e;
  e.kind = kind;
  e.gpu_present = env_.gpu_present;
  e.state = state_;
  e.stale = kind == PollEvent::Kind::Asleep || kind == PollEvent::Kind::Stale ||
            (kind == PollEvent::Kind::Awake && !error_.empty());
  e.sample = sample_;
  e.apps = apps_;
  e.hybrid = hybrid_;
  e.power_source = power_;
  e.runtime = runtime_;
  e.error = error_;
  return e;
}

void PollEngine::publish_state() {
  publish_(event(asleep() ? PollEvent::Kind::Asleep : PollEvent::Kind::Awake));
}

void PollEngine::announce() {
  read_state();
  publish_state();
}

void PollEngine::set_hybrid(std::optional<std::string> mode) {
  hybrid_ = std::move(mode);
  announce();
}

bool PollEngine::asleep_now() {
  read_state();
  return env_.gpu_present && state_ == "asleep";
}

std::optional<PollRequest> PollEngine::begin(bool wake) {
  if (in_flight_) return std::nullopt;
  in_flight_ = true;
  return PollRequest{wake, wake || !apps_at_ || env_.clock() - *apps_at_ >= kAppsEvery};
}

std::optional<PollRequest> PollEngine::tick(const PollContext &ctx) {
  if (!ctx.window_visible || !ctx.page_active) return std::nullopt;  // hidden: not even a sysfs read
  read_state();
  PollInput in;
  in.window_visible = in.page_active = true;
  in.state = asleep() ? "asleep" : state_;  // no NVIDIA device: never query
  in.in_flight = in_flight_;
  in.interval_s = ctx.interval_s;
  if (last_query_) in.since_last_s = env_.clock() - *last_query_;
  const std::string decision = poll_decision(in);
  publish_state();  // a listener may call query_now(); begin() re-checks in_flight_
  return decision == "query" ? begin(false) : std::nullopt;
}

std::optional<PollRequest> PollEngine::query_now(bool wake) {
  if (!wake) {
    read_state();
    if (asleep()) return std::nullopt;  // a plain refresh never wakes the GPU
  }
  return begin(wake);
}

PollResult PollEngine::read(const PollRequest &req) const {
  PollResult r;
  r.request = req;
  try {
    r.gpu = query_gpu(env_.runner);
    if (!r.gpu.value) r.gpu.error = "nvidia-smi failed: " + r.gpu.error;
    else if (req.want_apps) r.apps = query_compute_apps(env_.runner);
  } catch (const std::exception &e) {
    r.gpu = {std::nullopt, std::string("Could not read the GPU: ") + e.what()};
  }
  return r;
}

std::optional<std::string> PollEngine::read_hybrid() const {
  try {
    return supergfxctl_mode(env_.runner);
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

void PollEngine::finish(const PollResult &res) {
  in_flight_ = false;
  last_query_ = env_.clock();  // a failure counts too: the retry waits one interval, no hot loop
  read_state();
  if (!res.gpu.value) {
    error_ = res.gpu.error;
    publish_(event(PollEvent::Kind::Stale));
    return;
  }
  error_.clear();
  sample_ = res.gpu.value;
  if (res.apps && res.apps->value) {  // a failed process query keeps the old list
    apps_ = res.apps->value;
    apps_at_ = last_query_;
  }
  publish_(event(PollEvent::Kind::Sample));
}
