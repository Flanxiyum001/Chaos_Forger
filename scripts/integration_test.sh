#!/usr/bin/env bash
# ============================================================================
#  scripts/integration_test.sh — safe Forger integration test
#
#  Exercises Forger against REAL Docker with DISPOSABLE containers only:
#
#    1. start disposable test containers (labeled, clearly named)
#    2. run Forger against them via its normal config file
#    3. verify container discovery
#    4. verify name matching (positive + negative)
#    5. rehearse in dry-run mode first (nothing stopped)
#    6. perform a real "stop" against the disposable web container
#    7. verify the container actually stopped
#    8. clean up every test container — even when the test fails (EXIT trap)
#
#  Safety model:
#    - Only containers named forger-test-* AND labeled forger-integration-test
#      are ever created, struck, or removed. Anything else is refused.
#    - Everything is opt-in: without FORGER_IT_LIVE=1 the script creates
#      nothing and exits 77 (ctest SKIP_RETURN_CODE), so plain `ctest` never
#      mutates your Docker host.
#    - "kill" additionally requires FORGER_IT_KILL=1 and only ever targets the
#      disposable forger-test-database container.
#    - A pre-flight guard refuses to run if a forger-test-* container exists
#      WITHOUT our label (never touches something we did not create).
#
#  Usage:
#     bash scripts/integration_test.sh                 # safety preview (no-op)
#     FORGER_IT_LIVE=1 bash scripts/integration_test.sh
#     FORGER_IT_LIVE=1 FORGER_IT_KILL=1 bash scripts/integration_test.sh
#
#  Environment knobs:
#     FORGER_IT_LIVE=1          enable the destructive phases (required)
#     FORGER_IT_KILL=1          additionally test kill (disposable db only)
#     FORGER_IT_KEEP=1          keep containers after the run (for debugging)
#     FORGER_IT_DOCKER_SOCK     Engine socket for Forger (default
#                               /var/run/docker.sock)
#     FORGER_IT_IMAGE_WEB       web image      (default nginx:alpine)
#     FORGER_IT_IMAGE_DB        database image (default redis:alpine)
#     FORGER_BIN                path to the Forger binary
#                               (default <repo>/build/Forger)
# ============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Identity + paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

WEB_NAME="forger-test-web"
DB_NAME="forger-test-database"
INERT_NAME="forger-test-inert"     # proves matching is per-rule, not prefix-wide
LABEL="forger-integration-test"

WEB_IMAGE="${FORGER_IT_IMAGE_WEB:-nginx:alpine}"
DB_IMAGE="${FORGER_IT_IMAGE_DB:-redis:alpine}"
DOCKER_SOCK="${FORGER_IT_DOCKER_SOCK:-/var/run/docker.sock}"
KEEP="${FORGER_IT_KEEP:-0}"

FORGER_BIN="${FORGER_BIN:-$ROOT/build/Forger}"
# --forger-bin PATH (used by ctest) overrides the default binary location
while [ $# -gt 0 ]; do
    case "$1" in
        --forger-bin) FORGER_BIN="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

WORK="$(mktemp -d)"
FULL_CFG="$WORK/full.json"
STOP_CFG="$WORK/stop.json"
KILL_CFG="$WORK/kill.json"
FORGER_LOG="$WORK/forger.log"

note() { echo "[it] $*"; }
fail() {
    echo "[it] FAIL: $*" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# Cleanup on any exit path — success, assertion failure, or Ctrl+C.
# Removes ONLY containers that carry our label AND the forger-test- name
# prefix; everything else on the host is untouchable.
# ---------------------------------------------------------------------------
cleanup() {
    rc=$?
    trap - EXIT INT TERM
    if [ "$rc" -ne 0 ] && [ -f "$FORGER_LOG" ]; then
        echo "[it] ---- last Forger output ----" >&2
        tail -30 "$FORGER_LOG" >&2 || true
    fi
    if [ "$KEEP" != "1" ]; then
        # Intentionally unquoted $ids: whitespace-separated container IDs.
        local ids
        ids="$(docker ps -aq --filter "label=$LABEL" \
                                --filter "name=forger-test-" 2>/dev/null || true)"
        if [ -n "$ids" ]; then
            echo "[it] cleanup: removing leftover test containers:$ids" >&2
            # shellcheck disable=SC2086
            docker rm -f $ids >/dev/null 2>&1 || true
        fi
    else
        echo "[it] FORGER_IT_KEEP=1: leaving test containers in place" >&2
    fi
    if [ -n "$WORK" ] && [ -d "$WORK" ]; then
        rm -rf "$WORK"
    fi
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ---------------------------------------------------------------------------
# Assertion helpers
# ---------------------------------------------------------------------------
container_status() { docker inspect -f '{{.State.Status}}' "$1" 2>/dev/null; }

expect_status() {  # expect_status <name> <expected>
    local got
    got="$(container_status "$1")" \
        || fail "container '$1' disappeared during the test"
    [ "$got" = "$2" ] \
        || fail "container '$1': expected state '$2', got '$got'"
}

expect_log() {  # expect_log <fixed-string>
    grep -qF -- "$1" "$FORGER_LOG" \
        || fail "expected Forger log line not found: '$1'"
}

refuse_log() {  # refuse_log <fixed-string>
    if grep -qF -- "$1" "$FORGER_LOG"; then
        fail "forbidden Forger log line present: '$1'"
    fi
}

# write_config <file> <dry_run> <targets-json-array>
write_config() {
    cat > "$1" <<EOF
{
  "interval_seconds": 60,
  "chaos_probability": 1.0,
  "docker_socket": "$DOCKER_SOCK",
  "stop_timeout_seconds": 2,
  "dry_run": $2,
  "max_actions_per_cycle": 1,
  "max_actions_per_run": 0,
  "log_level": "info",
  "targets": $3
}
EOF
}

# run_forger <label> <config> [extra Forger args...]
run_forger() {
    local label="$1" cfg="$2"
    shift 2
    note "running Forger ($label)"
    if ! timeout 120 "$FORGER_BIN" --config "$cfg" "$@" >"$FORGER_LOG" 2>&1; then
        fail "Forger exited nonzero ($label); config: $cfg"
    fi
}

# ---------------------------------------------------------------------------
# Phase 0 — preconditions (fail fast BEFORE creating anything)
# ---------------------------------------------------------------------------
note "forger integration test"
note "forger binary : $FORGER_BIN"
note "engine socket : $DOCKER_SOCK"

command -v docker >/dev/null 2>&1 \
    || { echo "SKIP: docker CLI not available" ; exit 77 ; }
command -v timeout >/dev/null 2>&1 || fail "coreutils 'timeout' is required"
docker info >/dev/null 2>&1 \
    || { echo "SKIP: docker daemon not reachable" ; exit 77 ; }
[ -S "$DOCKER_SOCK" ] \
    || { echo "SKIP: Forger socket '$DOCKER_SOCK' is not a socket file" ; exit 77 ; }
[ -x "$FORGER_BIN" ] \
    || fail "Forger binary not found or not executable: $FORGER_BIN (build first: cmake --build build)"

# Collision guard: if a forger-test-* container exists that we did NOT label,
# it may belong to someone else — refuse rather than touch it.
for n in "$WEB_NAME" "$DB_NAME" "$INERT_NAME"; do
    if docker inspect "$n" >/dev/null 2>&1; then
        lab="$(docker inspect -f "{{index .Config.Labels \"$LABEL\"}}" "$n" 2>/dev/null || true)"
        [ "$lab" = "1" ] \
            || fail "container '$n' exists without our label '$LABEL' — refusing to touch it; remove it manually or rename the test containers"
        note "removing stale test container from a previous run: $n"
        docker rm -f "$n" >/dev/null
    fi
done

# ---------------------------------------------------------------------------
# Safety gate — EVERY phase below creates/strikes containers. Plain `ctest`
# (or a bare run of this script) must never do that silently.
# ---------------------------------------------------------------------------
if [ "${FORGER_IT_LIVE:-0}" != "1" ]; then
    echo "SKIP: destructive integration test not enabled."
    echo "      Review scripts/integration_test.sh, then run:"
    echo "        FORGER_IT_LIVE=1 bash scripts/integration_test.sh"
    echo "      (add FORGER_IT_KILL=1 to also test kill)"
    exit 77
fi

# ---------------------------------------------------------------------------
# Phase 1 — start disposable test containers (labeled = ours, and only ours)
# ---------------------------------------------------------------------------
note "phase 1: creating disposable containers"
docker run -d --name "$WEB_NAME"    --label "$LABEL" "$WEB_IMAGE" >/dev/null
docker run -d --name "$DB_NAME"     --label "$LABEL" "$DB_IMAGE"  >/dev/null
docker run -d --name "$INERT_NAME"  --label "$LABEL" "$WEB_IMAGE" >/dev/null

# ---------------------------------------------------------------------------
# Phase 2+3 — discovery & name matching (positive AND negative)
# ---------------------------------------------------------------------------
note "phase 2+3: discovery and name matching (--discover, no dice, no strikes)"
write_config "$FULL_CFG" true "[ { \"name_match\": \"$WEB_NAME\", \"action\": \"stop\" }, { \"name_match\": \"$DB_NAME\", \"action\": \"kill\" } ]"
run_forger "discovery" "$FULL_CFG" --discover
expect_log "Matched container $WEB_NAME"
expect_log "Matched container $DB_NAME"
refuse_log "Matched container $INERT_NAME"   # must not match: no rule for it
expect_status "$WEB_NAME"   running          # --discover never strikes
expect_status "$DB_NAME"    running
expect_status "$INERT_NAME" running

# ---------------------------------------------------------------------------
# Phase 4+5 — dry-run rehearsal: identical pipeline, zero strikes
# ---------------------------------------------------------------------------
note "phase 4+5: dry-run rehearsal (would-stop logged, nothing executed)"
write_config "$STOP_CFG" true "[ { \"name_match\": \"$WEB_NAME\", \"action\": \"stop\" } ]"
run_forger "dry-run" "$STOP_CFG" --once
expect_log "Matched container $WEB_NAME"
expect_log "Dry-run: would stop $WEB_NAME"
refuse_log "strike ok"
refuse_log "SAFETY: LIVE CHAOS MODE"
expect_status "$WEB_NAME"   running          # still alive after rehearsal
expect_status "$DB_NAME"    running
expect_status "$INERT_NAME" running

# ---------------------------------------------------------------------------
# Phase 6+7 — real stop against the disposable web container, then verify
# ---------------------------------------------------------------------------
note "phase 6: LIVE stop of $WEB_NAME (disposable)"
write_config "$STOP_CFG" false "[ { \"name_match\": \"$WEB_NAME\", \"action\": \"stop\" } ]"
run_forger "live stop" "$STOP_CFG" --once
expect_log "SAFETY: LIVE CHAOS MODE"
expect_log "strike ok: action=stop target=$WEB_NAME"

note "phase 7: verifying container state"
expect_status "$WEB_NAME" exited              # actually stopped
expect_status "$DB_NAME"  running             # NOT armed by this config
expect_status "$INERT_NAME" running           # blast radius stayed at 1

# ---------------------------------------------------------------------------
# Phase 6b — kill, only against the disposable database, only when asked
# ---------------------------------------------------------------------------
if [ "${FORGER_IT_KILL:-0}" = "1" ]; then
    note "phase 6b: LIVE kill of $DB_NAME (disposable, FORGER_IT_KILL=1)"
    write_config "$KILL_CFG" false "[ { \"name_match\": \"$DB_NAME\", \"action\": \"kill\" } ]"
    run_forger "live kill" "$KILL_CFG" --once
    expect_log "strike ok: action=kill target=$DB_NAME"
    expect_status "$DB_NAME" exited
    ec="$(docker inspect -f '{{.State.ExitCode}}' "$DB_NAME")"
    [ "$ec" = "137" ] || fail "kill: expected ExitCode 137 (SIGKILL), got $ec"
else
    note "phase 6b: kill phase skipped (set FORGER_IT_KILL=1 to include it)"
fi

# ---------------------------------------------------------------------------
# Phase 8 — cleanup is handled by the EXIT trap (runs on success, failure,
# or interrupt); only tell the operator what will be removed.
# ---------------------------------------------------------------------------
note "phase 8: cleanup is performed by the exit trap for: $(docker ps -aq --filter "label=$LABEL" --filter "name=forger-test-" | tr '\n' ' ')"

if [ "$KEEP" = "1" ]; then
    note "FORGER_IT_KEEP=1: containers kept for inspection (remove with: docker rm -f \$(docker ps -aq --filter label=$LABEL))"
fi
note "PASS: discovery, matching, dry-run, live stop$( [ "${FORGER_IT_KILL:-0}" = "1" ] && echo " + kill" ), cleanup verified"
exit 0
