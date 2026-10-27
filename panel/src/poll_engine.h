// The decisions of the GPU polling engine, without GTK or GLib: when nvidia-smi may run and what the pages are told.
// Time, sysfs and the subprocess runner are injected, so tests drive it with a fake clock and a counting runner.
//
// Battery rule: on a hybrid laptop nvidia-smi wakes the sleeping dGPU. tick() therefore asks for a query only while
// the window is visible, the Overview is showing and the sysfs runtime status is not suspended (gpu_sysfs
// poll_decision), at most once per interval, never while one is in flight. query_now(true) ("Wake and read") is the
// one explicit exception. Threads: everything runs on the main thread except read() and read_hybrid(), which block
// on the subprocess and touch no engine state.
#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "gpu_sysfs.h"
#include "smi_csv.h"

// Every event is a full snapshot of what the engine knows; `kind` says what just happened.
struct PollEvent {
  enum class Kind {
    Asleep,  // no NVIDIA GPU, or it is runtime-suspended: nothing is queried, `sample` is the old reading
    Awake,   // state refresh (sysfs only, about once a second while the Overview shows)
    Sample,  // a fresh nvidia-smi reading
    Stale,   // the reading failed: `error` says why, `sample` is the old reading
  };
  Kind kind = Kind::Awake;
  bool gpu_present = false;           // false: no NVIDIA device in sysfs (kind is Asleep)
  std::string state = "unknown";      // gpu_state(runtime.status): asleep | awake | unknown
  bool stale = false;                 // `sample` is not current: Asleep, Stale, or Awake after a failed read
  std::optional<GpuSample> sample;    // newest good reading, kept over Asleep/Stale
  std::optional<std::vector<ComputeApp>> apps;  // newest process list (refreshed every ~6 s), nullopt: never read
  std::optional<std::string> hybrid;  // supergfxctl mode, read once at start; nullopt: unknown (yet)
  std::string power_source = "unknown";  // ac | battery | unknown
  RuntimeInfo runtime;
  std::string error;                  // text of the last failed read, empty after a good one
};

struct PollContext {  // what the GLib side knows at each tick
  bool window_visible = false;
  bool page_active = false;           // the Overview is the visible page
  std::optional<double> interval_s;   // nullopt or <= 0: manual (query_now only)
};

struct PollRequest {  // handed from tick()/query_now() to the worker
  bool wake = false;
  bool want_apps = false;
};

struct PollResult {   // handed from the worker back to finish()
  PollRequest request;
  Query<GpuSample> gpu;
  std::optional<Query<std::vector<ComputeApp>>> apps;
};

class PollEngine {
 public:
  struct Env {
    bool gpu_present = false;
    std::function<RuntimeInfo()> runtime = [] { return RuntimeInfo{}; };  // cheap sysfs read
    std::function<std::string()> power_source = [] { return ::power_source(); };
    Runner runner = run;
    std::function<double()> clock = [] {  // monotonic seconds
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
  };
  using Publish = std::function<void(const PollEvent &)>;
  static constexpr double kAppsEvery = 6.0;  // seconds between process-list queries while polling

  PollEngine(Env env, Publish publish);

  // Reads sysfs and publishes the current state (Asleep or Awake). No subprocess.
  void announce();
  // The hybrid mode arrived (or failed: nullopt); publishes a state event.
  void set_hybrid(std::optional<std::string> mode);
  // The 1 s timer and every page switch. Publishes a state event when the window is visible and the Overview is
  // showing, otherwise does nothing at all (no sysfs read). Returns the query to start, if one is due.
  std::optional<PollRequest> tick(const PollContext &ctx);
  // An explicit request. wake = true: "Wake and read" / F5, queries even when the GPU is asleep and always refreshes
  // the process list. wake = false: skipped while the GPU is asleep. nullopt: nothing to do (asleep, or in flight).
  std::optional<PollRequest> query_now(bool wake);
  // True when the sysfs runtime status says the GPU is suspended (re-read now).
  bool asleep_now();
  // Worker thread: runs nvidia-smi for a request. Never throws.
  PollResult read(const PollRequest &req) const;
  std::optional<std::string> read_hybrid() const;
  // Main thread, once per request handed out: clears in-flight and publishes Sample or Stale.
  void finish(const PollResult &res);

  bool in_flight() const { return in_flight_; }

 private:
  bool asleep() const { return !env_.gpu_present || state_ == "asleep"; }
  void read_state();
  PollEvent event(PollEvent::Kind kind) const;
  void publish_state();
  std::optional<PollRequest> begin(bool wake);

  Env env_;
  Publish publish_;
  RuntimeInfo runtime_;
  std::string state_ = "unknown", power_ = "unknown", error_;
  std::optional<GpuSample> sample_;
  std::optional<std::vector<ComputeApp>> apps_;
  std::optional<std::string> hybrid_;
  bool in_flight_ = false;
  std::optional<double> last_query_, apps_at_;
};
