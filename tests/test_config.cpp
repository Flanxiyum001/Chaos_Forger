// ============================================================================
//  tests/test_config.cpp — unit tests for the Forger configuration system
// ============================================================================

#include "forger/config.hpp"

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

#define CHECK_CONTAINS(haystack, needle)                                     \
    do {                                                                     \
        ++g_checks;                                                          \
        if ((haystack).find(needle) == std::string::npos) {                  \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: '%s' should contain '%s'\n",   \
                         __FILE__, __LINE__, #haystack, needle);             \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!((a) == (b))) {                                                 \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__,        \
                         __LINE__, #a, #b);                                  \
        }                                                                    \
    } while (0)

using namespace forger;

// ----------------------------------------------------------------------------
static bool validate_text(const std::string& text, Config& cfg, std::string& err) {
    Json root;
    std::string jerr;
    if (!JsonParser::parse(text, root, jerr)) {
        err = "parse: " + jerr;
        return false;
    }
    return validate_config(root, cfg, err);
}

// ----------------------------------------------------------------------------
static void test_valid_minimal() {
    const std::string text = R"({
        "interval_seconds": 5,
        "chaos_probability": 0.6,
        "targets": [
            {"name_match": "web-app", "action": "stop"},
            {"name_match": "database", "action": "kill"}
        ]
    })";
    Config cfg;
    std::string err;
    CHECK(validate_text(text, cfg, err));
    CHECK(err.empty());
    CHECK_EQ(cfg.interval_seconds, 5);
    CHECK_EQ(cfg.chaos_probability, 0.6);
    CHECK_EQ(cfg.targets.size(), 2u);
    CHECK_EQ(cfg.targets[0].name_match, "web-app");
    CHECK_EQ(cfg.targets[0].action, Action::Stop);
    CHECK_EQ(cfg.targets[1].action, Action::Kill);
    // Defaults untouched by the minimal file. dry_run defaults TRUE (safe).
    CHECK_EQ(cfg.docker_socket, "/var/run/docker.sock");
    CHECK_EQ(cfg.stop_timeout_seconds, 10);
    CHECK_EQ(cfg.dry_run, true);
    CHECK_EQ(cfg.max_actions_per_cycle, 1);
    CHECK_EQ(cfg.max_actions_per_run, 0L);
    CHECK_EQ(cfg.log_level, "info");
}

static void test_valid_full_and_normalization() {
    const std::string text = R"({
        "interval_seconds": 1,
        "chaos_probability": 0.0,
        "docker_socket": "/tmp/custom.sock",
        "stop_timeout_seconds": 600,
        "dry_run": false,
        "max_actions_per_cycle": 5,
        "max_actions_per_run": 100,
        "log_level": "DEBUG",
        "targets": [{"name_match": "  padded  ", "action": " STOP "}]
    })";
    Config cfg;
    std::string err;
    CHECK(validate_text(text, cfg, err));
    CHECK_EQ(cfg.log_level, "debug");               // normalized case
    CHECK_EQ(cfg.targets[0].name_match, "padded");  // trimmed
    CHECK_EQ(cfg.targets[0].action, Action::Stop);  // case-insensitive
    CHECK_EQ(cfg.chaos_probability, 0.0);
    CHECK_EQ(cfg.dry_run, false);                   // explicit opt-out honored
    CHECK_EQ(cfg.max_actions_per_cycle, 5);
    CHECK_EQ(cfg.max_actions_per_run, 100L);
}

static void test_probability_boundaries() {
    // 0.0 and 1.0 are legal.
    for (const char* p : {"0.0", "1.0", "1", "0"}) {
        Config cfg;
        std::string err;
        CHECK(validate_text(std::string("{\"chaos_probability\":") + p +
                            ",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
                            cfg, err));
    }
    // Out of range rejected, with the value in the message.
    for (const char* p : {"-0.1", "1.01", "2"}) {
        Config cfg;
        std::string err;
        CHECK(!validate_text(std::string("{\"chaos_probability\":") + p +
                             ",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
                             cfg, err));
        CHECK_CONTAINS(err, "chaos_probability");
        CHECK_CONTAINS(err, p);
    }
}

static void test_interval_boundaries() {
    for (const char* n : {"1", "86400"}) {
        Config cfg;
        std::string err;
        CHECK(validate_text(std::string("{\"interval_seconds\":") + n +
                            ",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
                            cfg, err));
    }
    for (const char* n : {"0", "-5", "86401", "2.5"}) {
        Config cfg;
        std::string err;
        CHECK(!validate_text(std::string("{\"interval_seconds\":") + n +
                             ",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
                             cfg, err));
        CHECK_CONTAINS(err, "interval_seconds");
    }
}

static void test_targets_required() {
    // Missing key.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text("{}", cfg, err));
        CHECK_CONTAINS(err, "targets");
        CHECK_CONTAINS(err, "required");
    }
    // Empty array.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text("{\"targets\": []}", cfg, err));
        CHECK_CONTAINS(err, "non-empty");
    }
    // Wrong type.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text("{\"targets\": \"web\"}", cfg, err));
        CHECK_CONTAINS(err, "array");
    }
}

static void test_target_rules() {
    // Missing name_match / action — error names the index.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text("{\"targets\":[{\"action\":\"stop\"}]}", cfg, err));
        CHECK_CONTAINS(err, "targets[0]");
        CHECK_CONTAINS(err, "name_match");
    }
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text("{\"targets\":[{\"name_match\":\"x\"}]}", cfg, err));
        CHECK_CONTAINS(err, "targets[0]");
        CHECK_CONTAINS(err, "action");
    }
    // Empty name_match.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text("{\"targets\":[{\"name_match\":\"   \",\"action\":\"stop\"}]}",
                             cfg, err));
        CHECK_CONTAINS(err, "name_match");
    }
    // Bad action — message quotes the value.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text(
            "{\"targets\":[{\"name_match\":\"x\",\"action\":\"explode\"}]}", cfg, err));
        CHECK_CONTAINS(err, "targets[0].action");
        CHECK_CONTAINS(err, "explode");
    }
    // Wrong-typed action.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text("{\"targets\":[{\"name_match\":\"x\",\"action\":3}]}", cfg, err));
        CHECK_CONTAINS(err, "action");
    }
    // Index accounting across several rules.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text(
            "{\"targets\":["
            "{\"name_match\":\"a\",\"action\":\"stop\"},"
            "{\"name_match\":\"b\",\"action\":\"kill\"},"
            "{\"name_match\":\"c\",\"action\":\"pause\"}]}",
            cfg, err));
        CHECK_CONTAINS(err, "targets[2]");
    }
    // Rule entry not an object.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text("{\"targets\":[\"web-app\"]}", cfg, err));
        CHECK_CONTAINS(err, "targets[0]");
    }
}

static void test_other_fields_and_root() {
    // Root not an object.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text("[]", cfg, err));
        CHECK_CONTAINS(err, "root");
    }
    // dry_run wrong type.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text(
            "{\"dry_run\":\"yes\",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
            cfg, err));
        CHECK_CONTAINS(err, "dry_run");
    }
    // Empty docker_socket.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text(
            "{\"docker_socket\":\"\",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
            cfg, err));
        CHECK_CONTAINS(err, "docker_socket");
    }
    // Unknown keys warn but do not fail (typo resistance).
    {
        Config cfg;
        std::string err;
        CHECK(validate_text(
            "{\"interval_secunds\": 9,\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
            cfg, err));
        CHECK_EQ(cfg.interval_seconds, 5);  // default kept
    }
}

static void test_safety_fields() {
    // max_actions_per_cycle boundaries.
    for (const char* n : {"1", "1000"}) {
        Config cfg;
        std::string err;
        CHECK(validate_text(std::string("{\"max_actions_per_cycle\":") + n +
                            ",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
                            cfg, err));
    }
    for (const char* n : {"0", "-1", "2.5", "1001"}) {
        Config cfg;
        std::string err;
        CHECK(!validate_text(std::string("{\"max_actions_per_cycle\":") + n +
                             ",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
                             cfg, err));
        CHECK_CONTAINS(err, "max_actions_per_cycle");
    }
    // max_actions_per_run boundaries (0 = unlimited is legal).
    for (const char* n : {"0", "1", "1000000"}) {
        Config cfg;
        std::string err;
        CHECK(validate_text(std::string("{\"max_actions_per_run\":") + n +
                            ",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
                            cfg, err));
    }
    for (const char* n : {"-1", "3.2", "1000001"}) {
        Config cfg;
        std::string err;
        CHECK(!validate_text(std::string("{\"max_actions_per_run\":") + n +
                             ",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
                             cfg, err));
        CHECK_CONTAINS(err, "max_actions_per_run");
    }
    // Wrong types rejected.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text(
            "{\"max_actions_per_cycle\":\"three\",\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
            cfg, err));
    }
    // dry_run wrong type still rejected.
    {
        Config cfg;
        std::string err;
        CHECK(!validate_text(
            "{\"dry_run\":1,\"targets\":[{\"name_match\":\"x\",\"action\":\"stop\"}]}",
            cfg, err));
        CHECK_CONTAINS(err, "dry_run");
    }
}

static void test_load_from_disk() {
    // Missing file.
    {
        Config cfg;
        std::string err;
        CHECK(!load_config("/nonexistent/forger-config.json", cfg, err));
        CHECK_CONTAINS(err, "cannot open");
    }
    // Malformed JSON — parser error surfaces with byte offset.
    {
        Config cfg;
        std::string err;
        CHECK(!load_config("tests/data/broken.json", cfg, err));
        CHECK_CONTAINS(err, "JSON parse error");
    }
    // Valid file on disk.
    {
        Config cfg;
        std::string err;
        CHECK(load_config("config.json", cfg, err));
        CHECK(!cfg.targets.empty());
    }
}

// ----------------------------------------------------------------------------
int main() {
    test_valid_minimal();
    test_valid_full_and_normalization();
    test_probability_boundaries();
    test_interval_boundaries();
    test_targets_required();
    test_target_rules();
    test_other_fields_and_root();
    test_safety_fields();
    test_load_from_disk();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
