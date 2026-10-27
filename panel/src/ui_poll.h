// GPU polling for the pages. Contract (the Overview, SR, Power and About pages are written against it):
//   app.add_poll_listener([](const PollEvent &e) { ... });  once, in the page factory; runs on the GTK main loop.
//   Events (poll_engine.h): Asleep (no GPU or runtime-suspended: nothing is queried), Awake (state refresh, about
//     1/s while the Overview shows), Sample (fresh nvidia-smi reading), Stale (reading failed, e.error says why).
//   Every event is a full snapshot: sample/apps (newest known), hybrid (read once), power_source, runtime, state,
//     stale. Nothing is published while the window is hidden or another page shows: app.poll_last() is the latest.
//   app.query_now(true) is "Wake and read": queries once even when asleep. query_now(false) skips an asleep GPU.
//   nvidia-smi runs only while the window is visible AND the Overview shows AND the GPU is not suspended, once per
//     Settings interval (0 = manual). Pages never call nvidia-smi for state: use these events or gpu_sysfs.h.
//   F5 on the Overview is handled here (App::refresh_current): queries, or toasts when asleep; then your hook runs.
#pragma once

#include <glib.h>

#include <functional>
#include <memory>

#include "poll_engine.h"

// The GLib half: the 1 s timer, the worker (GTask) that runs nvidia-smi off the main loop, the listener list.
class UiPoll {
 public:
  using Listener = std::function<void(const PollEvent &)>;
  using Probe = std::function<PollContext()>;  // visibility, page and interval, asked at each tick

  explicit UiPoll(Probe probe);  // finds the NVIDIA GPU in sysfs; no subprocess
  ~UiPoll();
  UiPoll(const UiPoll &) = delete;
  UiPoll &operator=(const UiPoll &) = delete;

  void add_listener(Listener fn);
  const PollEvent &last() const;
  void start();                  // publishes the state, reads the hybrid mode once, starts the 1 s timer
  void stop();                   // window closing: no more ticks, late results are dropped
  void tick_now();               // page switch / window mapped
  void query_now(bool wake);
  bool refresh();                // F5 on the Overview; false: the GPU is asleep and nothing was queried

 private:
  struct Core;
  void start_query(const PollRequest &req);

  std::shared_ptr<Core> core_;  // shared with in-flight workers, which may outlive us at shutdown
  guint timer_ = 0;
};
