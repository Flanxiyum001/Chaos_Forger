#pragma once
// ============================================================================
//  forger/engine.hpp — the chaos engine, as a library component
//
//  One scheduler tick = discover → match → roll → strike → log.
//
//  Dependencies are interfaces so the whole tick pipeline is unit-testable
//  with a fake Docker client and an in-test shutdown flag: no daemon, no
//  socket, no signals.
//
//  Probability contract (per scheduler cycle, not per container):
//    0.0 -> never, 1.0 -> always, 0.6 -> ~60% of cycles fire.
//
//  Shutdown contract: once shutdown is requested, no new chaos starts —
//  the tick is skipped before discovery, armed targets are released before
//  the roll, and the strike sweep stops before each strike. A strike that is
//  already in flight completes (bounded by the transport timeout).
// ============================================================================

#include <optional>
#include <vector>

#include "forger/chaos.hpp"      // ChaosRoller, action_budget
#include "forger/config.hpp"     // Config
#include "forger/discovery.hpp"  // MatchedContainer, match_containers
#include "forger/docker_api.hpp" // IDockerClient
#include "forger/shutdown.hpp"

namespace forger {

// ---------------------------------------------------------------------------
// The engine. Holds no global state: construction takes everything it needs.
// ---------------------------------------------------------------------------
class ChaosEngine {
public:
    struct Counters {
        unsigned long long rolls = 0;
        unsigned long long strikes = 0;
        unsigned long long errors = 0;  // discovery + strike failures
    };

    struct Deps {
        IDockerClient* docker = nullptr;    // not owned
        ShutdownState* shutdown = nullptr;  // not owned, must outlive the engine
        bool discover_only = false;         // --discover: match but never roll/strike
    };

    // `dry_run_override` — nullopt: use cfg.dry_run; otherwise force
    // (true for --dry-run; tests use it to flip modes per scenario).
    ChaosEngine(Deps deps, Config cfg, std::optional<bool> dry_run_override = std::nullopt);

    // One scheduler tick. Returns false if discovery failed (the engine stays
    // usable; the caller decides whether to keep ticking).
    bool tick();

    // Observability for tests and the shutdown summary.
    const Counters& counters() const { return counters_; }
    unsigned long long rolls() const { return counters_.rolls; }
    unsigned long long strikes() const { return counters_.strikes; }
    unsigned long long errors() const { return counters_.errors; }
    const Config& config() const { return cfg_; }
    bool dry_run() const { return dry_run_; }
    bool discover_only() const { return deps_.discover_only; }

    // Deterministic gate override for tests: force the next should_strike()
    // outcome (the roll is still counted). Production never calls this; tests
    // use it to pin dry-run/max-actions behavior without relying on random
    // values.
    void force_next_roll(bool fires);

private:
    bool should_strike();
    void execute_strikes(const std::vector<MatchedContainer>& armed);
    void log_quiet_tick(std::size_t running_count) const;

    Deps deps_;
    Config cfg_;
    std::optional<bool> dry_run_override_;
    bool dry_run_;
    ChaosRoller roller_;
    std::optional<bool> forced_roll_;  // tests only
    Counters counters_;
    long run_budget_used_ = 0;
};

}  // namespace forger
