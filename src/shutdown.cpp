// ============================================================================
//  src/shutdown.cpp — ShutdownState implementation
// ============================================================================

#include "forger/shutdown.hpp"

#include <chrono>

namespace forger {

const char* stop_reason_name(StopReason r) {
    switch (r) {
        case StopReason::NotRequested: return "not requested";
        case StopReason::Signal: return "signal";
        case StopReason::Internal: return "internal shutdown";
    }
    return "unknown";
}

void ShutdownState::wait_for_stop(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [this] { return state_.load(std::memory_order_acquire); });
}

}  // namespace forger
