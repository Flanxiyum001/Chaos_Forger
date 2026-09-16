#!/usr/bin/env bash
# ============================================================================
#  scripts/integration_test.sh — safe Chaos_Forger integration test
#
#  Exercises Chaos_Forger against REAL Docker with DISPOSABLE containers only:
#
#    1. start disposable test containers (labeled, clearly named)
#    2. run Chaos_Forger against them via its normal config file
#    3. verify container discovery
#    4. verify name matching (positive + negative)
#    5. rehearse in dry-run mode first (nothing stopped)
#    6. perform a real "stop" against the disposable web container
#    7. verify the container actually stopped
#    8. clean up every test container — even when the test fails (EXIT trap)
#
#  Safety model:
#    - Only containers named Chaos_Forger-test-* AND labeled Chaos_Forger-integration-test
#      are ever created, struck, or removed. Anything else is refused.
#    - Everything is opt-in: without Chaos_Forger_IT_LIVE=1 the script creates
#      nothing and exits 77 (ctest SKIP_RETURN_CODE), so plain `ctest` never
#      mutates your Docker host.
#    - "kill" additionally requires Chaos_Forger_IT_KILL=1 and only ever targets the
#      disposable Chaos_Forger-test-database container.
#    - A pre-flight guard refuses to run if a Chaos_Forger-test-* container exists
#      WITHOUT our label (never touches something we did not create).
#
#  Usage:
#     bash scripts/integration_test.sh                 # safety preview (no-op)
#     Chaos_Forger_IT_LIVE=1 bash scripts/integration_test.sh
#     Chaos_Forger_IT_LIVE=1 Chaos_Forger_IT_KILL=1 bash scripts/integration_test.sh
#
#  Environment knobs:
#     Chaos_Forger_IT_LIVE=1          enable the destructive phases (required)
#     Chaos_Forger_IT_KILL=1          additionally test kill (disposable db only)
#     Chaos_Forger_IT_KEEP=1          keep containers after the run (for debugging)
#     Chaos_Forger_IT_DOCKER_SOCK     Engine socket for Chaos_Forger (default
#                               /var/run/docker.sock)
#     Chaos_Forger_IT_IMAGE_WEB       web image      (default nginx:alpine)
#     Chaos_Forger_IT_IMAGE_DB        database image (default redis:alpine)
#     Chaos_Forger_BIN                path to the Chaos_Forger binary
#                               (default <repo>/build/Chaos_Forger)
# ============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Identity + paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

WEB_NAME="Chaos_Forger-test-web"
DB_NAME="Chaos_Forger-test-database"
INERT_NAME="Chaos_Forger-test-inert"     # proves matching is per-rule, not prefix-wide
LABEL="Chaos_Forger-integration-test"

WEB_IMAGE="${Chaos_Forger_IT_IMAGE_WEB:-nginx:alpine}"
DB_IMAGE="${Chaos_Forger_IT_IMAGE_DB:-redis:alpine}"
DOCKER_SOCK="${Chaos_Forger_IT_DOCKER_SOCK:-/var/run/docker.sock}"
KEEP="${Chaos_Forger_IT_KEEP:-0}"

Chaos_Forger_BIN="${Chaos_Forger_BIN:-$ROOT/build/Chaos_Forger}"
# --Chaos_Forger-bin PATH (used by ctest) overrides the default binary location
while [ $# -gt 0 ]; do
    case "$1" in
        --Chaos_Forger-bin) Chaos_Forger_BIN="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

WORK="$(mktemp -d)"
FULL_CFG="$WORK/full.json"
STOP_CFG="$WORK/stop.json"
KILL_CFG="$WORK/kill.json"
Chaos_Forger_LOG="$WORK/Chaos_Forger.log"

note() { echo "[it] $*"; }
fail() {
    echo "[it] FAIL: $*" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# Cleanup on any exit path — success, assertion failure, or Ctrl+C.
# Removes ONLY containers that carry our label AND the Chaos_Forger-test- name
# prefix; everything else on the host is untouchable.
# ---------------------------------------------------------------------------
cleanup() {
    rc=$?
    trap - EXIT INT TERM
    if [ "$rc" -ne 0 ] && [ -f "$Chaos_Forger_LOG" ]; then
        echo "[it] ---- last Chaos_Forger output ----" >&2
        tail -30 "$Chaos_Forger_LOG" >&2 || true
    fi
    if [ "$KEEP" != "1" ]; then
        # Intentionally unquoted $ids: whitespace-separated container IDs.
        local ids
        ids="$(docker ps -aq --filter "label=$LABEL" \
                                --filter "name=Chaos_Forger-test-" 2>/dev/null || true)"
        if [ -n "$ids" ]; then
            echo "[it] cleanup: removing leftover test containers:$ids" >&2
            # shellcheck disable=SC2086
            docker rm -f $ids >/dev/null 2>&1 || true
        fi
    else
        echo "[it] Chaos_Forger_IT_KEEP=1: leaving test containers in place" >&2
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
    grep -qF -- "$1" "$Chaos_Forger_LOG" \
        || fail "expected Chaos_Forger log line not found: '$1'"
}

refuse_log() {  # refuse_log <fixed-string>
    if grep -qF -- "$1" "$Chaos_Forger_LOG"; then
        fail "forbidden Chaos_Forger log line present: '$1'"
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

# run_Chaos_Forger <label> <config> [extra Chaos_Forger args...]
run_Chaos_Forger() {
    local label="$1" cfg="$2"
    shift 2
    note "running Chaos_Forger ($label)"
    if ! timeout 120 "$Chaos_Forger_BIN" --config "$cfg" "$@" >"$Chaos_Forger_LOG" 2>&1; then
        fail "Chaos_Forger exited nonzero ($label); config: $cfg"
    fi
}

# ---------------------------------------------------------------------------
# Phase 0 — preconditions (fail fast BEFORE creating anything)
# ---------------------------------------------------------------------------
note "Chaos_Forger integration test"
note "Chaos_Forger binary : $Chaos_Forger_BIN"
note "engine socket : $DOCKER_SOCK"

command -v docker >/dev/null 2>&1 \
    || { echo "SKIP: docker CLI not available" ; exit 77 ; }
command -v timeout >/dev/null 2>&1 || fail "coreutils 'timeout' is required"
docker info >/dev/null 2>&1 \
    || { echo "SKIP: docker daemon not reachable" ; exit 77 ; }
[ -S "$DOCKER_SOCK" ] \
    || { echo "SKIP: Chaos_Forger socket '$DOCKER_SOCK' is not a socket file" ; exit 77 ; }
[ -x "$Chaos_Forger_BIN" ] \
    || fail "Chaos_Forger binary not found or not executable: $Chaos_Forger_BIN (build first: cmake --build build)"

# Collision guard: if a Chaos_Forger-test-* container exists that we did NOT label,
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
if [ "${Chaos_Forger_IT_LIVE:-0}" != "1" ]; then
    echo "SKIP: destructive integration test not enabled."
    echo "      Review scripts/integration_test.sh, then run:"
    echo "        Chaos_Forger_IT_LIVE=1 bash scripts/integration_test.sh"
    echo "      (add Chaos_Forger_IT_KILL=1 to also test kill)"
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
run_Chaos_Forger "discovery" "$FULL_CFG" --discover
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
run_Chaos_Forger "dry-run" "$STOP_CFG" --once
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
run_Chaos_Forger "live stop" "$STOP_CFG" --once
expect_log "SAFETY: LIVE CHAOS MODE"
expect_log "strike ok: action=stop target=$WEB_NAME"

note "phase 7: verifying container state"
expect_status "$WEB_NAME" exited              # actually stopped
expect_status "$DB_NAME"  running             # NOT armed by this config
expect_status "$INERT_NAME" running           # blast radius stayed at 1

# ---------------------------------------------------------------------------
# Phase 6b — kill, only against the disposable database, only when asked
# ---------------------------------------------------------------------------
if [ "${Chaos_Forger_IT_KILL:-0}" = "1" ]; then
    note "phase 6b: LIVE kill of $DB_NAME (disposable, Chaos_Forger_IT_KILL=1)"
    write_config "$KILL_CFG" false "[ { \"name_match\": \"$DB_NAME\", \"action\": \"kill\" } ]"
    run_Chaos_Forger "live kill" "$KILL_CFG" --once
    expect_log "strike ok: action=kill target=$DB_NAME"
    expect_status "$DB_NAME" exited
    ec="$(docker inspect -f '{{.State.ExitCode}}' "$DB_NAME")"
    [ "$ec" = "137" ] || fail "kill: expected ExitCode 137 (SIGKILL), got $ec"
else
    note "phase 6b: kill phase skipped (set Chaos_Forger_IT_KILL=1 to include it)"
fi

# ---------------------------------------------------------------------------
# Phase 8 — cleanup is handled by the EXIT trap (runs on success, failure,
# or interrupt); only tell the operator what will be removed.
# ---------------------------------------------------------------------------
note "phase 8: cleanup is performed by the exit trap for: $(docker ps -aq --filter "label=$LABEL" --filter "name=Chaos_Forger-test-" | tr '\n' ' ')"

if [ "$KEEP" = "1" ]; then
    note "Chaos_Forger_IT_KEEP=1: containers kept for inspection (remove with: docker rm -f \$(docker ps -aq --filter label=$LABEL))"
fi
note "PASS: discovery, matching, dry-run, live stop$( [ "${Chaos_Forger_IT_KILL:-0}" = "1" ] && echo " + kill" ), cleanup verified"
exit 0
