# Forger

Forger is a lightweight chaos-engineering daemon for local Docker. It runs on
your Linux host (or WSL2), periodically lists running containers, matches them
against name rules you configure, and — on a probabilistic "dice roll" — stops
or kills matching containers.

It talks to the Docker Engine directly over the UNIX domain socket
(`/var/run/docker.sock`) with a hand-rolled HTTP/1.1 client. No Docker SDK, no
libcurl, no Boost — the only dependencies are a C++17 compiler, CMake, and
pthreads.

> **Safety first:** Forger ships in **dry-run mode**. Real destruction requires
> an explicit `"dry_run": false` in the config, and every run prints the
> blast-radius settings before acting.

## Features

- **Direct Engine communication** — raw HTTP/1.1 over the Docker UNIX socket;
  no Docker SDK, libcurl, or Boost (POSIX sockets only)
- **Container discovery** — queries `GET /v1.41/containers/json` for running
  containers; names are normalized (leading `/` stripped) before matching
- **Name matching** — case-sensitive substring match over container *names
  only*; first matching rule wins
- **Probability gate** — `chaos_probability` evaluated once per scheduler
  cycle (0.0 = never, 1.0 = always, 0.6 ≈ 60% of ticks)
- **Actions** — `stop` (graceful, configurable timeout) and `kill` (SIGKILL)
- **Safety controls** — dry-run default, per-cycle and per-run blast-radius
  caps, live-mode warnings, IDs validated as 64-hex before use
- **Clean shutdown** — SIGINT/SIGTERM handled via a `sigwait()` listener and a
  condition variable: shutdown is prompt even with a long interval, and no new
  strikes start once shutdown begins
- **Structured logging** — `[YYYY-MM-DD HH:MM:SS] [LEVEL] message`, with
  DEBUG/INFO on stdout and WARN/ERROR on stderr; CRLF in messages is folded to
  spaces so container names can't forge log lines
- **Bounded resource use** — response/header/chunk size limits, socket
  timeouts, and a recursion-capped JSON parser; a misbehaving Engine cannot
  make Forger allocate without bound
- **Testable** — 44k+ unit assertions and a mock Engine, no Docker needed; a
  separate gated integration test uses disposable real containers

## Architecture

```text
                    +------------------------------------------+
                    |                main.cpp                  |
                    |  CLI parsing, signal listener (sigwait), |
                    |  scheduler loop (condition-variable wait)|
                    +--------------------+---------------------+
                                         |
                                         v
+------------------+    rules   +---------------------------------+
|   config.json    +----------->|       forger (static lib)       |
+------------------+            |                                 |
                                |  config   JSON parse + validate |
+------------------+            |  discovery  name matching       |
|  Docker Engine   |<---HTTP--->|  http_socket  HTTP/1.1 client   |
|  /var/run/       |  over UNIX |  docker_api   Engine API v1.41  |
|  docker.sock     |    socket  |  chaos   probability + budget   |
+------------------+            |  engine   one tick pipeline     |
                                |  shutdown  atomic stop state    |
                                +---------------------------------+
```

One tick of the engine is:

```text
discover --> match --> roll dice --> budget --> strike --> log
 (HTTP)     (names)    (p=0.6?)    (caps)    (stop/kill)
```

## How It Works

1. **Load config** and validate it (fail fast; a bad config never reaches the
   engine). `FORGER_LOG_LEVEL` overrides the configured log level.
2. **Check the socket** exists and is a socket file, then ping the Engine.
3. **Every `interval_seconds`**, tick:
   - `GET /v1.41/containers/json?all=false` (running containers only)
   - normalize names (`/web-app` → `web-app`) and match against `targets`
   - roll once: `uniform_real_distribution[0,1) < chaos_probability`
   - if fired, strike up to `max_actions_per_cycle` armed containers (and
     never past the `max_actions_per_run` lifetime budget)
   - `stop` → `POST /v1.41/containers/{id}/stop?t=<timeout>`;
     `kill` → `POST /v1.41/containers/{id}/kill?signal=SIGKILL`
4. **Sleep until the next tick or shutdown**, whichever comes first — a
   signal wakes the daemon immediately via the condition variable.

Probability is evaluated **once per cycle, not per container**: a fired cycle
strikes all armed containers (subject to budgets), an unfired cycle strikes
none.

## Requirements

- Linux or WSL2 (uses `pthread_sigmask`, `localtime_r`, AF_UNIX)
- GCC or Clang with C++17 support
- CMake ≥ 3.10, Make, POSIX threads
- Docker Engine with the UNIX socket enabled (for actually running Forger;
  unit tests do not need Docker)
- Python 3 (only for the mock Engine / integration tooling)

## Installation

```bash
git clone <your-fork-url> forger
cd forger
```

Or grab a release tarball and unpack it. There is nothing to install beyond
the build; `make install` puts the binary and a sample config where you want
them (see [Building](#building)).

## Building

```bash
cmake -S . -B build
cmake --build build
```

The daemon lands at `build/Forger`. The default configuration is Release.

Build types:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
```

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `FORGER_BUILD_TESTS` | `ON` | build unit tests (run with ctest) |
| `FORGER_ENABLE_ASAN` | `OFF` | AddressSanitizer (forces Debug) |
| `FORGER_ENABLE_UBSAN` | `OFF` | UBSan (forces Debug) |

Sanitizer builds are for development only:

```bash
cmake -S . -B build-asan -DFORGER_ENABLE_ASAN=ON && cmake --build build-asan
ctest --test-dir build-asan
```

Install (binary + sample config):

```bash
sudo cmake --install build --prefix /usr/local
# /usr/local/bin/Forger and /usr/local/etc/forger.json
```

## Configuration

Forger reads a JSON config (default path `./config.json`, override with
`--config`):

```json
{
  "interval_seconds": 5,
  "chaos_probability": 0.6,
  "docker_socket": "/var/run/docker.sock",
  "stop_timeout_seconds": 10,
  "dry_run": true,
  "max_actions_per_cycle": 1,
  "max_actions_per_run": 10,
  "log_level": "info",
  "targets": [
    { "name_match": "forger-web",   "action": "stop" },
    { "name_match": "forger-cache", "action": "kill" }
  ]
}
```

If `dry_run` is omitted it defaults to **true** with a warning — running
live requires writing `"dry_run": false` yourself.

## Configuration Reference

| Key | Type | Default | Meaning |
|---|---|---|---|
| `interval_seconds` | int 1–86400 | `5` | seconds between ticks |
| `chaos_probability` | float 0.0–1.0 | `0.3` | chance the dice fire each tick |
| `docker_socket` | string | `/var/run/docker.sock` | Engine socket path |
| `stop_timeout_seconds` | int 1–600 | `10` | grace period for `stop` |
| `dry_run` | bool | **`true`** | never issue strikes (live requires explicit `false`) |
| `max_actions_per_cycle` | int 1–1000 | `1` | max strikes per tick |
| `max_actions_per_run` | int 0–1e6 (`0` = ∞) | `0` | process-lifetime strike budget |
| `log_level` | string | `info` | `debug`, `info`, `warn`, `error` |
| `targets` | array | — *(required)* | rules: `name_match` + `action` |

Notes:

- Matching is **case-sensitive substring over container names only**:
  `web-app` matches `/web-app`, `/my-web-app`, `/web-app-production`; it does
  **not** match a container merely because its *image* is `web-app:latest`.
- A container matching several rules is armed by the **first** matching rule.
- Unknown config keys and duplicate rules produce warnings (typo protection).
- Invalid values fail startup with the offending value in the message, e.g.
  `'chaos_probability' must be a number in [0.0, 1.0] (got 1.5)`.

## Dry Run

Dry-run is the default. The full pipeline runs — discovery, matching, dice —
but no stop/kill request is ever sent. Instead you get lines like:

```text
[2026-09-13 12:00:06] [WARN] Dry-run: would stop web-app
```

You can also force it from the CLI regardless of config:

```bash
./build/Forger --once --dry-run
```

Go-live sequence: rehearse in dry-run → set a finite `max_actions_per_run` →
flip `"dry_run": false` → watch the first ticks live.

## Running Forger

```bash
# 1. Spin up disposable dummy targets (never point Forger at anything precious)
docker run -d --name forger-web   nginx:alpine
docker run -d --name forger-cache redis:alpine

# 2. Rehearse: one tick, dry-run
./build/Forger --once --dry-run

# 3. Daemon mode (Ctrl+C for prompt clean shutdown)
./build/Forger

# 4. Inspect-only: list matches and exit, never rolls or strikes
./build/Forger --discover
```

CLI options:

| Option | Description |
|---|---|
| `-c, --config <path>` | config file (default `./config.json`) |
| `-o, --once` | run a single tick and exit |
| `-n, --dry-run` | log strikes without executing them |
| `-d, --discover` | one discovery tick with rule matches, then exit |
| `-h, --help` | usage |
| `-v, --version` | version |

Log levels and streams: DEBUG/INFO → stdout, WARN/ERROR → stderr. Raise
verbosity without touching config: `FORGER_LOG_LEVEL=debug ./build/Forger`.

A sample run (dry-run, one armed container):

```text
[2026-09-13 12:00:01] [INFO] Forger starting
[2026-09-13 12:00:01] [INFO] Connecting to Docker
[2026-09-13 12:00:01] [INFO] Connected to Docker
[2026-09-13 12:00:06] [INFO] Matched container forger-web-1
[2026-09-13 12:00:06] [INFO] dice roll: 0.38 < 1.00 -> CHAOS FIRES
[2026-09-13 12:00:06] [WARN] Dry-run: would stop forger-web-1 (aaaaaaaaaaaa)
```

## Testing

Unit tests need no Docker daemon:

```bash
ctest --test-dir build --output-on-failure
```

| Suite | Covers |
|---|---|
| `forger_http_tests` | HTTP parser framing, Content-Length/chunked/EOF, malformed responses, Docker list parsing |
| `forger_config_tests` | invalid JSON, missing fields, bad probability/interval/action, empty `name_match` |
| `forger_discovery_tests` | name normalization, case-sensitive substring matching, names-only policy |
| `forger_chaos_tests` | probability distribution, roller determinism |
| `forger_shutdown_tests` | shutdown transitions, first-reason-wins, prompt CV wake |
| `forger_engine_tests` | dry-run, blast-radius budgets, strike dispatch, fault isolation, shutdown guards |

All tests build from `FORGER_BUILD_TESTS=ON` (the default).

### Mock Engine (no Docker)

`scripts/mock_test.py` fakes a Docker Engine on `/tmp/forger-test.sock` and
logs every strike POST it receives:

```bash
python3 scripts/mock_test.py &
sleep 0.5
sed 's#/var/run/docker.sock#/tmp/forger-test.sock#' config.json > /tmp/forger-smoke.json
./build/Forger --once --dry-run --config /tmp/forger-smoke.json
kill %1
cat /tmp/forger-post.log   # strike requests the mock received
```

## Integration Testing

`scripts/integration_test.sh` exercises Forger against **real** Docker with
**disposable containers**. It is *not* part of a plain `ctest` run: it exits
77 (reported as skipped) unless `FORGER_IT_LIVE=1` is set, so normal test runs
never touch your Docker host.

What it does:

1. Creates `forger-test-web` (nginx:alpine), `forger-test-database`
   (redis:alpine), and `forger-test-inert`, all labeled
   `forger-integration-test`
2. Verifies discovery + matching (positive and negative)
3. Rehearses in dry-run (nothing stopped)
4. Performs a real `stop` of `forger-test-web` and verifies state `exited`
   while the others stay `running`
5. Optionally performs `kill` of `forger-test-database` (exit code 137) with
   `FORGER_IT_KILL=1`
6. Removes every test container on exit — even on failure — via a trap

Only containers named `forger-test-*` *and* labeled
`forger-integration-test` are ever created or removed; a pre-flight guard
refuses to run if a `forger-test-*` container exists without the label.

```bash
# Preview the plan (creates nothing, exits 77)
bash scripts/integration_test.sh

# Full rehearsal
FORGER_IT_LIVE=1 bash scripts/integration_test.sh

# Include the kill phase
FORGER_IT_LIVE=1 FORGER_IT_KILL=1 bash scripts/integration_test.sh

# Or through ctest
FORGER_IT_LIVE=1 ctest --test-dir build -R forger_integration_tests --output-on-failure
```

| Variable | Default | Meaning |
|---|---|---|
| `FORGER_IT_LIVE` | unset | **required** to create/strike anything |
| `FORGER_IT_KILL` | unset | additionally test `kill` (disposable db only) |
| `FORGER_IT_KEEP` | unset | keep containers after the run |
| `FORGER_IT_DOCKER_SOCK` | `/var/run/docker.sock` | socket handed to Forger |
| `FORGER_IT_IMAGE_WEB` / `FORGER_IT_IMAGE_DB` | `nginx:alpine` / `redis:alpine` | disposable images |
| `FORGER_BIN` | `build/Forger` | Forger binary under test |

## Security Considerations

**Access to `/var/run/docker.sock` is effectively root on the host.** Any
process that can talk to that socket can start privileged containers, mount
the host filesystem, and escape the container sandbox entirely. Forger holds
that power while it runs — treat it (and its config) accordingly:

- Run Forger as a **non-root user in the `docker` group**, never as root if
  you can avoid it. If you would not give that user `sudo`, think twice.
- **Config file permissions matter.** `config.json` decides what gets struck.
  Keep it owned and writable only by the Forger user; a world-writable config
  is a remote-controlled destruction switch.
- No shell is ever invoked: there is no `system()`, `popen()`, `exec*()`, or
  `fork()` in the codebase. Container IDs are validated as exactly 64 hex
  chars before being placed in request paths, so config strings can never
  become requests or commands.
- A hostile **local** Engine response is bounded (16 MB body cap, 64 KB
  header cap, 5 s socket timeout, recursion-capped JSON parser). But the
  Engine itself is inside the trust boundary: a compromised daemon on the
  host has far easier routes than Forger's parser.
- Log forging is mitigated: CR/LF bytes in log messages are folded to spaces,
  so container names or Engine error bodies cannot fabricate `[LEVEL]` lines.
- Randomness uses `std::mt19937_64` seeded from `std::random_device`; the
  dice are not a security mechanism, but they are not guessable via `rand()`.

Forger is **not claimed to be secure** — see [Limitations](#limitations).

## Limitations

- **No privilege separation**: Forger runs with whatever the socket grants.
- **Linux-only** by design (AF_UNIX, `pthread_sigmask`, `localtime_r`).
- **One request per connection** (`Connection: close`); no keep-alive,
  pipelining, TLS, redirects, compression, or streaming/attach endpoints.
- **No config reload**: `SIGHUP` is not handled; restart to re-read config.
- **Substring matching is blunt**: `web` matches `web-app` and `web-2`. Broad
  patterns are operator error Forger will not prevent — scope rules carefully.
- **`max_actions_per_run` resets on restart** (it is process-lifetime).
- **No authentication/authorization** on the config: whoever edits it chooses
  the targets.
- The strike budget is enforced per process; a second Forger instance doubles
  the blast radius.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `docker socket not found at '...'` | Start the daemon: `sudo service docker start` (WSL2). Check `docker context ls` if you use a non-default socket. |
| `permission denied while connecting` | Add yourself to the `docker` group and re-login: `sudo usermod -aG docker $USER` |
| No matches despite running containers | Check `FORGER_LOG_LEVEL=debug ./build/Forger --once --dry-run` to see names as seen and rules as armed |
| Strikes fire too often | Lower `chaos_probability`, or cap the blast: `max_actions_per_cycle`, `max_actions_per_run` |
| Startup exits 1 with a config error | The message names the key and echoes the offending value; fix that key |
| Log lines missing | DEBUG/INFO go to stdout, WARN/ERROR to stderr — don't merge them when filtering |

## Development

```bash
# Configure + build + test (the everyday loop)
cmake -S . -B build && cmake --build build && ctest --test-dir build

# Warnings are part of the contract
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build

# Sanitizers
cmake -S . -B build-asan -DFORGER_ENABLE_ASAN=ON && cmake --build build-asan && ctest --test-dir build-asan
cmake -S . -B build-ubsan -DFORGER_ENABLE_UBSAN=ON && cmake --build build-ubsan && ctest --test-dir build-ubsan

# Mock-engine loop without Docker
python3 scripts/mock_test.py & sleep 0.5
sed 's#/var/run/docker.sock#/tmp/forger-test.sock#' config.json > /tmp/forger-smoke.json
./build/Forger --once --dry-run --config /tmp/forger-smoke.json && kill %1
```

Layout:

```text
CMakeLists.txt        build: forger static lib + Forger daemon + tests
main.cpp              daemon shell: CLI, signals, scheduler loop
config.json           reference configuration (dry-run by default)
include/forger/       library headers (log, json, http_socket, docker_api,
                      config, discovery, chaos, shutdown, engine)
src/                  library sources (HTTP/1.1 over UNIX socket, Docker API
                      v1.41, config, matching, engine, shutdown)
tests/                dependency-free assert suites run via ctest
scripts/mock_test.py  mock Docker daemon for tests without Docker
scripts/integration_test.sh gated end-to-end test on disposable containers
```

Code maps to layers one-to-one: `http_socket` (transport + parser) →
`docker_api` (Engine endpoints, ID validation) → `discovery` (matching) →
`chaos` (probability + budget) → `engine` (tick pipeline) → `main.cpp`
(signals + scheduler). Each layer is unit-tested through interfaces
(`Transport`, `IDockerClient`, `ShutdownState`), so no test needs a daemon.

## License

MIT — see [LICENSE](LICENSE).
