// ============================================================================
//  src/config.cpp — configuration model, loading, validation
// ============================================================================

#include "forger/config.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include "forger/log.hpp"

namespace forger {

namespace {

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string quoted(const std::string& s) { return "'" + s + "'"; }

std::string num(double v) {
    std::ostringstream os;
    if (v == std::floor(v) && std::isfinite(v)) {
        os << static_cast<long long>(v);
    } else {
        os << v;
    }
    return os.str();
}

}  // namespace

const char* action_name(Action a) { return a == Action::Stop ? "stop" : "kill"; }

bool action_from_string(const std::string& s, Action& out, std::string& err) {
    const std::string v = to_lower(trim(s));
    if (v == "stop") {
        out = Action::Stop;
        return true;
    }
    if (v == "kill") {
        out = Action::Kill;
        return true;
    }
    err = "must be \"stop\" or \"kill\", got " + quoted(s);
    return false;
}

bool validate_config(const Json& root, Config& cfg, std::string& err) {
    if (!root.is_object()) {
        err = std::string("config root must be a JSON object (got ") +
              (root.is_null()   ? "null"
               : root.is_array() ? "array"
                                 : "scalar") +
              ")";
        return false;
    }
    const JsonObject& obj = root.as_object();

    // --- interval_seconds -----------------------------------------------------
    if (const Json* v = root.find("interval_seconds")) {
        if (!v->is_number() || v->as_number() != std::floor(v->as_number()) ||
            v->as_number() < 1 || v->as_number() > 86400) {
            err = "'interval_seconds' must be an integer in [1, 86400]";
            if (v->is_number()) err += ", got " + num(v->as_number());
            return false;
        }
        cfg.interval_seconds = static_cast<int>(v->as_number());
    }

    // --- chaos_probability ----------------------------------------------------
    if (const Json* v = root.find("chaos_probability")) {
        if (!v->is_number() || v->as_number() < 0.0 || v->as_number() > 1.0) {
            err = "'chaos_probability' must be a number in [0.0, 1.0]";
            if (v->is_number()) err += ", got " + num(v->as_number());
            return false;
        }
        cfg.chaos_probability = v->as_number();
    }

    // --- docker_socket ----------------------------------------------------------
    if (const Json* v = root.find("docker_socket")) {
        if (!v->is_string() || v->as_string().empty()) {
            err = "'docker_socket' must be a non-empty string";
            return false;
        }
        cfg.docker_socket = v->as_string();
    }

    // --- stop_timeout_seconds ---------------------------------------------------
    if (const Json* v = root.find("stop_timeout_seconds")) {
        if (!v->is_number() || v->as_number() != std::floor(v->as_number()) ||
            v->as_number() < 1 || v->as_number() > 600) {
            err = "'stop_timeout_seconds' must be an integer in [1, 600]";
            if (v->is_number()) err += ", got " + num(v->as_number());
            return false;
        }
        cfg.stop_timeout_seconds = static_cast<int>(v->as_number());
    }

    // --- dry_run (SAFE DEFAULT: absent key => dry-run mode) -------------------
    if (const Json* v = root.find("dry_run")) {
        if (!v->is_bool()) {
            err = "'dry_run' can only be true or false";
            return false;
        }
        cfg.dry_run = v->as_bool();
    }

    // --- max_actions_per_cycle ------------------------------------------------
    if (const Json* v = root.find("max_actions_per_cycle")) {
        if (!v->is_number() || v->as_number() != std::floor(v->as_number()) ||
            v->as_number() < 1 || v->as_number() > 1000) {
            err = "'max_actions_per_cycle' must be an integer in [1, 1000]";
            if (v->is_number()) err += ", got " + num(v->as_number());
            return false;
        }
        cfg.max_actions_per_cycle = static_cast<int>(v->as_number());
    }

    // --- max_actions_per_run (0 = unlimited) ----------------------------------
    if (const Json* v = root.find("max_actions_per_run")) {
        if (!v->is_number() || v->as_number() != std::floor(v->as_number()) ||
            v->as_number() < 0 || v->as_number() > 1000000) {
            err = "'max_actions_per_run' must be an integer in [0, 1000000] (0 = unlimited)";
            if (v->is_number()) err += ", got " + num(v->as_number());
            return false;
        }
        cfg.max_actions_per_run = static_cast<long>(v->as_number());
    }

    // --- log_level ---------------------------------------------------------
    if (const Json* v = root.find("log_level")) {
        if (!v->is_string()) {
            err = "'log_level' must be a string (debug|info|warn|error)";
            return false;
        }
        const std::string lvl = to_lower(trim(v->as_string()));
        if (lvl != "debug" && lvl != "info" && lvl != "warn" && lvl != "error") {
            err = "'log_level' must be one of debug|info|warn|error, got " + quoted(v->as_string());
            return false;
        }
        cfg.log_level = lvl;
    }

    // --- targets (required) ---------------------------------------------------
    const Json* targets = root.find("targets");
    if (!targets) {
        err = "'targets' is required: add at least one rule, e.g. "
              "\"targets\": [{\"name_match\": \"web-app\", \"action\": \"stop\"}]";
        return false;
    }
    if (!targets->is_array() || targets->as_array().empty()) {
        err = "'targets' must be a non-empty array of rule objects";
        return false;
    }

    int index = 0;
    for (const Json& entry : targets->as_array()) {
        const std::string where = "targets[" + std::to_string(index) + "]";
        if (!entry.is_object()) {
            err = where + " must be an object with 'name_match' and 'action'";
            return false;
        }
        const Json* name = entry.find("name_match");
        const Json* action = entry.find("action");
        if (!name) {
            err = where + " is missing 'name_match'";
            return false;
        }
        if (!name->is_string() || trim(name->as_string()).empty()) {
            err = where + ".name_match must be a non-empty string" +
                  (name->is_string() ? " (got an empty/whitespace string)" : "");
            return false;
        }
        if (!action) {
            err = where + " is missing 'action'";
            return false;
        }
        if (!action->is_string()) {
            err = where + ".action must be a string (\"stop\" or \"kill\")";
            return false;
        }
        TargetRule rule;
        rule.name_match = trim(name->as_string());
        std::string aerr;
        if (!action_from_string(action->as_string(), rule.action, aerr)) {
            err = where + ".action " + aerr;
            return false;
        }
        cfg.targets.push_back(std::move(rule));
        ++index;
    }

    // A config that omits dry_run never asked for real destruction - say so,
    // because "why is nothing dying?" deserves an answer in the log.
    if (!root.find("dry_run")) {
        LOG_WARN("config: 'dry_run' not set - defaulting to DRY-RUN mode "
                 "(no strikes; set \"dry_run\": false to go live)");
    }

    // --- warnings (non-fatal, but almost always config mistakes) ---------------
    for (const auto& [key, val] : obj) {
        (void)val;
        if (key != "interval_seconds" && key != "chaos_probability" &&
            key != "docker_socket" && key != "stop_timeout_seconds" &&
            key != "dry_run" && key != "max_actions_per_cycle" &&
            key != "max_actions_per_run" && key != "log_level" &&
            key != "targets") {
            LOG_WARN("config: ignoring unknown key " + quoted(key));
        }
    }
    for (size_t i = 0; i < cfg.targets.size(); ++i) {
        for (size_t j = i + 1; j < cfg.targets.size(); ++j) {
            if (cfg.targets[i].name_match == cfg.targets[j].name_match &&
                cfg.targets[i].action == cfg.targets[j].action) {
                LOG_WARN("config: duplicate rule " + quoted(cfg.targets[i].name_match) + " -> " +
                         action_name(cfg.targets[i].action));
            }
        }
    }
    return true;
}

bool load_config(const std::string& path, Config& cfg, std::string& err) {
    std::ifstream file(path);
    if (!file) {
        err = "cannot open config file " + quoted(path) + ": " + std::strerror(errno);
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();
    if (trim(text).empty()) {
        err = "config file " + quoted(path) + " is empty";
        return false;
    }

    Json root;
    std::string jerr;
    if (!JsonParser::parse(text, root, jerr)) {
        err = "JSON parse error in " + quoted(path) + ": " + jerr;
        return false;
    }
    return validate_config(root, cfg, err);
}

}  // namespace forger
