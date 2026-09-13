// ============================================================================
//  Forger — lightweight chaos-engineering daemon for the local Docker Engine
//
//  Talks to Docker over the UNIX domain socket (/var/run/docker.sock) using
//  raw HTTP/1.1 built by hand. No Docker SDK, no libcurl, no Boost.
//
//  This file is a thin shell: config load, log level, socket presence check,
//  signal plumbing, lifecycle logging, and the daemon timing loop. The chaos
//  engine itself lives in the library (forger/engine.hpp) behind interfaces
//  (IDockerClient, ShutdownState) so it is unit-testable without Docker.
// ============================================================================

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "forger/config.hpp"
#include "forger/engine.hpp"
#include "forger/log.hpp"
#include "forger/shutdown.hpp"

#ifndef FORGER_VERSION
#define FORGER_VERSION "1.0.0"
#endif

using Clock = std::chrono::steady_clock;

// Forward declaration (defined below main's helpers).
static std::string format_prob(double p);

// ============================================================================
// [1] SHUTDOWN STATE + SIGNAL LISTENER (plumbing only; state in forger/shutdown)
// ============================================================================

// The single process-wide shutdown state: owned here, shared with the engine
// and the scheduler loop (both take non-owning pointers/references).
static forger::ShutdownState g_shutdown;

// Signal number that requested shutdown (0 until a signal is caught).
static std::atomic<int> g_signal_received{0};

// SIGINT/SIGTERM are blocked process-wide *before any thread exists*, so the
// kernel never delivers them into arbitrary code. A dedicated listener thread
// consumes them with sigwait(); no signal handler runs at all, which keeps the
// async-signal-safety rule trivially satisfied (nothing happens in a signal
// context -- no logging, no allocation, no locks).
static sigset_t g_signal_set;

// The listener thread is joined at shutdown (never detached): waking it via a
// pending signal lets sigwait() return, and joining guarantees the thread is
// gone before ShutdownState teardown.
static std::thread g_signal_listener;
static bool g_listener_started = false;

// Block SIGINT/SIGTERM in the calling (main) thread. Must run before any
// std::thread is created so every later thread inherits the blocked mask.
static bool install_signal_handlers(std::string& err) {
    ::sigemptyset(&g_signal_set);
    ::sigaddset(&g_signal_set, SIGINT);
    ::sigaddset(&g_signal_set, SIGTERM);
    if (::pthread_sigmask(SIG_BLOCK, &g_signal_set, nullptr) != 0) {
        err = forger::sys_error("pthread_sigmask(SIG_BLOCK) failed");
        return false;
    }
    return true;
}

// Listener thread body: blocks in sigwait() until SIGINT or SIGTERM, then
// records the reason and wakes the scheduler. It performs no logging and no
// allocation -- main() logs the lifecycle lines once it observes the flag, so
// output order stays deterministic.
static void signal_listener_main() {
    int sig = 0;
    const int rc = ::sigwait(&g_signal_set, &sig);
    if (rc != 0) return;  // never happens for a valid blocked set
    g_signal_received.store(sig, std::memory_order_release);
    g_shutdown.request_signal();  // flag store + condition-variable wake
}

static void spawn_signal_listener() {
    g_signal_listener = std::thread(signal_listener_main);
    g_listener_started = true;
}

// At shutdown: wake the sigwait() listener so it can be joined. SIGINT/SIGTERM
// are blocked in every thread, so this signal stays pending until the
// listener consumes it. If the external signal was already consumed, the
// pending self-signal dies with the process.
static void join_signal_listener() {
    if (!g_listener_started) return;
    ::kill(::getpid(), SIGTERM);
    g_signal_listener.join();
    g_listener_started = false;
}

static bool shutting_down() { return g_shutdown.stop_requested(); }

// ============================================================================
// [2] LOG LEVEL (config value or FORGER_LOG_LEVEL -> library log backend)
// ============================================================================

// FORGER_LOG_LEVEL=debug|info|warn|error overrides config's log_level (handy
// for CI and containers: no config edit needed). The value is case-insensitive;
// an empty value counts as unset. An invalid value is a hard startup error --
// silently running at the wrong verbosity is how strikes go unnoticed.
static bool log_level_from_env(std::string& out) {
    const char* raw = ::getenv("FORGER_LOG_LEVEL");
    if (raw == nullptr || *raw == '\0') return false;
    out = raw;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return true;
}

static bool apply_log_level(const std::string& lvl, std::string& err) {
    if (lvl == "debug") forger_set_log_level(LogLevel::Debug);
    else if (lvl == "info") forger_set_log_level(LogLevel::Info);
    else if (lvl == "warn") forger_set_log_level(LogLevel::Warn);
    else if (lvl == "error") forger_set_log_level(LogLevel::Error);
    else {
        err = "unknown log level '" + lvl + "'";
        return false;
    }
    return true;
}

// Two-decimal probability formatting for the startup/scheduler lines.
static std::string format_prob(double p) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << p;
    return os.str();
}

// ============================================================================
// [3] SCHEDULER LOOP (daemon timing only; tick pipeline lives in forger/engine)
// ============================================================================

// Daemon loop: waits on the shutdown condition_variable (forger/shutdown.hpp)
// until the next tick or shutdown, whichever comes first -- a signal ends the
// wait immediately no matter how long interval_seconds is.
static void run_until_shutdown(forger::ChaosEngine& engine, const forger::Config& cfg) {
    if (shutting_down()) return;
    LOG_INFO("Scheduler started: interval=" + std::to_string(cfg.interval_seconds) +
             "s, chaos_probability=" + format_prob(cfg.chaos_probability) +
             (engine.discover_only()
                  ? ", DISCOVERY-ONLY MODE (no dice, no strikes)"
                  : (engine.dry_run() ? ", DRY-RUN MODE (no strikes will be issued)"
                                      : ", *** LIVE MODE: real stop/kill against " +
                                            cfg.docker_socket + " ***")));
    auto next_tick = Clock::now();
    while (!shutting_down()) {
        if (Clock::now() >= next_tick) {
            engine.tick();
            next_tick += std::chrono::seconds(cfg.interval_seconds);
            if (Clock::now() >= next_tick) {
                // We stalled (slow API calls): reset rather than burst.
                next_tick = Clock::now();
            }
            continue;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - Clock::now());
        g_shutdown.wait_for_stop(static_cast<int>(remaining.count()));
    }
}

// ============================================================================
// [4] MAIN
// ============================================================================

static void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "Forger %s -- local Docker chaos daemon\n"
                 "\n"
                 "Usage: %s [options]\n"
                 "\n"
                 "Options:\n"
                 "  -c, --config <path>   path to config.json (default: ./config.json)\n"
                 "  -o, --once            run a single tick and exit\n"
                 "  -n, --dry-run         log strikes without executing them\n"
                 "  -d, --discover        list running containers, log rule matches, exit\n"
                 "  -h, --help            show this help\n"
                 "  -v, --version         print version\n",
                 FORGER_VERSION, argv0);
}

// === MAIN_FUNCTION_BELOW ===

int main(int argc, char** argv) {
    std::string config_path = "config.json";
    bool once = false;
    bool dry_run_flag = false;
    bool discover_flag = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* opt, std::string& out) -> bool {
            if (i + 1 >= argc) {
                LOG_ERROR(std::string(opt) + " requires a value");
                return false;
            }
            out = argv[++i];
            return true;
        };
        if (arg == "-c" || arg == "--config") {
            if (!need_value("--config", config_path)) return 1;
        } else if (arg == "-o" || arg == "--once") {
            once = true;
        } else if (arg == "-n" || arg == "--dry-run") {
            dry_run_flag = true;
        } else if (arg == "-d" || arg == "--discover") {
            discover_flag = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            std::printf("Forger %s\n", FORGER_VERSION);
            return 0;
        } else {
            LOG_ERROR("unknown argument '" + arg + "'");
            print_usage(argv[0]);
            return 1;
        }
    }

    // ---- Configuration (fail fast: never run misconfigured) -----------------
    forger::Config cfg;
    std::string err;
    if (!forger::load_config(config_path, cfg, err)) {
        LOG_ERROR("configuration error: " + err);
        return 1;
    }
    if (dry_run_flag) cfg.dry_run = true;  // CLI flag overrides config

    // Log level: FORGER_LOG_LEVEL (if set) beats config's log_level; a bad
    // value must fail fast, never run at the wrong verbosity.
    bool level_from_env = false;
    std::string env_level;
    if (log_level_from_env(env_level)) {
        if (!apply_log_level(env_level, err)) {
            LOG_ERROR("configuration error: FORGER_LOG_LEVEL: " + err);
            return 1;
        }
        cfg.log_level = env_level;
        level_from_env = true;
    } else if (!apply_log_level(cfg.log_level, err)) {
        LOG_ERROR("configuration error: " + err);
        return 1;
    }

    // ---- Socket presence check (clear diagnostic before touching syscalls) --
    struct stat sock_stat{};
    if (::stat(cfg.docker_socket.c_str(), &sock_stat) != 0) {
        LOG_ERROR("docker socket not found at '" + cfg.docker_socket + "': " +
                  std::strerror(errno));
        LOG_ERROR("is the Docker daemon running? (WSL2: sudo service docker start)");
        return 1;
    }
    if (!S_ISSOCK(sock_stat.st_mode)) {
        LOG_ERROR("'" + cfg.docker_socket + "' exists but is not a socket");
        return 1;
    }

    // ---- Signals -------------------------------------------------------------
    if (!install_signal_handlers(err)) {
        LOG_ERROR("startup error: " + err);
        return 1;
    }

    LOG_INFO("Forger starting");

    // ---- Docker reachability probe ------------------------------------------
    LOG_INFO("Connecting to Docker");
    forger::DockerClient docker(forger::DockerClient::unix_socket_factory(cfg.docker_socket));
    if (!docker.ping(err)) {
        LOG_ERROR("docker engine unreachable: " + err);
        return 1;
    }
    LOG_INFO("Connected to Docker");

    // ---- Engine wiring: real client + shared shutdown state -----------------
    forger::ChaosEngine::Deps deps;
    deps.docker = &docker;
    deps.shutdown = &g_shutdown;
    deps.discover_only = discover_flag;
    const std::optional<bool> dry_run_override =
        dry_run_flag ? std::optional<bool>(true) : std::nullopt;
    forger::ChaosEngine engine(deps, cfg, dry_run_override);

    LOG_INFO("Forger " FORGER_VERSION ": config='" + config_path +
             "' socket='" + cfg.docker_socket + "' api=" + forger::kDockerApiVersion +
             " interval=" + std::to_string(cfg.interval_seconds) + "s" +
             " chaos_probability=" + format_prob(cfg.chaos_probability) +
             " targets=" + std::to_string(cfg.targets.size()) +
             " log=" + cfg.log_level + (level_from_env ? " (env)" : " (config)"));
    for (const forger::TargetRule& rule : cfg.targets) {
        LOG_INFO("  rule: name~='" + rule.name_match + "' -> " +
                 std::string(forger::action_name(rule.action)));
    }

    // ---- Startup safety warning (both --once and daemon modes) --------------
    if (!discover_flag && !engine.dry_run()) {
        LOG_WARN("SAFETY: LIVE CHAOS MODE - matching containers WILL be stopped or "
                 "killed. probability=" + format_prob(cfg.chaos_probability) +
                 " max/cycle=" + std::to_string(cfg.max_actions_per_cycle) +
                 (cfg.max_actions_per_run > 0
                      ? " max/run=" + std::to_string(cfg.max_actions_per_run)
                      : " max/run=unlimited"));
        LOG_WARN("SAFETY: if this is not intentional, stop now (Ctrl+C) and set "
                 "\"dry_run\": true in " + config_path);
    } else if (!discover_flag) {
        LOG_INFO("safety: dry-run mode - no strikes will be issued");
    }

    // ---- Main loop -----------------------------------------------------------
    if (once || discover_flag) {
        // One-shot modes: a single tick, then exit (--discover is implicitly
        // read-only: discovery only, no dice, no strikes).
        engine.tick();
    } else {
        spawn_signal_listener();
        run_until_shutdown(engine, cfg);
    }

    // ---- Shutdown: join the listener, release I/O, report, exit --------------
    const int signalled = g_signal_received.load(std::memory_order_acquire);
    if (signalled != 0) {
        LOG_INFO(std::string("Shutdown requested (signal ") +
                 (signalled == SIGINT ? "SIGINT" : "SIGTERM") + ")");
    }
    join_signal_listener();
    // Connection-per-request design: the Engine connection used by the last
    // tick was closed inside HttpClient::request; `docker` is destroyed on
    // return, releasing anything left.
    LOG_DEBUG("Docker connections closed");
    LOG_INFO("Forger stopped | rolls=" + std::to_string(engine.rolls()) +
             " strikes=" + std::to_string(engine.strikes()) +
             " errors=" + std::to_string(engine.errors()) +
             " | reason=" + forger::stop_reason_name(g_shutdown.reason()));
    return 0;
}
