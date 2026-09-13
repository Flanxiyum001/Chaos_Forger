// ============================================================================
//  tests/test_discovery.cpp — unit tests for container discovery + matching
//
//  Pins the discovery contract:
//    - GET /v1.41/containers/json?all=false (running only)
//    - Container{id, names, image, state, status} parsed from the list body
//    - names normalized (no leading '/')
//    - names-only, case-sensitive substring matching; first rule wins
// ============================================================================

#include "forger/discovery.hpp"
#include "forger/docker_api.hpp"

#include <cstdio>
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

using namespace forger;

// ----------------------------------------------------------------------------
// Test fixture: parse a canned /containers/json payload (what the Engine sends).
// ----------------------------------------------------------------------------
static std::vector<Container> parse(const std::string& body) {
    std::vector<Container> out;
    std::string err;
    if (!DockerClient::parse_container_list(body, out, err)) {
        std::fprintf(stderr, "parse_container_list failed: %s\n", err.c_str());
    }
    return out;
}

// ----------------------------------------------------------------------------
// Normalization: Docker's leading "/" must never leak into matching.
// ----------------------------------------------------------------------------
static void test_name_normalization() {
    CHECK_EQ(normalize_container_name("/web-app"), "web-app");
    CHECK_EQ(normalize_container_name("web-app"), "web-app");
    CHECK_EQ(normalize_container_name("//weird"), "weird");  // idempotent
    CHECK_EQ(normalize_container_name(""), "");
    CHECK_EQ(normalize_container_name("/"), "");

    // Through the parser: "/web-app" -> "web-app".
    const std::string id(64, 'a');
    const auto containers = parse("[{\"Id\":\"" + id + "\",\"Names\":[\"/web-app\"]}]");
    CHECK_EQ(containers.size(), 1u);
    CHECK_EQ(containers[0].names.size(), 1u);
    CHECK_EQ(containers[0].names[0], "web-app");
}

// ----------------------------------------------------------------------------
// Descriptive fields: id, names, image, state, status.
// ----------------------------------------------------------------------------
static void test_container_fields() {
    const std::string id1(64, 'a');
    const std::string id2(64, 'b');
    const std::string body =
        "[{\"Id\":\"" + id1 + "\",\"Names\":[\"/web-app\",\"/alias\"],"
        "\"Image\":\"nginx:alpine\",\"State\":\"running\",\"Status\":\"Up 2 minutes\"},"
        "{\"Id\":\"" + id2 + "\",\"Names\":[\"/db\"]}]";  // optional fields absent

    const auto containers = parse(body);
    CHECK_EQ(containers.size(), 2u);

    CHECK_EQ(containers[0].id, id1);
    CHECK_EQ(containers[0].names.size(), 2u);
    CHECK_EQ(containers[0].names[0], "web-app");
    CHECK_EQ(containers[0].names[1], "alias");
    CHECK_EQ(containers[0].image, "nginx:alpine");
    CHECK_EQ(containers[0].state, "running");
    CHECK_EQ(containers[0].status, "Up 2 minutes");

    CHECK_EQ(containers[1].id, id2);
    CHECK_EQ(containers[1].image, "");  // absent -> empty, not garbage
    CHECK_EQ(containers[1].state, "");
    CHECK_EQ(containers[1].status, "");
}

// ----------------------------------------------------------------------------
// The discovery query must be pinned to running-only.
// ----------------------------------------------------------------------------
static void test_running_only_query() {
    CHECK_EQ(containers_json_path(), "/v1.41/containers/json?all=false");
}

// ----------------------------------------------------------------------------
// Matching: substring semantics from the requirements.
// "web-app" matches /web-app, /my-web-app, /web-app-production.
// ----------------------------------------------------------------------------
static void test_substring_matching() {
    CHECK(contains_substring("web-app", "web-app"));            // exact
    CHECK(contains_substring("my-web-app", "web-app"));         // substring
    CHECK(contains_substring("web-app-production", "web-app")); // prefix-substring
    CHECK(!contains_substring("webapp", "web-app"));            // not a substring
    CHECK(!contains_substring("Web-App", "web-app"));           // case-sensitive
    CHECK(!contains_substring("WEB-APP", "web-app"));
    CHECK(!contains_substring("anything", ""));                 // empty never matches
    CHECK(!contains_substring("", "web-app"));
    CHECK(!contains_substring("", ""));                         // even empty against empty
}

// ----------------------------------------------------------------------------
// Names only: image/state/status must never influence matching.
// ----------------------------------------------------------------------------
static void test_names_only_matching() {
    const std::string id(64, 'c');
    // The image name contains the rule substring; the container name does not.
    // A names-only policy must NOT arm this container.
    const auto containers = parse(
        "[{\"Id\":\"" + id + "\",\"Names\":[\"/unrelated-app\"],"
        "\"Image\":\"web-app-image:latest\",\"State\":\"running\",\"Status\":\"Up 1 hour\"}]");

    const std::vector<TargetRule> rules = {{"web-app", Action::Stop}};
    const auto matched = match_containers(containers, rules);
    CHECK(matched.empty());  // would be armed by an image-matching implementation
}

// ----------------------------------------------------------------------------
// Rule application: which container matched which rule; first rule wins.
// ----------------------------------------------------------------------------
static void test_rule_application() {
    const std::string id1(64, 'a');
    const std::string id2(64, 'b');
    const std::string id3(64, 'c');
    const auto containers = parse(
        "[{\"Id\":\"" + id1 + "\",\"Names\":[\"/web-app\"]},"
        "{\"Id\":\"" + id2 + "\",\"Names\":[\"/my-web-app\"],\"Image\":\"redis:7\"},"
        "{\"Id\":\"" + id3 + "\",\"Names\":[\"/cache\",\"/web-app\"]}]");

    const std::vector<TargetRule> rules = {
        {"cache", Action::Stop},    // targets[0]
        {"web-app", Action::Kill},  // targets[1]
    };

    const auto matched = match_containers(containers, rules);
    CHECK_EQ(matched.size(), 3u);

    // Which containers matched which rules.
    CHECK_EQ(matched[0].container.id, id1);
    CHECK_EQ(matched[0].rule_index, 1u);  // /web-app -> targets[1]
    CHECK_EQ(matched[1].container.id, id2);
    CHECK_EQ(matched[1].rule_index, 1u);  // /my-web-app -> targets[1]
    CHECK_EQ(matched[2].container.id, id3);
    CHECK_EQ(matched[2].rule_index, 0u);  // /cache wins over /web-app (rule order)

    // One entry per container even when several rules match.
    const std::vector<TargetRule> rules_overlap = {
        {"web", Action::Stop},
        {"app", Action::Kill},
    };
    const auto overlap = match_containers(containers, rules_overlap);
    CHECK_EQ(overlap.size(), 3u);            // every container still listed once
    CHECK_EQ(overlap[0].rule_index, 0u);     // first matching rule wins
    CHECK_EQ(overlap[2].rule_index, 0u);     // /cache/~/web-app matches "web" first

    // No rules -> nothing armed.
    CHECK(match_containers(containers, {}).empty());

    // No containers -> nothing armed.
    const std::vector<TargetRule> rules_one = {{"web-app", Action::Stop}};
    CHECK(match_containers({}, rules_one).empty());
}

// ----------------------------------------------------------------------------
// End-to-end through parse + match, mirroring one discovery tick.
// ----------------------------------------------------------------------------
static void test_discovery_tick_end_to_end() {
    const std::string ida(64, 'a');
    const std::string idb(64, 'b');
    const std::string idc(64, 'c');
    const std::string body =
        "[{\"Id\":\"" + ida + "\",\"Names\":[\"/forger-web-1\"],"
        "\"Image\":\"nginx:alpine\",\"State\":\"running\",\"Status\":\"Up 2 minutes\"},"
        "{\"Id\":\"" + idb + "\",\"Names\":[\"/forger-cache-1\"],"
        "\"Image\":\"redis:alpine\",\"State\":\"running\",\"Status\":\"Up 2 minutes\"},"
        "{\"Id\":\"" + idc + "\",\"Names\":[\"/unrelated-app\"],"
        "\"Image\":\"web-app-image:latest\",\"State\":\"running\",\"Status\":\"Up 3 days\"}]";

    const auto running = parse(body);
    CHECK_EQ(running.size(), 3u);

    const std::vector<TargetRule> rules = {
        {"forger-web", Action::Stop},
        {"forger-cache", Action::Kill},
    };
    const auto matched = match_containers(running, rules);

    // Exactly the two forger-* containers, in rule order of first appearance.
    CHECK_EQ(matched.size(), 2u);
    CHECK_EQ(matched[0].container.names.at(0), "forger-web-1");
    CHECK_EQ(matched[0].rule_index, 0u);
    CHECK_EQ(matched[0].container.image, "nginx:alpine");
    CHECK_EQ(matched[0].container.state, "running");
    CHECK_EQ(matched[1].container.names.at(0), "forger-cache-1");
    CHECK_EQ(matched[1].rule_index, 1u);
    // The image-only "web-app" container stays untouched.
    for (const MatchedContainer& m : matched) {
        CHECK(m.container.names.at(0) != "unrelated-app");
    }
}

// ----------------------------------------------------------------------------
int main() {
    test_name_normalization();
    test_container_fields();
    test_running_only_query();
    test_substring_matching();
    test_names_only_matching();
    test_rule_application();
    test_discovery_tick_end_to_end();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
