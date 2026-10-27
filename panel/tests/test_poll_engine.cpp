// PollEngine: the battery rule (when nvidia-smi may run) with a fake clock, fake sysfs state and a counting runner.
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "check.h"
#include "poll_engine.h"

namespace {

const char *const kGpuCsv =
    "NVIDIA GeForce RTX 3070 Ti Laptop GPU, 615.78.08, 1312, 8192, 12, 47, 18.52, 125.00, 1230, 6000, 4, 16, "
    "[N/A], P0, Disabled\n";

struct Rig {
  double now = 100;
  std::string runtime = "active";  // runtime_status the fake sysfs reports
  bool gpu_present = true;
  bool fail = false;               // nvidia-smi exits 1
  bool throws = false;             // the runner itself throws
  int gpu_calls = 0, app_calls = 0, hybrid_calls = 0, sysfs_reads = 0;
  std::vector<PollEvent> events;
  PollEngine engine;

  Rig() : engine(env(), [this](const PollEvent &e) { events.push_back(e); }) {}

  PollEngine::Env env() {
    PollEngine::Env e;
    e.gpu_present = gpu_present;
    e.clock = [this] { return now; };
    e.runtime = [this] {
      ++sysfs_reads;
      RuntimeInfo r;
      r.status = runtime;
      return r;
    };
    e.power_source = [] { return std::string("ac"); };
    e.runner = [this](const std::vector<std::string> &argv, double, const std::vector<std::string> &) {
      if (throws) throw std::runtime_error("boom");
      CmdResult r;
      r.argv = argv;
      const std::string &prog = argv.at(0);
      if (prog == "supergfxctl") {
        ++hybrid_calls;
        r.rc = 0;
        r.out = "Hybrid\n";
      } else if (argv.size() > 1 && argv[1].rfind("--query-compute-apps", 0) == 0) {
        ++app_calls;
        r.rc = 0;
        r.out = "4242, /usr/bin/mpv, 512\n";
      } else {
        ++gpu_calls;
        r.rc = fail ? 1 : 0;
        r.out = fail ? "" : kGpuCsv;
        r.err = fail ? "Failed to initialize NVML" : "";
      }
      return r;
    };
    return e;
  }

  // A tick plus, when it asks for a query, the worker and the main-thread completion (what UiPoll does with GTask).
  bool step(bool visible = true, bool overview = true, std::optional<double> interval = 2.0) {
    PollContext ctx;
    ctx.window_visible = visible;
    ctx.page_active = overview;
    ctx.interval_s = interval;
    const auto req = engine.tick(ctx);
    if (!req) return false;
    engine.finish(engine.read(*req));
    return true;
  }
};

int count_kind(const std::vector<PollEvent> &ev, PollEvent::Kind k) {
  int n = 0;
  for (const PollEvent &e : ev) n += e.kind == k;
  return n;
}

}  // namespace

int main() {
  using Kind = PollEvent::Kind;

  {  // asleep: zero runner calls, however long we tick; the event says Asleep and stale
    Rig r;
    r.runtime = "suspended";
    for (int i = 0; i < 20; ++i, r.now += 1) r.step();
    CHECK(r.gpu_calls == 0 && r.app_calls == 0, "suspended GPU: 0 nvidia-smi calls in 20 s (got %d)", r.gpu_calls);
    CHECK(count_kind(r.events, Kind::Asleep) == 20, "suspended GPU: one Asleep event per tick");
    CHECK(r.events.back().stale && r.events.back().state == "asleep", "Asleep event is stale and says asleep");
    CHECK(!r.engine.query_now(false), "query_now(false) refuses an asleep GPU");
    CHECK(!r.engine.in_flight() && r.gpu_calls == 0, "the refused request leaves no query in flight");
    r.runtime = "suspending";
    r.step();
    CHECK(r.gpu_calls == 0, "suspending counts as asleep");
    r.runtime = "resuming";
    CHECK(r.step() && r.gpu_calls == 1, "resuming counts as awake: one query");
  }

  {  // awake: one query per interval, not more
    Rig r;
    for (int i = 0; i < 10; ++i, r.now += 1) r.step();  // t = 0..9 s, interval 2 s
    CHECK(r.gpu_calls == 5, "awake, 2 s interval, 10 s of 1 s ticks: 5 queries (got %d)", r.gpu_calls);
    CHECK(count_kind(r.events, Kind::Sample) == 5, "each query publishes one Sample");
    Rig q;
    for (int i = 0; i < 3; ++i) q.step();  // same instant
    CHECK(q.gpu_calls == 1, "three ticks at the same instant: 1 query (got %d)", q.gpu_calls);
    q.now += 0.5;
    CHECK(!q.step() && q.gpu_calls == 1, "0.5 s later: still waiting");
    q.now += 1.4;  // 1.9 s: past 0.9 x interval (the 1 s timer tolerates slightly early ticks)
    CHECK(q.step() && q.gpu_calls == 2, "1.9 s later: the next query");
    const PollEvent &s = q.events.back();
    CHECK(s.kind == Kind::Sample && s.sample && s.sample->name == "NVIDIA GeForce RTX 3070 Ti Laptop GPU" &&
              !s.stale && s.error.empty() && s.power_source == "ac" && s.gpu_present,
          "Sample event carries the reading, power source, no error");
    CHECK(s.runtime.status == "active", "Sample event carries the runtime status");
  }

  {  // process list: first query, then every 6 s
    Rig r;
    for (int i = 0; i < 13; ++i, r.now += 2) r.step();  // t = 0, 2, ... 24 s: queries on every tick
    CHECK(r.gpu_calls == 13, "13 ticks 2 s apart: 13 GPU queries (got %d)", r.gpu_calls);
    CHECK(r.app_calls == 5, "process list at t = 0, 6, 12, 18, 24 (got %d)", r.app_calls);
    CHECK(r.events.back().apps && r.events.back().apps->size() == 1 && r.events.back().apps->at(0).pid == 4242,
          "events carry the process list");
  }

  {  // hidden / minimised / another page: nothing, not even a sysfs read or an event
    Rig r;
    CHECK(!r.step(false, true) && !r.step(true, false) && !r.step(false, false), "no request while hidden");
    r.now += 60;
    CHECK(!r.step(false, true), "still no request a minute later");
    CHECK(r.gpu_calls == 0 && r.sysfs_reads == 0 && r.events.empty(),
          "hidden: 0 nvidia-smi calls, 0 sysfs reads, 0 events (calls %d, reads %d)", r.gpu_calls, r.sysfs_reads);
    CHECK(r.step(true, true) && r.gpu_calls == 1, "visible on the Overview again: queries at once");
  }

  {  // "Wake and read": exactly one query on a suspended GPU, process list included; no second one in flight
    Rig r;
    r.runtime = "suspended";
    const auto req = r.engine.query_now(true);
    CHECK(req && req->wake && req->want_apps, "wake request asks for GPU and process list");
    CHECK(r.engine.in_flight() && !r.engine.query_now(true) && !r.engine.query_now(false),
          "a second request while one is in flight is refused");
    PollContext ctx;
    ctx.window_visible = ctx.page_active = true;
    ctx.interval_s = 2;
    CHECK(!r.engine.tick(ctx), "tick during the wake query adds none (still asleep)");
    r.runtime = "active";  // the query woke the GPU
    r.now += 0.3;
    r.engine.finish(r.engine.read(*req));
    CHECK(r.gpu_calls == 1 && r.app_calls == 1, "wake query: 1 GPU call, 1 process call (%d, %d)", r.gpu_calls, r.app_calls);
    CHECK(r.events.back().kind == Kind::Sample && r.events.back().state == "awake", "result is a Sample, now awake");
    r.now += 0.5;
    CHECK(!r.step() && r.gpu_calls == 1, "no extra query right after the wake query (interval not elapsed)");
    r.now += 1.6;
    CHECK(r.step() && r.gpu_calls == 2, "polling resumes one interval later");
    // wake = false is fine while awake
    CHECK(r.engine.query_now(false).has_value(), "query_now(false) works on an awake GPU");
  }

  {  // error -> stale, no hot loop, recovery clears it
    Rig r;
    CHECK(r.step() && r.gpu_calls == 1, "first query ok");
    r.now += 2;
    r.fail = true;
    CHECK(r.step(), "second query runs");
    CHECK(r.events.back().kind == Kind::Stale && r.events.back().stale, "failed query publishes Stale");
    CHECK(r.events.back().error == "nvidia-smi failed: Failed to initialize NVML", "error text: '%s'",
          r.events.back().error.c_str());
    CHECK(r.events.back().sample && r.events.back().sample->temp_c == 47.0, "the old reading is kept");
    CHECK(!r.engine.in_flight(), "a failed query clears in-flight");
    r.now += 0.5;
    CHECK(!r.step() && r.gpu_calls == 2, "failure does not retry at once (no loop)");
    CHECK(r.events.back().kind == Kind::Awake && r.events.back().stale && !r.events.back().error.empty(),
          "the next state tick stays stale with the error");
    r.now += 1.5;
    r.fail = false;
    CHECK(r.step() && r.gpu_calls == 3, "retry one interval after the failure");
    CHECK(r.events.back().kind == Kind::Sample && !r.events.back().stale && r.events.back().error.empty(),
          "recovery clears stale and the error");
    r.now += 2;
    r.throws = true;
    const bool asked = r.step();  // before the CHECK: its arguments must not point into the events vector
    CHECK(asked && r.events.back().kind == Kind::Stale && r.events.back().error == "Could not read the GPU: boom",
          "an exception in the runner becomes a Stale event (got '%s')", r.events.back().error.c_str());
  }

  {  // interval off: manual only
    Rig r;
    for (int i = 0; i < 30; ++i, r.now += 1) r.step(true, true, std::nullopt);
    for (int i = 0; i < 5; ++i, r.now += 1) r.step(true, true, 0.0);
    CHECK(r.gpu_calls == 0, "interval off: 0 queries in 35 s (got %d)", r.gpu_calls);
    CHECK(count_kind(r.events, Kind::Awake) == 35, "interval off still publishes the state");
    const auto req = r.engine.query_now(true);
    CHECK(req.has_value(), "manual request is served");
    r.engine.finish(r.engine.read(*req));
    CHECK(r.gpu_calls == 1, "manual: exactly one query");
    r.now += 100;
    CHECK(!r.step(true, true, std::nullopt) && r.gpu_calls == 1, "and no more afterwards");
  }

  {  // one query in flight at a time
    Rig r;
    PollContext ctx;
    ctx.window_visible = ctx.page_active = true;
    ctx.interval_s = 2;
    const auto first = r.engine.tick(ctx);
    CHECK(first.has_value() && r.engine.in_flight(), "first tick starts a query");
    r.now += 30;
    CHECK(!r.engine.tick(ctx) && !r.engine.query_now(true) && !r.engine.query_now(false),
          "30 s later, still in flight: no second query from tick or query_now");
    r.engine.finish(r.engine.read(*first));
    CHECK(r.gpu_calls == 1 && !r.engine.in_flight(), "one nvidia-smi run, then free again");
    r.now += 2;
    CHECK(r.engine.tick(ctx).has_value(), "after finish the next interval queries");
  }

  {  // a listener that asks for a query during the state event does not double it
    Rig r;
    std::unique_ptr<PollEngine> eng;
    int from_listener = 0;
    eng = std::make_unique<PollEngine>(r.env(), [&](const PollEvent &e) {
      if (e.kind == Kind::Awake && eng && eng->query_now(true)) ++from_listener;
    });
    PollContext ctx;
    ctx.window_visible = ctx.page_active = true;
    ctx.interval_s = 2;
    CHECK(!eng->tick(ctx), "tick hands out no second query when a listener already started one");
    CHECK(from_listener == 1 && eng->in_flight(), "the listener's query is the one in flight");
  }

  {  // no NVIDIA device: never queries, says so
    Rig r;
    r.gpu_present = false;
    PollEngine eng(r.env(), [&](const PollEvent &e) { r.events.push_back(e); });
    PollContext ctx;
    ctx.window_visible = ctx.page_active = true;
    ctx.interval_s = 2;
    CHECK(!eng.tick(ctx), "no GPU: tick queries nothing");
    CHECK(!r.events.empty() && r.events.back().kind == Kind::Asleep && !r.events.back().gpu_present &&
              r.events.back().stale,
          "no GPU: Asleep event with gpu_present = false");
    CHECK(r.gpu_calls == 0 && !eng.asleep_now(), "no GPU: 0 calls; F5 may still try (asleep_now is false)");
  }

  {  // hybrid mode: read once by the glue, published to the pages
    Rig r;
    r.engine.announce();
    CHECK(!r.events.back().hybrid, "hybrid unknown before it is read");
    const auto mode = r.engine.read_hybrid();
    CHECK(mode == std::optional<std::string>("Hybrid") && r.hybrid_calls == 1, "read_hybrid runs supergfxctl -g once");
    r.engine.set_hybrid(mode);
    CHECK(r.events.back().hybrid == mode && r.events.back().kind == Kind::Awake, "set_hybrid publishes a state event");
    for (int i = 0; i < 10; ++i, r.now += 1) r.step();
    CHECK(r.hybrid_calls == 1 && r.events.back().hybrid == mode, "polling never re-reads it; events keep it");
    r.engine.set_hybrid(std::nullopt);
    CHECK(!r.events.back().hybrid, "a failed read stays unknown");
  }

  return finish("test_poll_engine");
}
