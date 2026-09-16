// ============================================================================
//  src/engine.cpp — the chaos engine: one tick = discover -> match -> roll -> strike
// ============================================================================

#include "Chaos_Forger/engine.hpp"

#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Chaos_Forger/log.hpp"

namespace Chaos_Forger {

namespace {

std::string format_probability(double p) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << p;
    return os.str();
}

}  // namespace

ChaosEngine::ChaosEngine(Deps deps, Config cfg, std::optional<bool> dry_run_override)
    : deps_(deps),
      cfg_(std::move(cfg)),
      dry_run_override_(dry_run_override),
      dry_run_(dry_run_override_.value_or(cfg_.dry_run)) {
    roller_.set_probability(cfg_.chaos_probability);
}

// The one INFO line a quiet cycle is allowed to spend.
void ChaosEngine::log_quiet_tick(size_t running_count) const {
    LOG_INFO("tick: " + std::to_string(running_count) + " running container(s), no match for " +
             std::to_string(cfg_.targets.size()) + " rule(s)");
}

bool ChaosEngine::tick() {
    // Shutdown guard #1: once shutdown begins, no new work starts — no
    // discovery, no dice roll, no strikes. A tick that began earlier may
    // finish (bounded by the 5 s socket timeout).
    if (deps_.shutdown->stop_requested()) {
        LOG_DEBUG("shutdown in progress: cycle skipped before discovery");
        return true;
    }

    std::string err;
    std::vector<Container> running;
    if (!deps_.docker->list_containers(running, err)) {
        ++counters_.errors;
        LOG_ERROR("discovery failed: " + err);
        return false;
    }
    // Loop-log line 1 (DEBUG): how many running containers were seen.
    // A quiet cycle must cost one INFO line, not three.
    LOG_DEBUG("tick: " + std::to_string(running.size()) + " running container(s) discovered");

    // Matching policy (names-only, case-sensitive substring, first rule
    // wins) lives in Chaos_Forger::match_containers — see Chaos_Forger/discovery.hpp.
    const auto matched = match_containers(running, cfg_.targets);
    if (matched.empty()) {
        log_quiet_tick(running.size());
        return true;
    }
    // One line per armed container: the friendly name on INFO (the line an
    // operator greps for), the full rule detail on DEBUG.
    for (const MatchedContainer& m : matched) {
        const TargetRule& rule = cfg_.targets[m.rule_index];
        LOG_INFO("Matched container " +
                 std::string(m.container.names.empty() ? m.container.id.substr(0, 12)
                                                       : m.container.names.front()));
        LOG_DEBUG("matched rule: '" + rule.name_match + "' -> " +
                  std::string(action_name(rule.action)) + " (targets[" +
                  std::to_string(m.rule_index) + "]) id=" + m.container.id);
    }

    // Shutdown guard #2: a signal may arrive between discovery and the
    // roll — armed targets are released without a single strike.
    if (deps_.shutdown->stop_requested()) {
        LOG_WARN("shutdown requested mid-cycle: " + std::to_string(matched.size()) +
                 " armed target(s) released without strikes");
        return true;
    }

    // --discover: log matches and stop -- no dice, no strikes.
    if (deps_.discover_only) return true;

    if (should_strike()) {
        // Blast-radius cap: select at most max_actions_per_cycle targets,
        // never exceeding the remaining max_actions_per_run budget.
        const auto selected =
            action_budget(matched, cfg_.max_actions_per_cycle, cfg_.max_actions_per_run,
                          run_budget_used_);
        if (selected.size() < matched.size()) {
            LOG_WARN("cycle budget: " + std::to_string(matched.size() - selected.size()) +
                     " of " + std::to_string(matched.size()) + " armed target(s) held back " +
                     "(max_actions_per_cycle=" + std::to_string(cfg_.max_actions_per_cycle) +
                     ", run budget " + std::to_string(run_budget_used_) + "/" +
                     (cfg_.max_actions_per_run > 0
                          ? std::to_string(cfg_.max_actions_per_run)
                          : std::string("unlimited")) +
                     ")");
        }
        execute_strikes(selected);
    }
    return true;
}

// The probability gate: evaluated ONCE per scheduler cycle, before any
// strike. When it fires, every armed container is struck; when it does
// not, none are. Semantics documented in Chaos_Forger/chaos.hpp.
bool ChaosEngine::should_strike() {
    ++counters_.rolls;
    if (forced_roll_.has_value()) {
        const bool fires = *forced_roll_;
        forced_roll_.reset();  // one-shot: the next roll is random again
        LOG_DEBUG("forced roll (test): -> " + std::string(fires ? "FIRES" : "no chaos"));
        return fires;
    }
    double rolled = 0.0;
    const bool fires = roller_.roll(&rolled);
    // Losing rolls are routine; only a fired cycle is worth an INFO line.
    const std::string line = "dice roll: " + format_probability(rolled) +
                             (fires ? " < " : " >= ") + format_probability(cfg_.chaos_probability) +
                             (fires ? " -> CHAOS FIRES" : " -> no chaos this cycle");
    if (fires) {
        LOG_INFO(line);
    } else {
        LOG_DEBUG(line);
    }
    return fires;
}

void ChaosEngine::execute_strikes(const std::vector<MatchedContainer>& armed) {
    for (const MatchedContainer& m : armed) {
        // Shutdown guard #3: stop issuing new strikes the moment shutdown
        // begins, even mid-sweep.
        if (deps_.shutdown->stop_requested()) {
            LOG_WARN("shutdown requested mid-sweep: remaining strike(s) cancelled");
            return;
        }
        const TargetRule& rule = cfg_.targets[m.rule_index];
        const Container& container = m.container;
        const std::string label =
            container.names.empty()
                ? container.id.substr(0, 12)
                : container.names.front() + " (" + container.id.substr(0, 12) + ")";

        if (dry_run_) {
            // WARN (-> stderr): dry-run "would" lines must stand out as
            // "this did NOT happen" — a strike log they are not.
            LOG_WARN("Dry-run: would " + std::string(action_name(rule.action)) + " " + label);
            continue;  // no stop/kill is ever sent in dry-run mode
        }

        std::string err;
        int http_status = 0;
        bool ok = false;
        if (rule.action == Action::Stop) {
            LOG_INFO("strike: stop container " + label + " id=" + container.id +
                     " (graceful, t=" + std::to_string(cfg_.stop_timeout_seconds) + "s)");
            ok = deps_.docker->stop_container(container.id, cfg_.stop_timeout_seconds, err,
                                              &http_status);
        } else {
            LOG_INFO("strike: kill container " + label + " id=" + container.id + " (SIGKILL)");
            ok = deps_.docker->kill_container(container.id, err, &http_status);
        }
        if (ok) {
            ++counters_.strikes;
            ++run_budget_used_;
            LOG_INFO("strike ok: action=" + std::string(action_name(rule.action)) +
                     " target=" + label + " http=" + std::to_string(http_status));
        } else if (http_status == 404 || http_status == 409) {
            // Container vanished or already stopped between discovery and strike.
            LOG_WARN("strike skipped: action=" + std::string(action_name(rule.action)) +
                     " target=" + label + " http=" + std::to_string(http_status) +
                     " reason=" + err);
        } else {
            ++counters_.errors;
            LOG_ERROR("strike failed: action=" + std::string(action_name(rule.action)) +
                      " target=" + label +
                      " http=" + (http_status > 0 ? std::to_string(http_status) : "none") +
                      " reason=" + err);
        }
        // Continue with the remaining targets: one failure never aborts the sweep.
    }
}

void ChaosEngine::force_next_roll(bool fires) { forced_roll_ = fires; }

}  // namespace Chaos_Forger
