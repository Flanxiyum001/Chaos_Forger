#pragma once
// ============================================================================
//  Chaos_Forger/shutdown.hpp — the process-wide shutdown state, as a testable object
//
//  Extraction of the flag previously kept in main.cpp, so the chaos engine's
//  shutdown guards are unit-testable without signals, threads, or a daemon:
//  tests flip request() and assert tick() skips work and strikes stop
//  mid-sweep. Chaos_Forger runs exactly one ShutdownState (main.cpp owns it); the
//  class itself is plain value semantics, no globals.
//
//  Wait mechanism (production): a condition_variable so SIGINT/SIGTERM via
//  the signal-listener thread wakes the scheduler the moment shutdown is
//  requested — never after a long interval_seconds sleep.
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

namespace Chaos_Forger {

// Why the process is stopping. Used for the exit-reason log line and for
// deciding the exit code in main(). Extensible: future internal shutdown
// paths (budget exhausted, SIGHUP reload) add enumerators here.
enum class StopReason {
    NotRequested,   // running normally
    Signal,         // SIGINT/SIGTERM (or any process-directed signal)
    Internal,       // reserved for future internal shutdown paths
};

const char* stop_reason_name(StopReason r);

class ShutdownState {
public:
    ShutdownState() = default;

    // Non-copyable: the CV/mutex pair is not meant to be duplicated.
    ShutdownState(const ShutdownState&) = delete;
    ShutdownState& operator=(const ShutdownState&) = delete;

    // -- queries (wait-free) ------------------------------------------------
    bool stop_requested() const { return state_.load(std::memory_order_acquire); }
    bool running() const { return !stop_requested(); }
    StopReason reason() const { return reason_.load(std::memory_order_acquire); }

    // -- commands ------------------------------------------------------------
    // Idempotent: the first caller wins — both the transition and the reason
    // (a later request never overwrites the original cause, so the final log
    // line reports why the daemon actually stopped). Wakes wait_for_stop().
    void request(StopReason reason) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!state_.exchange(true, std::memory_order_acq_rel)) {
                reason_.store(reason, std::memory_order_release);
            }
        }
        cv_.notify_all();
    }

    // Convenience for the signal-listener thread.
    void request_signal() { request(StopReason::Signal); }

    // -- interruptible wait ---------------------------------------------------
    // Blocks up to `timeout_ms` (0 = check-and-return) and returns as soon as
    // stop is requested. The predicate form (wait_for with predicate) closes
    // the lost-wakeup race between flag store and notify.
    void wait_for_stop(int timeout_ms);

private:
    std::atomic<bool> state_{false};
    std::atomic<StopReason> reason_{StopReason::NotRequested};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

}  // namespace Chaos_Forger
