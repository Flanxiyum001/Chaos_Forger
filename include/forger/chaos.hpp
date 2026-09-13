#pragma once
// ============================================================================
//  forger/chaos.hpp — probability decision core + blast-radius budget
//
//  PROBABILITY SEMANTICS (the documented contract):
//
//  chaos_probability is evaluated ONCE PER SCHEDULER CYCLE — not per target
//  rule and not per container. A cycle either fires chaos (armed containers
//  are struck, subject to budgets and per-strike errors) or it does not.
//  This is the simplest predictable behavior: the dice decide "does chaos
//  happen this tick?", the target rules decide "to whom?", the budgets
//  decide "how many at most?".
//
//    0.0  -> never injects chaos (every roll fails)
//    1.0  -> always attempts chaos (every roll succeeds)
//    0.6  -> approximately 60% of scheduler cycles trigger chaos
//            (law of large numbers; individual cycles are independent)
//
//  Implementation: std::mt19937_64 seeded from std::random_device (or an
//  explicit seed for reproducible tests) + std::uniform_real_distribution.
//  No rand(), ever. Values outside [0.0, 1.0] are clamped so a bad config can
//  never invert the gate's meaning.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "forger/discovery.hpp"  // MatchedContainer

namespace forger {

// ---------------------------------------------------------------------------
// Blast-radius budget: cap strikes per cycle and per process lifetime.
// Pure function so the selection policy is unit-testable.
//
// Policy (documented in README "Safety model"):
//   - If the run budget is exhausted, no strikes are selected.
//   - Otherwise, at most `max_per_cycle` armed containers are selected, in
//     match order, and never more than the remaining run budget.
//   - max_per_run <= 0 means unlimited (only the per-cycle cap applies).
//   - max_per_cycle < 1 is clamped to 1 (a zero cap must not silently
//     disable strikes; use dry_run for that).
// ---------------------------------------------------------------------------
std::vector<MatchedContainer> action_budget(const std::vector<MatchedContainer>& armed,
                                            int max_per_cycle,
                                            long max_per_run,
                                            long already_used);

class ChaosRoller {
public:
    // Seed from std::random_device (production path).
    ChaosRoller();

    // Explicit seed (tests: reproducible sequences).
    explicit ChaosRoller(unsigned long long seed);

    // Clamp to [0.0, 1.0]. A probability of exactly 0.0 never fires and
    // exactly 1.0 always fires (see roll()).
    void set_probability(double p);
    double probability() const { return probability_; }

    // One gate evaluation. Returns true when chaos fires this cycle.
    // The roll value (for logging) is stored in `rolled` when non-null.
    bool roll(double* rolled = nullptr);

    // Monotonic counter of rolls taken (observability / shutdown summary).
    unsigned long long rolls() const { return rolls_; }

private:
    double probability_ = 0.0;
    unsigned long long rolls_ = 0;
    std::mt19937_64 rng_;
};

}  // namespace forger
