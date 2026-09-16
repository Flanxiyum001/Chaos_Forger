#pragma once
// ============================================================================
//  Chaos_Forger/log.hpp — leveled, timestamped logging (implementation in src/log.cpp)
//
//  Format:  [YYYY-MM-DD HH:MM:SS] [LEVEL] message
//  Streams: DEBUG/INFO -> stdout, WARN/ERROR -> stderr
//
//  Thread-safe: each call emits one whole line under a mutex, so concurrent
//  loggers cannot interleave mid-line. Level filtering happens before any
//  formatting work. The initial level comes from $Chaos_Forger_LOG_LEVEL
//  (debug|info|warn|error, default info); main() may override it from the
//  config file via Chaos_Forger_set_log_level().
//
//  Log-forging hardening: CR/LF bytes in messages are folded to spaces by the
//  backend, so attacker-influenced strings (container names, Engine error
//  bodies) can never forge extra '[LEVEL] ...' lines for log parsers.
// ============================================================================

#include <string>

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

// Defined in main.cpp; the library code calls the macros below.
void Chaos_Forger_set_log_level(LogLevel lvl);
LogLevel Chaos_Forger_log_level();
void Chaos_Forger_log_line(LogLevel lvl, const std::string& msg);

#define LOG_DEBUG(msg) ::Chaos_Forger_log_line(LogLevel::Debug, (msg))
#define LOG_INFO(msg)  ::Chaos_Forger_log_line(LogLevel::Info, (msg))
#define LOG_WARN(msg)  ::Chaos_Forger_log_line(LogLevel::Warn, (msg))
#define LOG_ERROR(msg) ::Chaos_Forger_log_line(LogLevel::Error, (msg))

namespace Chaos_Forger {

// errno -> human-readable context, e.g. "connect failed: Connection refused".
std::string sys_error(const std::string& what);

}  // namespace Chaos_Forger
