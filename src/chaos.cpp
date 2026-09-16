// ============================================================================
//  src/chaos.cpp — probability decision core
// ============================================================================

#include "Chaos_Forger/chaos.hpp"

#include <cmath>

namespace Chaos_Forger {

std::vector<MatchedContainer> action_budget(const std::vector<MatchedContainer>& armed,
                                            int max_per_cycle,
                                            long max_per_run,
                                            long already_used) {
    if (max_per_cycle < 1) max_per_cycle = 1;  // 0/invalid must not disable strikes
    if (max_per_run < 0) max_per_run = 0;

    // Run budget exhausted (or overshot by an earlier config change) -> none.
    if (max_per_run > 0 && already_used >= max_per_run) return {};

    std::size_t allowed = static_cast<std::size_t>(max_per_cycle);
    if (max_per_run > 0) {
        const long remaining = max_per_run - already_used;
        const std::size_t remaining_sz = remaining > 0 ? static_cast<std::size_t>(remaining)
                                                       : 0;
        if (remaining_sz < allowed) allowed = remaining_sz;
    }
    if (armed.size() <= allowed) return armed;  // common case: budget not binding
    return std::vector<MatchedContainer>(armed.begin(), armed.begin() + static_cast<std::ptrdiff_t>(allowed));
}

ChaosRoller::ChaosRoller()
    : rng_(static_cast<unsigned long long>(std::random_device{}())) {}

ChaosRoller::ChaosRoller(unsigned long long seed) : rng_(seed) {}

void ChaosRoller::set_probability(double p) {
    if (std::isnan(p)) {
        probability_ = 0.0;  // a nonsensical value must not mean "always fire"
        return;
    }
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    probability_ = p;
}

bool ChaosRoller::roll(double* rolled) {
    ++rolls_;
    // uniform_real_distribution<double>(0,1) yields [0,1). Therefore:
    //   p = 0.0 -> roll < 0.0 is impossible  -> never fires
    //   p = 1.0 -> roll < 1.0 is always true -> always fires
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const double value = uniform(rng_);
    if (rolled != nullptr) *rolled = value;
    return value < probability_;
}

}  // namespace Chaos_Forger
