// ============================================================================
//  tests/test_shutdown.cpp — shutdown state transitions (no signals, no Docker)
//
//  Pins the shutdown contract:
//    - transitions: running -> stop_requested, first reason wins (idempotent)
//    - wait_for_stop returns promptly when stop is requested from another
//      thread (the condition-variable contract the scheduler relies on)
//    - reason() starts NotRequested and stays at the first requested reason
// ============================================================================

#include "Chaos_Forger/shutdown.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

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

using Chaos_Forger::ShutdownState;
using Chaos_Forger::StopReason;

// ----------------------------------------------------------------------------
// Initial state: running, no reason.
// ----------------------------------------------------------------------------
static void test_initial_state() {
    ShutdownState s;
    CHECK(!s.stop_requested());
    CHECK(s.running());
    CHECK(s.reason() == StopReason::NotRequested);
}

// ----------------------------------------------------------------------------
// request() flips running -> stopped exactly once and never regresses.
// ----------------------------------------------------------------------------
static void test_request_transitions() {
    ShutdownState s;
    CHECK(s.running());

    s.request(StopReason::Signal);
    CHECK(s.stop_requested());
    CHECK(!s.running());
    CHECK(s.reason() == StopReason::Signal);

    // Idempotent: further requests do not change state or reason.
    s.request(StopReason::Internal);
    CHECK(s.stop_requested());
    CHECK(s.reason() == StopReason::Signal);
}

// ----------------------------------------------------------------------------
// First reason wins: an internal shutdown does not overwrite an earlier signal.
// ----------------------------------------------------------------------------
static void test_first_reason_wins() {
    ShutdownState s;
    s.request_signal();
    CHECK(s.reason() == StopReason::Signal);
    s.request(StopReason::Internal);
    CHECK(s.reason() == StopReason::Signal);
}

// ----------------------------------------------------------------------------
// wait_for_stop(0) is a non-blocking check; a short wait times out while
// running and returns immediately once stop is requested.
// ----------------------------------------------------------------------------
static void test_wait_timeouts() {
    ShutdownState s;

    const auto t0 = std::chrono::steady_clock::now();
    s.wait_for_stop(0);
    const auto elapsed0 = std::chrono::steady_clock::now() - t0;
    CHECK(elapsed0 < std::chrono::milliseconds(50));
    CHECK(s.running());

    const auto t1 = std::chrono::steady_clock::now();
    s.wait_for_stop(50);
    const auto elapsed1 = std::chrono::steady_clock::now() - t1;
    CHECK(elapsed1 >= std::chrono::milliseconds(45));
    CHECK(elapsed1 < std::chrono::milliseconds(500));
    CHECK(s.running());

    // Once requested, any wait returns immediately.
    s.request(StopReason::Internal);
    const auto t2 = std::chrono::steady_clock::now();
    s.wait_for_stop(5000);
    CHECK(std::chrono::steady_clock::now() - t2 < std::chrono::milliseconds(500));
}

// ----------------------------------------------------------------------------
// Cross-thread wake: a waiter blocked in wait_for_stop() must return promptly
// when another thread requests stop (this is what makes SIGINT/SIGTERM end a
// scheduler sleep immediately, no matter how long interval_seconds is).
// ----------------------------------------------------------------------------
static void test_cross_thread_wake() {
    ShutdownState s;
    std::atomic<bool> waiter_returned{false};

    std::thread waiter([&] {
        s.wait_for_stop(10'000);  // would sleep 10 s if the wake never arrives
        waiter_returned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t0 = std::chrono::steady_clock::now();
    s.request(StopReason::Signal);
    waiter.join();
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    CHECK(waiter_returned.load());
    CHECK(elapsed < std::chrono::milliseconds(500));  // prompt, not 10 s
}

// ----------------------------------------------------------------------------
int main() {
    test_initial_state();
    test_request_transitions();
    test_first_reason_wins();
    test_wait_timeouts();
    test_cross_thread_wake();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
