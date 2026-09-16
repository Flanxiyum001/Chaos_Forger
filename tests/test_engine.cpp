// ============================================================================
//  tests/test_engine.cpp — chaos engine behavior with a fake Docker client
//
//  Requires no Docker daemon, no socket, no threads: ChaosEngine depends on
//  IDockerClient, and this file injects FakeDockerClient, which scripts the
//  container list and records every strike.
//
//  Pins the engine contract:
//    - dry_run: discovery/matching/rolling happen, stop/kill never called
//    - max_actions_per_cycle: at most N strikes per tick, first-match order
//    - max_actions_per_run: process-lifetime budget, exhaustion is safe
//    - strike dispatch: stop vs kill per rule, full 64-hex ID used
//    - fault isolation: one failing strike does not abort the sweep
//    - shutdown guards: no discovery, no roll, no strike after stop_requested
// ============================================================================

#include "Chaos_Forger/engine.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!((a) == (b))) {                                                 \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                    \
    } while (0)

using namespace Chaos_Forger;

// ----------------------------------------------------------------------------
// FakeDockerClient: scripts discovery, records strikes, optional failures.
// ----------------------------------------------------------------------------
namespace {

struct Strike {
    std::string id;
    Action action;
    int timeout = 0;  // stop only
    int http_status = 0;
    bool ok = false;
};

class FakeDockerClient final : public IDockerClient {
public:
    std::vector<Container> containers;
    bool fail_discovery = false;
    std::vector<Strike> strikes;

    // Test hooks: run inside the calls, so tests can flip shutdown state at
    // exact points of the engine pipeline (mid-discovery, mid-sweep).
    std::function<void()> on_list;
    std::function<void()> after_stop;

    bool list_containers(std::vector<Container>& out, std::string& err) override {
        if (on_list) on_list();
        if (fail_discovery) {
            err = "simulated engine unreachable";
            return false;
        }
        out = containers;
        return true;
    }

    bool stop_container(const std::string& id, int timeout_seconds, std::string& err,
                        int* http_status) override {
        Strike s;
        s.id = id;
        s.action = Action::Stop;
        s.timeout = timeout_seconds;
        if (stop_status_ != 0) {
            if (http_status != nullptr) *http_status = stop_status_;
            err = "stop: HTTP " + std::to_string(stop_status_);
            s.http_status = stop_status_;
            strikes.push_back(std::move(s));
            return false;
        }
        if (http_status != nullptr) *http_status = 204;
        s.http_status = 204;
        s.ok = true;
        strikes.push_back(std::move(s));
        if (after_stop) after_stop();
        return true;
    }

    bool kill_container(const std::string& id, std::string& err, int* http_status) override {
        Strike s;
        s.id = id;
        s.action = Action::Kill;
        if (kill_status_ != 0) {
            if (http_status != nullptr) *http_status = kill_status_;
            err = "kill: HTTP " + std::to_string(kill_status_);
            s.http_status = kill_status_;
            strikes.push_back(std::move(s));
            return false;
        }
        if (http_status != nullptr) *http_status = 204;
        s.http_status = 204;
        s.ok = true;
        strikes.push_back(std::move(s));
        return true;
    }

    bool ping(std::string&) override { return true; }

    void set_stop_status(int status) { stop_status_ = status; }
    void set_kill_status(int status) { kill_status_ = status; }

private:
    int stop_status_ = 0;
    int kill_status_ = 0;
};

Container mk(const std::string& name) {
    Container c;
    c.id = std::string(64, name[0]);
    c.names = {name};
    c.image = name + ":latest";
    c.state = "running";
    c.status = "Up 2 minutes";
    return c;
}

Config base_config() {
    Config cfg;
    cfg.chaos_probability = 1.0;  // dice always fire unless a test forces otherwise
    cfg.dry_run = false;
    cfg.max_actions_per_cycle = 10;
    cfg.max_actions_per_run = 0;  // unlimited
    cfg.targets = {{"web", Action::Stop}, {"db", Action::Kill}};
    return cfg;
}

}  // namespace

// ----------------------------------------------------------------------------
// Live mode, probability 1.0: every matching container is struck exactly once,
// with the configured action and the full container ID.
// ----------------------------------------------------------------------------
static void test_live_strikes_dispatch() {
    FakeDockerClient docker;
    docker.containers = {mk("web-1"), mk("db-1"), mk("unrelated")};

    ShutdownState shutdown;
    ChaosEngine::Deps deps;
    deps.docker = &docker;
    deps.shutdown = &shutdown;

    Config cfg = base_config();
    ChaosEngine engine(deps, cfg);
    CHECK(engine.tick());

    CHECK_EQ(docker.strikes.size(), 2u);
    CHECK_EQ(docker.strikes[0].action, Action::Stop);
    CHECK_EQ(docker.strikes[0].id, std::string(64, 'w'));
    CHECK_EQ(docker.strikes[0].timeout, cfg.stop_timeout_seconds);
    CHECK_EQ(docker.strikes[1].action, Action::Kill);
    CHECK_EQ(docker.strikes[1].id, std::string(64, 'd'));

    CHECK_EQ(engine.rolls(), 1u);
    CHECK_EQ(engine.strikes(), 2u);
    CHECK_EQ(engine.errors(), 0u);
}

// ----------------------------------------------------------------------------
// Requirement 11 — dry-run: the full pipeline runs, but no strike exists.
// ----------------------------------------------------------------------------
static void test_dry_run_never_strikes() {
    FakeDockerClient docker;
    docker.containers = {mk("web-1"), mk("db-1")};

    ShutdownState shutdown;
    ChaosEngine::Deps deps;
    deps.docker = &docker;
    deps.shutdown = &shutdown;

    Config cfg = base_config();
    cfg.dry_run = true;
    ChaosEngine engine(deps, cfg);

    CHECK(engine.tick());
    CHECK(docker.strikes.empty());           // nothing on the wire
    CHECK_EQ(engine.strikes(), 0u);          // nothing counted
    CHECK_EQ(engine.rolls(), 1u);            // the dice still ran
    CHECK(engine.dry_run());
}

// ----------------------------------------------------------------------------
// Requirement 12 — max_actions_per_cycle: at most N strikes per tick, in
// first-match order; the rest are held back and the next tick continues.
// ----------------------------------------------------------------------------
static void test_max_actions_per_cycle() {
    FakeDockerClient docker;
    docker.containers = {mk("web-1"), mk("web-2"), mk("db-1")};

    ShutdownState shutdown;
    ChaosEngine::Deps deps;
    deps.docker = &docker;
    deps.shutdown = &shutdown;

    Config cfg = base_config();
    cfg.max_actions_per_cycle = 2;
    ChaosEngine engine(deps, cfg);

    CHECK(engine.tick());
    CHECK_EQ(docker.strikes.size(), 2u);           // cap applied
    CHECK_EQ(docker.strikes[0].id, std::string(64, 'w'));  // first-match order
    CHECK_EQ(docker.strikes[1].id, std::string(64, 'w'));  // web-2
    CHECK_EQ(engine.strikes(), 2u);

    // Next cycle: the struck containers have left the running set (real
    // Docker drops stopped containers from /containers/json), the per-cycle
    // budget resets, and the remaining target is struck.
    docker.containers = {mk("db-1")};
    CHECK(engine.tick());
    CHECK_EQ(docker.strikes.size(), 3u);
    CHECK_EQ(docker.strikes[2].id, std::string(64, 'd'));
}

// ----------------------------------------------------------------------------
// max_actions_per_run: the lifetime budget binds across cycles and, once
// exhausted, no further strikes occur (restart required — documented).
// ----------------------------------------------------------------------------
static void test_max_actions_per_run() {
    FakeDockerClient docker;
    docker.containers = {mk("web-1"), mk("web-2"), mk("web-3")};

    ShutdownState shutdown;
    ChaosEngine::Deps deps;
    deps.docker = &docker;
    deps.shutdown = &shutdown;

    Config cfg = base_config();
    cfg.max_actions_per_run = 2;
    ChaosEngine engine(deps, cfg);

    CHECK(engine.tick());
    CHECK_EQ(docker.strikes.size(), 2u);  // lifetime budget hit mid-sweep

    // Budget exhausted: dice may still fire, but nothing is selected.
    CHECK(engine.tick());
    CHECK_EQ(docker.strikes.size(), 2u);
    CHECK_EQ(engine.strikes(), 2u);
}

// ----------------------------------------------------------------------------
// Fault isolation: a 500 on the first strike is logged and counted, and the
// sweep continues with the remaining target.
// ----------------------------------------------------------------------------
static void test_fault_isolation() {
    FakeDockerClient docker;
    docker.containers = {mk("web-1"), mk("db-1")};
    docker.set_stop_status(500);

    ShutdownState shutdown;
    ChaosEngine::Deps deps;
    deps.docker = &docker;
    deps.shutdown = &shutdown;

    ChaosEngine engine(deps, base_config());
    CHECK(engine.tick());

    CHECK_EQ(docker.strikes.size(), 2u);      // both attempts made
    CHECK(!docker.strikes[0].ok);             // stop failed (500)
    CHECK(docker.strikes[1].ok);              // kill still succeeded
    CHECK_EQ(engine.errors(), 1u);
    CHECK_EQ(engine.strikes(), 1u);
}

// ----------------------------------------------------------------------------
// Discovery failure: no strike, error counted, engine stays usable.
// ----------------------------------------------------------------------------
static void test_discovery_failure_is_not_fatal() {
    FakeDockerClient docker;
    docker.fail_discovery = true;

    ShutdownState shutdown;
    ChaosEngine::Deps deps;
    deps.docker = &docker;
    deps.shutdown = &shutdown;

    ChaosEngine engine(deps, base_config());
    CHECK(!engine.tick());
    CHECK(docker.strikes.empty());
    CHECK_EQ(engine.errors(), 1u);

    // Recover: a later tick works.
    docker.fail_discovery = false;
    docker.containers = {mk("web-1")};
    CHECK(engine.tick());
    CHECK_EQ(engine.strikes(), 1u);
}

// ----------------------------------------------------------------------------
// Shutdown guards (the lifecycle contract from README):
//   #1 requested before the tick -> no discovery at all
//   #2 requested after discovery (forced via discover_only-style injection:
//      here we flip the flag from inside a scripted client) -> no roll
//   #3 requested mid-sweep -> remaining strikes cancelled
// ----------------------------------------------------------------------------
static void test_shutdown_guards() {
    {
        // Guard #1: stop before tick -> tick() discovers nothing.
        FakeDockerClient docker;
        docker.containers = {mk("web-1")};
        ShutdownState shutdown;
        ChaosEngine::Deps deps;
        deps.docker = &docker;
        deps.shutdown = &shutdown;
        ChaosEngine engine(deps, base_config());

        shutdown.request(StopReason::Signal);
        CHECK(engine.tick());
        CHECK(docker.strikes.empty());
        CHECK_EQ(engine.rolls(), 0u);  // no dice either
    }
    {
        // Guard #2: stop arrives during discovery -> no roll, no strike.
        FakeDockerClient docker;
        docker.containers = {mk("web-1")};
        ShutdownState shutdown;
        ChaosEngine::Deps deps;
        deps.docker = &docker;
        deps.shutdown = &shutdown;
        ChaosEngine engine(deps, base_config());

        docker.on_list = [&] { shutdown.request(StopReason::Signal); };
        CHECK(engine.tick());
        CHECK_EQ(engine.rolls(), 0u);
        CHECK(docker.strikes.empty());
    }
    {
        // Guard #3: stop arrives during the strike sweep -> later strikes
        // cancelled. Only the first target (whose strike is already "in
        // flight") completes.
        FakeDockerClient docker;
        docker.containers = {mk("web-1"), mk("web-2")};
        ShutdownState shutdown;
        ChaosEngine::Deps deps;
        deps.docker = &docker;
        deps.shutdown = &shutdown;
        ChaosEngine engine(deps, base_config());

        bool first_strike_done = false;
        docker.after_stop = [&] {
            if (!first_strike_done) {
                first_strike_done = true;
                shutdown.request(StopReason::Signal);
            }
        };
        CHECK(engine.tick());
        CHECK_EQ(docker.strikes.size(), 1u);  // second strike cancelled
        CHECK_EQ(engine.strikes(), 1u);
    }
}

// ----------------------------------------------------------------------------
// Probability contract end-to-end: p = 0.0 never strikes; p = 1.0 always does.
// ----------------------------------------------------------------------------
static void test_probability_end_to_end() {
    {
        FakeDockerClient docker;
        docker.containers = {mk("web-1")};
        ShutdownState shutdown;
        ChaosEngine::Deps deps;
        deps.docker = &docker;
        deps.shutdown = &shutdown;

        Config cfg = base_config();
        cfg.chaos_probability = 0.0;
        ChaosEngine engine(deps, cfg);

        for (int i = 0; i < 50; ++i) CHECK(engine.tick());
        CHECK(docker.strikes.empty());
        CHECK_EQ(engine.rolls(), 50u);
    }
    {
        FakeDockerClient docker;
        docker.containers = {mk("web-1")};
        ShutdownState shutdown;
        ChaosEngine::Deps deps;
        deps.docker = &docker;
        deps.shutdown = &shutdown;

        ChaosEngine engine(deps, base_config());  // p = 1.0
        for (int i = 0; i < 50; ++i) CHECK(engine.tick());
        CHECK_EQ(engine.strikes(), 50u);
    }
}

// ----------------------------------------------------------------------------
// discover_only: matches are logged, the dice never run.
// ----------------------------------------------------------------------------
static void test_discover_only_never_rolls() {
    FakeDockerClient docker;
    docker.containers = {mk("web-1")};

    ShutdownState shutdown;
    ChaosEngine::Deps deps;
    deps.docker = &docker;
    deps.shutdown = &shutdown;
    deps.discover_only = true;

    ChaosEngine engine(deps, base_config());
    CHECK(engine.tick());
    CHECK_EQ(engine.rolls(), 0u);
    CHECK(docker.strikes.empty());
}

// ----------------------------------------------------------------------------
int main() {
    test_live_strikes_dispatch();
    test_dry_run_never_strikes();
    test_max_actions_per_cycle();
    test_max_actions_per_run();
    test_fault_isolation();
    test_discovery_failure_is_not_fatal();
    test_shutdown_guards();
    test_probability_end_to_end();
    test_discover_only_never_rolls();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
