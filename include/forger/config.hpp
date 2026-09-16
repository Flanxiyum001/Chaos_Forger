#pragma once
// ============================================================================
//  Chaos_Forger/config.hpp — configuration model, loading, validation
//
//  A missing or invalid config must stop Chaos_Forger at startup with a precise,
//  actionable error — never a crash, never a silent default.
// ============================================================================

#include <string>
#include <vector>

#include "Chaos_Forger/json.hpp"

namespace Chaos_Forger {

enum class Action { Stop, Kill };

const char* action_name(Action a);          // "stop" | "kill"
bool action_from_string(const std::string& s, Action& out, std::string& err);

struct TargetRule {
    std::string name_match;  // case-sensitive substring match (see discovery.hpp)
    Action action = Action::Stop;
};

struct Config {
    int interval_seconds = 5;                 // > 0
    double chaos_probability = 0.3;           // [0.0, 1.0]
    std::string docker_socket = "/var/run/docker.sock";
    int stop_timeout_seconds = 10;            // [1, 600]

    // --- safety controls -------------------------------------------------------
    // SAFE DEFAULT: a config that omits dry_run runs in dry-run mode. Real
    // destruction requires the operator to write "dry_run": false.
    bool dry_run = true;
    // Blast-radius caps. Per cycle: at most this many strikes per tick.
    // Per run: lifetime budget for the whole process (0 = unlimited).
    int max_actions_per_cycle = 1;            // [1, 1000]
    long max_actions_per_run = 0;             // [0, 1e6], 0 = unlimited
    std::string log_level = "info";           // debug|info|warn|error
    std::vector<TargetRule> targets;          // required, non-empty
};

// Validates and normalizes a parsed JSON object into `cfg`.
// Returns false with a human-readable `err` (includes the offending value)
// on any rule violation. Warnings (unknown keys, duplicate rules) are logged.
bool validate_config(const Json& root, Config& cfg, std::string& err);

// Convenience: read file -> parse JSON -> validate.
bool load_config(const std::string& path, Config& cfg, std::string& err);

}  // namespace Chaos_Forger
