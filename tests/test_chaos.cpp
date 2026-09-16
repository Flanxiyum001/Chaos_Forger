// ============================================================================
//  tests/test_chaos.cpp — unit tests for the chaos probability core
//
//  Pins the probability contract:
//    - 0.0 never fires, 1.0 always fires
//    - 0.6 fires ~60% of cycles (statistical, fixed seed)
//    - out-of-range values are clamped (never invert the gate)
//    - seeded rollers are reproducible
//    - no rand() anywhere: std::mt19937_64 + uniform_real_distribution
// ============================================================================

#include "Chaos_Forger/chaos.hpp"
#include "Chaos_Forger/docker_api.hpp"

#include <cstdio>
#include <string>

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

using Chaos_Forger::ChaosRoller;
using Chaos_Forger::MatchedContainer;

static constexpr int kTrials = 10000;

// ----------------------------------------------------------------------------
// Blast-radius budget: per-cycle cap + per-run cap selection policy.
// ----------------------------------------------------------------------------
static void test_action_budget() {
    auto mk = [](char fill) {
        Chaos_Forger::Container c;
        c.id = std::string(64, fill);
        c.names = {std::string("app-") + fill};
        return MatchedContainer{c, 0};
    };
    const std::vector<MatchedContainer> armed = {mk('a'), mk('b'), mk('c'), mk('d'), mk('e')};

    // Per-cycle cap selects the first N in match order.
    {
        const auto sel = Chaos_Forger::action_budget(armed, 2, 0, 0);
        CHECK_EQ(sel.size(), 2u);
        CHECK_EQ(sel[0].container.id, armed[0].container.id);
        CHECK_EQ(sel[1].container.id, armed[1].container.id);
    }
    // Cap above the armed count -> everyone passes.
    {
        const auto sel = Chaos_Forger::action_budget(armed, 10, 0, 0);
        CHECK_EQ(sel.size(), 5u);
    }
    // Run budget nearly exhausted -> only the remainder passes.
    {
        const auto sel = Chaos_Forger::action_budget(armed, 10, 3, 1);  // 2 remaining
        CHECK_EQ(sel.size(), 2u);
    }
    // Run budget exhausted -> nothing is selected.
    {
        const auto sel = Chaos_Forger::action_budget(armed, 5, 4, 4);
        CHECK(sel.empty());
    }
    // Combined: per-cycle cap binds harder than the run budget.
    {
        const auto sel = Chaos_Forger::action_budget(armed, 1, 100, 50);
        CHECK_EQ(sel.size(), 1u);
    }
    // Invalid per-cycle values clamp to 1 (never silently disable strikes).
    {
        const auto sel = Chaos_Forger::action_budget(armed, 0, 0, 0);
        CHECK_EQ(sel.size(), 1u);
    }
    // Unlimited run budget (0) never binds.
    {
        const auto sel = Chaos_Forger::action_budget(armed, 5, 0, 999999);
        CHECK_EQ(sel.size(), 5u);
    }
}

// ----------------------------------------------------------------------------
// p = 0.0 -> never injects chaos.
// ----------------------------------------------------------------------------
static void test_zero_probability_never_fires() {
    ChaosRoller r(42);
    r.set_probability(0.0);
    for (int i = 0; i < kTrials; ++i) {
        double rolled = 0.0;
        CHECK(!r.roll(&rolled));
        CHECK(rolled >= 0.0 && rolled < 1.0);  // roll value is sane
    }
    CHECK_EQ(r.rolls(), static_cast<unsigned long long>(kTrials));
}

// ----------------------------------------------------------------------------
// p = 1.0 -> always attempts chaos.
// ----------------------------------------------------------------------------
static void test_full_probability_always_fires() {
    ChaosRoller r(42);
    r.set_probability(1.0);
    for (int i = 0; i < kTrials; ++i) {
        CHECK(r.roll());
    }
}

// ----------------------------------------------------------------------------
// p = 0.6 -> approximately 60% of cycles fire (law of large numbers).
// ----------------------------------------------------------------------------
static void test_partial_probability_frequency() {
    ChaosRoller r(1234);
    r.set_probability(0.6);
    int fired = 0;
    for (int i = 0; i < kTrials; ++i) {
        if (r.roll()) ++fired;
    }
    const double observed = static_cast<double>(fired) / kTrials;
    CHECK(observed > 0.55 && observed < 0.65);
}

// ----------------------------------------------------------------------------
// Out-of-range and NaN values clamp; the gate meaning is never inverted.
// ----------------------------------------------------------------------------
static void test_clamping() {
    {
        ChaosRoller r(42);
        r.set_probability(-5.0);
        CHECK_EQ(r.probability(), 0.0);
        CHECK(!r.roll());  // clamped to "never"
    }
    {
        ChaosRoller r(42);
        r.set_probability(7.0);
        CHECK_EQ(r.probability(), 1.0);
        CHECK(r.roll());  // clamped to "always"
    }
    {
        ChaosRoller r(42);
        r.set_probability(0.0 / 0.0);  // NaN -> must not mean "always"
        CHECK_EQ(r.probability(), 0.0);
        CHECK(!r.roll());
    }
}

// ----------------------------------------------------------------------------
// Seeded rollers are reproducible; default construction is (effectively) not.
// ----------------------------------------------------------------------------
static void test_reproducibility() {
    ChaosRoller a(777);
    ChaosRoller b(777);
    a.set_probability(0.5);
    b.set_probability(0.5);
    for (int i = 0; i < 1000; ++i) {
        CHECK_EQ(a.roll(), b.roll());
    }

    // Two default-seeded rollers agreeing on all of 64 rolls is ~2^-64.
    ChaosRoller c;
    ChaosRoller d;
    c.set_probability(0.5);
    d.set_probability(0.5);
    int agreement = 0;
    for (int i = 0; i < 64; ++i) {
        if (c.roll() == d.roll()) ++agreement;
    }
    CHECK(agreement < 64);
}

// ----------------------------------------------------------------------------
// Roll values stay in [0, 1).
// ----------------------------------------------------------------------------
static void test_roll_value_range() {
    ChaosRoller r(9000);
    r.set_probability(1.0);
    for (int i = 0; i < kTrials; ++i) {
        double rolled = -1.0;
        (void)r.roll(&rolled);
        CHECK(rolled >= 0.0 && rolled < 1.0);
    }
}

// ----------------------------------------------------------------------------
int main() {
    test_zero_probability_never_fires();
    test_full_probability_always_fires();
    test_partial_probability_frequency();
    test_clamping();
    test_reproducibility();
    test_roll_value_range();
    test_action_budget();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
