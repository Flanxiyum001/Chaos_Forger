#pragma once
// ============================================================================
//  Chaos_Forger/discovery.hpp — container discovery matching (pure logic, no I/O)
//
//  Containers come in from the Docker client, rules from the config; the
//  matching policy lives here so it is unit-testable without a daemon.
//
//  Policy (pinned by tests/test_discovery.cpp):
//    - Match against container NAMES only — never image, state, status, or
//      labels. A rule like "web-app" targets the container called web-app,
//      not every container built from the web-app image.
//    - Case-sensitive substring: "web-app" matches /web-app, /my-web-app,
//      /web-app-production — but not /Web-App.
//    - First matching rule (lowest index in `targets`) wins: one entry per
//      container.
// ============================================================================

#include <cstddef>
#include <string>
#include <vector>

#include "Chaos_Forger/config.hpp"     // TargetRule
#include "Chaos_Forger/docker_api.hpp" // Container

namespace Chaos_Forger {

// Case-sensitive substring test. An empty needle never matches (config
// validation rejects empty name_match anyway — defense in depth).
bool contains_substring(const std::string& haystack, const std::string& needle);

// A container that matched one of the configured rules.
struct MatchedContainer {
    Container container;
    std::size_t rule_index;  // index into the config's targets array
};

// Apply the rules to a discovered container list.
std::vector<MatchedContainer> match_containers(const std::vector<Container>& containers,
                                               const std::vector<TargetRule>& rules);

}  // namespace Chaos_Forger
