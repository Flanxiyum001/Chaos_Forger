// ============================================================================
//  src/discovery.cpp — container discovery matching (pure logic, no I/O)
// ============================================================================

#include "forger/discovery.hpp"

namespace forger {

bool contains_substring(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    return haystack.find(needle) != std::string::npos;
}

std::vector<MatchedContainer> match_containers(const std::vector<Container>& containers,
                                               const std::vector<TargetRule>& rules) {
    std::vector<MatchedContainer> matched;
    for (const Container& c : containers) {
        // A container with no names can never match (defensive: the Engine
        // always returns at least one, but parse skips nothing silently).
        for (std::size_t r = 0; r < rules.size(); ++r) {
            bool hit = false;
            for (const std::string& name : c.names) {
                if (contains_substring(name, rules[r].name_match)) {
                    hit = true;
                    break;
                }
            }
            if (hit) {
                matched.push_back(MatchedContainer{c, r});
                break;  // first matching rule wins: one entry per container
            }
        }
    }
    return matched;
}

}  // namespace forger
