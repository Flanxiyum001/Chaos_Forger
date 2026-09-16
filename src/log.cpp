// ============================================================================
//  src/log.cpp — logging backend for the Chaos_Forger library
//
//  Format:  [YYYY-MM-DD HH:MM:SS] [LEVEL] message
//  Streams: DEBUG/INFO -> stdout, WARN/ERROR -> stderr
//
//  Deliberately not a logging framework: one mutex, one fprintf, one flush.
//  The mutex serializes whole lines even if multiple threads log concurrently
//  (the signal-listener thread never logs, so there is no lock-ordering
//  interaction with the shutdown path); the flush keeps tail -f and piped
//  runs live. Filtered-out levels pay one relaxed atomic load.
// ============================================================================

#include "Chaos_Forger/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace {

LogLevel level_from_env_or_default() {
    // getenv before main() is fine for plain C strings (no allocation, no
    // other threads exist yet); the value is parsed with strtoull.
    if (const char* raw = ::getenv("Chaos_Forger_LOG_LEVEL")) {
        const std::string v = raw;  // NOLINT: readable compare, runs once
        if (v == "debug") return LogLevel::Debug;
        if (v == "info") return LogLevel::Info;
        if (v == "warn") return LogLevel::Warn;
        if (v == "error") return LogLevel::Error;
        // Unrecognized values fall through to the default below.
    }
    return LogLevel::Info;
}

std::atomic<LogLevel> g_log_level{level_from_env_or_default()};

const char* log_tag(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

bool goes_to_stderr(LogLevel lvl) { return lvl >= LogLevel::Warn; }

}  // namespace

void Chaos_Forger_set_log_level(LogLevel lvl) { g_log_level.store(lvl, std::memory_order_relaxed); }

LogLevel Chaos_Forger_log_level() { return g_log_level.load(std::memory_order_relaxed); }

void Chaos_Forger_log_line(LogLevel lvl, const std::string& msg) {
    if (lvl < g_log_level.load(std::memory_order_relaxed)) return;

    using clock = std::chrono::system_clock;
    const std::time_t t = clock::to_time_t(clock::now());
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    // Log-forging hardening: container names and Engine error bodies reach
    // these lines. Fold CR/LF to spaces so attacker-influenced strings can
    // never start a forged '[LEVEL] ...' line of their own. Bytes >= 0x80
    // pass through untouched (the log is byte-transparent for UTF-8).
    std::string safe;
    safe.reserve(msg.size());
    for (const char c : msg) {
        safe += (c == '\n' || c == '\r') ? ' ' : c;
    }

    // One lock per line: concurrent loggers can never interleave mid-line.
    static std::mutex log_mutex;
    const std::lock_guard<std::mutex> lock(log_mutex);
    std::FILE* stream = goes_to_stderr(lvl) ? stderr : stdout;
    std::fprintf(stream, "[%s] [%s] %s\n", stamp, log_tag(lvl), safe.c_str());
    std::fflush(stream);
}
