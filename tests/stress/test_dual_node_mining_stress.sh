#!/usr/bin/env bash
#
# Dual-Node Mining Stress Test
# 2 nodes × 20 miners - Fork Pressure & Reorg Stability
#
# Tests:
#   - Fork pressure: both nodes frequently find blocks at similar times
#   - Reorg stability: ActivateBestChain hammered under load
#   - Relay correctness: inv/getdata/block traffic under constant churn
#   - Concurrency safety: mempool, validation locks, peer manager locks
#
# Pass Criteria:
#   - Both nodes converge to same tip within timeout
#   - Height matches on both nodes
#   - Minimum height target reached
#   - No crashes, no deadlocks
#
# Usage:
#   ./test_dual_node_mining_stress.sh
#   TARGET_HEIGHT=500 MINERS_PER_NODE=10 ./test_dual_node_mining_stress.sh

set -euo pipefail

# ═══════════════════════════════════════════════════════════════════════════
# Configuration (overridable via environment)
# ═══════════════════════════════════════════════════════════════════════════

TARGET_HEIGHT=${TARGET_HEIGHT:-200}
CONVERGENCE_TIMEOUT=${CONVERGENCE_TIMEOUT:-90}   # Increased for fork pressure
SETTLE_TIME=${SETTLE_TIME:-10}                   # Wait for in-flight blocks
MINERS_PER_NODE=${MINERS_PER_NODE:-16}           # Max threads is 16 (default for both)
MINERS_A=${MINERS_A:-$MINERS_PER_NODE}           # Asymmetric: miners for Node A
MINERS_B=${MINERS_B:-$MINERS_PER_NODE}           # Asymmetric: miners for Node B
RATE_LIMIT_MS=${RATE_LIMIT_MS:-100}
STARTUP_WAIT=${STARTUP_WAIT:-8}
RPC_TIMEOUT=${RPC_TIMEOUT:-5}

# Ports (randomized to avoid conflicts)
BASE_PORT=$((19000 + RANDOM % 500))
NODE_A_RPC=$BASE_PORT
NODE_A_P2P=$((BASE_PORT + 1000))
NODE_B_RPC=$((BASE_PORT + 1))
NODE_B_P2P=$((BASE_PORT + 1001))

# Data directories (unique per run)
DATA_A="/tmp/dinero_stress_a_$$"
DATA_B="/tmp/dinero_stress_b_$$"

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DINEROD="${DINEROD:-$ROOT_DIR/build/dinerod}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# PIDs for cleanup
PID_A=""
PID_B=""

# ═══════════════════════════════════════════════════════════════════════════
# Utility Functions
# ═══════════════════════════════════════════════════════════════════════════

log_info() { echo -e "${BLUE}[INFO]${NC} $*"; }
log_pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
log_fail() { echo -e "${RED}[FAIL]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_step() { echo -e "${CYAN}[$1]${NC} $2"; }

cleanup() {
    # MUST be the first statement: in an EXIT trap $? is the script's exit
    # status only before any other command runs. Capturing it further down
    # (after the kills and sleeps below) yields the status of that sleep --
    # effectively always 0 -- and the failure-log preservation would silently
    # never fire.
    local exit_code=$?
    echo ""
    log_warn "Cleaning up test environment..."

    # Never let cleanup turn a passed stress cycle into a harness failure.
    set +e

    # Stop mining first (best effort)
    rpc_a "mining.stop" >/dev/null 2>&1 || true
    rpc_b "mining.stop" >/dev/null 2>&1 || true
    sleep 1

    # Kill processes
    if [ -n "$PID_A" ]; then
        kill "$PID_A" 2>/dev/null || true
    fi
    if [ -n "$PID_B" ]; then
        kill "$PID_B" 2>/dev/null || true
    fi

    # Also kill by pattern (fallback)
    pkill -f "dinerod.*$DATA_A" 2>/dev/null || true
    pkill -f "dinerod.*$DATA_B" 2>/dev/null || true

    # Give TERM a chance to shut the daemons down cleanly before escalating.
    for _ in $(seq 1 10); do
        local a_alive=0
        local b_alive=0
        pgrep -f "dinerod.*$DATA_A" >/dev/null 2>&1 && a_alive=1
        pgrep -f "dinerod.*$DATA_B" >/dev/null 2>&1 && b_alive=1
        if [ "$a_alive" -eq 0 ] && [ "$b_alive" -eq 0 ]; then
            break
        fi
        sleep 1
    done

    # Escalate if anything is still alive.
    pkill -9 -f "dinerod.*$DATA_A" 2>/dev/null || true
    pkill -9 -f "dinerod.*$DATA_B" 2>/dev/null || true

    sleep 1

    # PRESERVE THE EVIDENCE ON FAILURE.
    #
    # This cleanup previously ran `rm -rf` on both datadirs unconditionally, so
    # every failure destroyed the daemon logs that would explain it. That is a
    # large part of why the convergence failure in this test has survived as a
    # "just re-run it" flake: each occurrence erased its own diagnosis.
    #
    # The failure mode worth capturing: node B's height OSCILLATES DOWNWARD
    # (e.g. 30 -> 29 -> 50 -> 49 -> 73 -> 68 -> 51) while node A sits stable at
    # the target, so B never converges. A node should not move to a lower-work
    # chain; the reason is in B's log, which no longer exists by the time anyone
    # looks.
    #
    # On a non-zero exit, copy each datadir's logs to STRESS_LOG_PRESERVE_DIR
    # (defaults to a stable path under the artifact root when the caller sets
    # one). Only *.log is copied -- the chainstate can be gigabytes.
    if [ "$exit_code" -ne 0 ]; then
        local preserve="${STRESS_LOG_PRESERVE_DIR:-${ARTIFACT_ROOT:-/tmp}/stress-failure-logs-$$}"
        mkdir -p "$preserve" 2>/dev/null || true
        for tag in a:"$DATA_A" b:"$DATA_B"; do
            local node="${tag%%:*}" dir="${tag#*:}"
            [ -n "$dir" ] && [ -d "$dir" ] || continue
            # daemon.log ONLY. A bare *.log glob also matches RocksDB's
            # WAL files (000004.log etc), which are binary database
            # internals and useless for diagnosis -- the first version of
            # this preserved exactly those and nothing readable.
            [ -f "$dir/daemon.log" ] && cp "$dir/daemon.log" \
                "$preserve/node-$node-daemon.log" 2>/dev/null || true
        done
        log_warn "Failure logs preserved in: $preserve"
        ls -1 "$preserve" 2>/dev/null | sed 's/^/  preserved: /' || true
    fi

    # Remove data directories with retries; background shutdown may flush peers.dat/banlist.dat
    # briefly after the first TERM.
    for dir in "$DATA_A" "$DATA_B"; do
        if [ -z "$dir" ] || [ ! -e "$dir" ]; then
            continue
        fi

        local removed=0
        for _ in $(seq 1 5); do
            rm -rf "$dir" >/dev/null 2>&1 && removed=1 && break
            sleep 1
        done

        if [ "$removed" -eq 0 ] && [ -e "$dir" ]; then
            log_warn "Cleanup left directory behind: $dir"
            find "$dir" -maxdepth 2 -mindepth 1 2>/dev/null | sed 's/^/  leftover: /'
        fi
    done

    log_pass "Cleanup complete"
}

trap cleanup EXIT

# RPC helper for Node A
rpc_a() {
    local method="$1"
    shift
    local params=""

    for p in "$@"; do
        if [ -n "$params" ]; then
            params="$params,"
        fi
        # Handle booleans and numbers without quotes
        if [[ "$p" =~ ^(true|false)$ ]] || [[ "$p" =~ ^[0-9]+\.?[0-9]*$ ]]; then
            params="$params$p"
        else
            params="$params\"$p\""
        fi
    done

    local cookie
    cookie=$(cat "$DATA_A/.cookie" 2>/dev/null | cut -d: -f2) || return 1

    curl -s --max-time "$RPC_TIMEOUT" \
        --user "__cookie__:$cookie" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":[$params],\"id\":1}" \
        "http://127.0.0.1:$NODE_A_RPC" 2>/dev/null | jq -r '.result // .error // .'
}

# RPC helper for Node B
rpc_b() {
    local method="$1"
    shift
    local params=""

    for p in "$@"; do
        if [ -n "$params" ]; then
            params="$params,"
        fi
        if [[ "$p" =~ ^(true|false)$ ]] || [[ "$p" =~ ^[0-9]+\.?[0-9]*$ ]]; then
            params="$params$p"
        else
            params="$params\"$p\""
        fi
    done

    local cookie
    cookie=$(cat "$DATA_B/.cookie" 2>/dev/null | cut -d: -f2) || return 1

    curl -s --max-time "$RPC_TIMEOUT" \
        --user "__cookie__:$cookie" \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":[$params],\"id\":1}" \
        "http://127.0.0.1:$NODE_B_RPC" 2>/dev/null | jq -r '.result // .error // .'
}

# Check if process is running
is_running() {
    local pid="$1"
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

# Get current time in milliseconds
now_ms() {
    python3 -c "import time; print(int(time.time() * 1000))"
}

# Sleep for milliseconds
sleep_ms() {
    local ms="$1"
    python3 -c "import time; time.sleep($ms / 1000.0)"
}

# ═══════════════════════════════════════════════════════════════════════════
# Step A: Start Nodes and Connect
# ═══════════════════════════════════════════════════════════════════════════

start_nodes() {
    log_step "Step A" "Starting nodes and connecting..."

    # Check binary exists
    if [ ! -x "$DINEROD" ]; then
        log_fail "dinerod not found at $DINEROD"
        log_info "Run: cmake --build build --target dinerod"
        exit 1
    fi

    # Create data directories
    mkdir -p "$DATA_A" "$DATA_B"

    # Start Node A
    log_info "Starting Node A (RPC: $NODE_A_RPC, P2P: $NODE_A_P2P)..."
    # Capture the daemon's output instead of discarding it.
    #
    # This was `--daemon ... >/dev/null 2>&1`, which threw away every line the
    # node produced. Combined with cleanup's rm -rf, a convergence failure left
    # literally no evidence: two heights and a timeout, nothing else. That is
    # why this test has survived as a "just re-run it" flake.
    #
    # dinerod has no --logfile option and logs to stdout, so the redirect is
    # the only capture point; and `--daemon` is dropped because a forked child
    # detaches from this stdout. Backgrounding with & keeps the stream attached.
    # PID discovery below already uses pgrep, so it works either way.
    "$DINEROD" \
        --regtest \
        --datadir="$DATA_A" \
        --rpcport="$NODE_A_RPC" \
        --p2pport="$NODE_A_P2P" \
        > "$DATA_A/daemon.log" 2>&1 &

    # Start Node B
    log_info "Starting Node B (RPC: $NODE_B_RPC, P2P: $NODE_B_P2P)..."
    "$DINEROD" \
        --regtest \
        --datadir="$DATA_B" \
        --rpcport="$NODE_B_RPC" \
        --p2pport="$NODE_B_P2P" \
        > "$DATA_B/daemon.log" 2>&1 &

    # Wait for daemons to fork and start
    sleep 2

    # Find actual daemon PIDs using pgrep (daemon fork means $! is stale)
    PID_A=$(pgrep -f "dinerod.*$DATA_A" | head -1) || PID_A=""
    PID_B=$(pgrep -f "dinerod.*$DATA_B" | head -1) || PID_B=""

    log_info "Node A PID: ${PID_A:-not found}"
    log_info "Node B PID: ${PID_B:-not found}"

    # Wait for startup
    log_info "Waiting ${STARTUP_WAIT}s for nodes to start..."
    sleep "$STARTUP_WAIT"

    # Verify Node A is running
    local tries=0
    while [ $tries -lt 30 ]; do
        if rpc_a "blockchain.getblockcount" >/dev/null 2>&1; then
            break
        fi
        tries=$((tries + 1))
        sleep 1
    done

    if [ $tries -eq 30 ]; then
        log_fail "Node A failed to start (RPC not responding)"
        exit 1
    fi
    log_pass "Node A started"

    # Verify Node B is running
    tries=0
    while [ $tries -lt 30 ]; do
        if rpc_b "blockchain.getblockcount" >/dev/null 2>&1; then
            break
        fi
        tries=$((tries + 1))
        sleep 1
    done

    if [ $tries -eq 30 ]; then
        log_fail "Node B failed to start (RPC not responding)"
        exit 1
    fi
    log_pass "Node B started"

    # Connect Node A to Node B using addnode with "onetry"
    log_info "Connecting Node A to Node B..."
    rpc_a "addnode" "127.0.0.1:$NODE_B_P2P" "onetry" >/dev/null 2>&1 || true

    # Wait for connection
    sleep 3

    # Verify peer connection using getconnectioncount
    local peer_count
    # Note: rpc_a already extracts .result, don't double-parse
    peer_count=$(rpc_a "getconnectioncount" 2>/dev/null) || peer_count=0

    if [ "$peer_count" -lt 1 ]; then
        log_warn "Peer count is $peer_count, retrying connection..."
        rpc_b "addnode" "127.0.0.1:$NODE_A_P2P" "onetry" >/dev/null 2>&1 || true
        sleep 3
        peer_count=$(rpc_a "getconnectioncount" 2>/dev/null) || peer_count=0
    fi

    if [ "$peer_count" -lt 1 ]; then
        log_fail "Nodes failed to connect (peer count: $peer_count)"
        exit 1
    fi

    log_pass "Nodes connected (peers: $peer_count)"

    # Get initial heights (rpc_a/rpc_b already extract .result)
    local height_a height_b
    height_a=$(rpc_a "blockchain.getblockcount" 2>/dev/null) || height_a=0
    height_b=$(rpc_b "blockchain.getblockcount" 2>/dev/null) || height_b=0

    log_info "Initial heights: A=$height_a, B=$height_b"
}

# ═══════════════════════════════════════════════════════════════════════════
# Step B: Start Mining
# ═══════════════════════════════════════════════════════════════════════════

start_mining() {
    log_step "Step B" "Starting mining ($MINERS_PER_NODE threads per node)..."

    # Get mining addresses
    local addr_a addr_b
    addr_a=$(rpc_a "wallet.listaddresses" | jq -r 'if type=="array" then .[0].address else empty end' 2>/dev/null)
    addr_b=$(rpc_b "wallet.listaddresses" | jq -r 'if type=="array" then .[0].address else empty end' 2>/dev/null)

    if [ -z "$addr_a" ]; then
        log_info "Creating wallet on Node A..."
        addr_a=$(rpc_a "wallet.createhd" "stress_a" | jq -r '.first_address // empty' 2>/dev/null)
    fi

    if [ -z "$addr_b" ]; then
        log_info "Creating wallet on Node B..."
        addr_b=$(rpc_b "wallet.createhd" "stress_b" | jq -r '.first_address // empty' 2>/dev/null)
    fi

    log_info "Mining address A: ${addr_a:-<default>}"
    log_info "Mining address B: ${addr_b:-<default>}"

    # Set mining addresses if available
    if [ -n "$addr_a" ]; then
        rpc_a "mining.setaddress" "$addr_a" >/dev/null 2>&1 || true
    fi
    if [ -n "$addr_b" ]; then
        rpc_b "mining.setaddress" "$addr_b" >/dev/null 2>&1 || true
    fi

    local info_a info_b ibd_a ibd_b
    info_a=$(rpc_a "getblockchaininfo" 2>/dev/null || echo '{}')
    info_b=$(rpc_b "getblockchaininfo" 2>/dev/null || echo '{}')
    ibd_a=$(echo "$info_a" | jq -r '.initialblockdownload // .ibd // false' 2>/dev/null || echo "false")
    ibd_b=$(echo "$info_b" | jq -r '.initialblockdownload // .ibd // false' 2>/dev/null || echo "false")

    # Fresh regtest nodes can start in IBD and reject mining.start until there is
    # at least a small validated chain to synchronize on.
    if [[ "$ibd_a" == "true" || "$ibd_b" == "true" ]]; then
        log_info "Nodes still report IBD; bootstrapping 2 blocks on Node A before stress mining..."
        local bootstrap
        bootstrap=$(rpc_a "generatetoaddress" 2 "$addr_a" 2>&1) || {
            log_fail "Bootstrap mining failed on Node A: $bootstrap"
            exit 1
        }

        local sync_tries=0 sync_height_a sync_height_b
        while [ $sync_tries -lt 30 ]; do
            sync_height_a=$(rpc_a "blockchain.getblockcount" 2>/dev/null) || sync_height_a=0
            sync_height_b=$(rpc_b "blockchain.getblockcount" 2>/dev/null) || sync_height_b=0
            if [ "$sync_height_a" -ge 2 ] && [ "$sync_height_b" -ge 2 ] && [ "$sync_height_a" = "$sync_height_b" ]; then
                break
            fi
            sleep 1
            sync_tries=$((sync_tries + 1))
        done

        if [ "${sync_height_a:-0}" -lt 2 ] || [ "${sync_height_b:-0}" -lt 2 ]; then
            log_fail "Bootstrap sync did not complete (A=${sync_height_a:-0}, B=${sync_height_b:-0})"
            exit 1
        fi
        log_pass "Bootstrap chain synchronized (A=${sync_height_a}, B=${sync_height_b})"
    fi

    # Start mining on both nodes (supports asymmetric mining via MINERS_A/MINERS_B)
    local result_a result_b
    result_a=$(rpc_a "mining.start" "$MINERS_A" 2>&1)
    result_b=$(rpc_b "mining.start" "$MINERS_B" 2>&1)

    log_info "Node A mining.start: $result_a"
    log_info "Node B mining.start: $result_b"

    if echo "$result_a" | jq -e 'type == "object" and has("code")' >/dev/null 2>&1; then
        log_fail "Node A mining.start failed"
        exit 1
    fi
    if echo "$result_b" | jq -e 'type == "object" and has("code")' >/dev/null 2>&1; then
        log_fail "Node B mining.start failed"
        exit 1
    fi

    log_pass "Mining started on both nodes"
}

# ═══════════════════════════════════════════════════════════════════════════
# Step C: Wait for Target Height
# ═══════════════════════════════════════════════════════════════════════════

wait_for_height() {
    log_step "Mining" "Waiting for height >= $TARGET_HEIGHT..."

    local start_time height_a height_b max_height last_report
    start_time=$(date +%s)
    last_report=0

    while true; do
        # Get heights (rpc_a/rpc_b already extract .result)
        height_a=$(rpc_a "blockchain.getblockcount" 2>/dev/null) || height_a=0
        height_b=$(rpc_b "blockchain.getblockcount" 2>/dev/null) || height_b=0

        # Calculate max
        if [ "$height_a" -gt "$height_b" ]; then
            max_height=$height_a
        else
            max_height=$height_b
        fi

        # Progress report every 5 seconds
        local now
        now=$(date +%s)
        if [ $((now - last_report)) -ge 5 ]; then
            log_info "Height: A=$height_a B=$height_b (target: $TARGET_HEIGHT)"
            last_report=$now
        fi

        # Check target reached
        if [ "$max_height" -ge "$TARGET_HEIGHT" ]; then
            log_pass "Target height reached (A=$height_a, B=$height_b)"
            break
        fi

        # Check for crashes
        if ! is_running "$PID_A"; then
            log_fail "Node A crashed during mining!"
            exit 1
        fi
        if ! is_running "$PID_B"; then
            log_fail "Node B crashed during mining!"
            exit 1
        fi

        # Rate limiting
        sleep_ms "$RATE_LIMIT_MS"
    done

    local elapsed=$(($(date +%s) - start_time))
    log_info "Mining phase completed in ${elapsed}s"
}

# ═══════════════════════════════════════════════════════════════════════════
# Step D: Stop Mining and Wait for Convergence
# ═══════════════════════════════════════════════════════════════════════════

stop_and_converge() {
    log_step "Step C" "Stopping mining and waiting for convergence..."

    # Stop mining
    log_info "Stopping mining on Node A..."
    rpc_a "mining.stop" >/dev/null 2>&1 || true

    log_info "Stopping mining on Node B..."
    rpc_b "mining.stop" >/dev/null 2>&1 || true

    sleep 2
    log_pass "Mining stopped"

    # ═══════════════════════════════════════════════════════════════════════
    # SETTLE PHASE: Allow in-flight blocks to propagate and reorgs to complete
    # With 40 concurrent miners, there can be many blocks in-flight when we stop.
    # Give the network time to settle before checking convergence.
    # ═══════════════════════════════════════════════════════════════════════
    local settle_time=${SETTLE_TIME:-10}
    log_info "Settle phase: waiting ${settle_time}s for in-flight blocks to propagate..."

    local settle_start settle_elapsed prev_height_a prev_height_b stable_count
    settle_start=$(date +%s)
    prev_height_a=0
    prev_height_b=0
    stable_count=0

    while true; do
        local height_a height_b
        height_a=$(rpc_a "blockchain.getblockcount" 2>/dev/null) || height_a=0
        height_b=$(rpc_b "blockchain.getblockcount" 2>/dev/null) || height_b=0

        # Check if heights are stable (not changing)
        if [ "$height_a" = "$prev_height_a" ] && [ "$height_b" = "$prev_height_b" ]; then
            stable_count=$((stable_count + 1))
        else
            stable_count=0
        fi
        prev_height_a=$height_a
        prev_height_b=$height_b

        settle_elapsed=$(($(date +%s) - settle_start))

        # Exit settle phase when: timeout reached OR heights stable for 3 seconds
        if [ "$settle_elapsed" -ge "$settle_time" ] || [ "$stable_count" -ge 3 ]; then
            log_pass "Settle phase complete (heights stable: A=$height_a, B=$height_b)"
            break
        fi

        sleep 1
    done

    # Phase G.X: Fork resolution - announce tips to trigger sync of diverged chains
    log_info "Triggering tip announcement for fork resolution..."
    rpc_a "announcetip" >/dev/null 2>&1 || true
    rpc_b "announcetip" >/dev/null 2>&1 || true
    sleep 2

    # Wait for convergence (hash comparison is the key invariant)
    log_info "Waiting for tips to match (timeout: ${CONVERGENCE_TIMEOUT}s)..."
    log_info "Key invariant: same tip hash = same chain (height is secondary)"

    local start_time tip_a tip_b height_a height_b
    start_time=$(date +%s)

    while true; do
        # Get tips and heights
        # Note: rpc_a/rpc_b already extract .result with jq, so don't double-parse
        tip_a=$(rpc_a "blockchain.getbestblockhash" 2>/dev/null) || tip_a=""
        tip_b=$(rpc_b "blockchain.getbestblockhash" 2>/dev/null) || tip_b=""
        height_a=$(rpc_a "blockchain.getblockcount" 2>/dev/null) || height_a=0
        height_b=$(rpc_b "blockchain.getblockcount" 2>/dev/null) || height_b=0

        log_info "A: height=$height_a tip=${tip_a:0:16}..."
        log_info "B: height=$height_b tip=${tip_b:0:16}..."

        # Check convergence - HASH COMPARISON IS PRIMARY
        if [ -n "$tip_a" ] && [ -n "$tip_b" ] && [ "$tip_a" = "$tip_b" ]; then
            local elapsed=$(($(date +%s) - start_time))
            log_pass "Converged in ${elapsed}s!"
            log_info "Final tip: $tip_a"
            log_info "Final height: $height_a"
            return 0
        fi

        # Check timeout
        local elapsed=$(($(date +%s) - start_time))
        if [ "$elapsed" -ge "$CONVERGENCE_TIMEOUT" ]; then
            # Try one last sync attempt - announce tips and reconnect peers
            log_warn "Tips not matching, triggering sync retry with tip announcement..."
            rpc_a "announcetip" >/dev/null 2>&1 || true
            rpc_b "announcetip" >/dev/null 2>&1 || true
            sleep 2
            rpc_a "addnode" "127.0.0.1:$NODE_B_P2P" "onetry" >/dev/null 2>&1 || true
            rpc_b "addnode" "127.0.0.1:$NODE_A_P2P" "onetry" >/dev/null 2>&1 || true
            sleep 5

            # Final check
            tip_a=$(rpc_a "blockchain.getbestblockhash" 2>/dev/null) || tip_a=""
            tip_b=$(rpc_b "blockchain.getbestblockhash" 2>/dev/null) || tip_b=""
            height_a=$(rpc_a "blockchain.getblockcount" 2>/dev/null) || height_a=0
            height_b=$(rpc_b "blockchain.getblockcount" 2>/dev/null) || height_b=0

            if [ -n "$tip_a" ] && [ -n "$tip_b" ] && [ "$tip_a" = "$tip_b" ]; then
                local total_elapsed=$(($(date +%s) - start_time))
                log_pass "Converged after sync retry (${total_elapsed}s)!"
                log_info "Final tip: $tip_a"
                log_info "Final height: $height_a"
                return 0
            fi

            log_fail "Convergence timeout after ${elapsed}s!"
            log_info "Node A: height=$height_a tip=$tip_a"
            log_info "Node B: height=$height_b tip=$tip_b"
            return 1
        fi

        sleep 1
    done
}

# ═══════════════════════════════════════════════════════════════════════════
# Step E: Assert Invariants
# ═══════════════════════════════════════════════════════════════════════════

assert_invariants() {
    log_step "Step D" "Asserting invariants..."

    local failures=0

    # Assert 1: Both processes running
    if is_running "$PID_A"; then
        log_pass "Node A process running (PID $PID_A)"
    else
        log_fail "Node A process NOT running!"
        failures=$((failures + 1))
    fi

    if is_running "$PID_B"; then
        log_pass "Node B process running (PID $PID_B)"
    else
        log_fail "Node B process NOT running!"
        failures=$((failures + 1))
    fi

    # Assert 2: RPC responsive (no deadlocks)
    # Note: rpc_a/rpc_b already extract .result, don't double-parse
    local height_a height_b
    height_a=$(rpc_a "blockchain.getblockcount" 2>/dev/null) || height_a=""
    if [ -n "$height_a" ]; then
        log_pass "Node A RPC responsive"
    else
        log_fail "Node A RPC NOT responding (possible deadlock)"
        failures=$((failures + 1))
    fi

    height_b=$(rpc_b "blockchain.getblockcount" 2>/dev/null) || height_b=""
    if [ -n "$height_b" ]; then
        log_pass "Node B RPC responsive"
    else
        log_fail "Node B RPC NOT responding (possible deadlock)"
        failures=$((failures + 1))
    fi

    # Assert 3: Same tip hash
    local tip_a tip_b
    tip_a=$(rpc_a "blockchain.getbestblockhash" 2>/dev/null) || tip_a=""
    tip_b=$(rpc_b "blockchain.getbestblockhash" 2>/dev/null) || tip_b=""

    if [ -n "$tip_a" ] && [ -n "$tip_b" ] && [ "$tip_a" = "$tip_b" ]; then
        log_pass "Same tip hash: ${tip_a:0:16}..."
    else
        log_fail "Tip hash mismatch!"
        log_info "  Node A: $tip_a"
        log_info "  Node B: $tip_b"
        failures=$((failures + 1))
    fi

    # Assert 4: Same height
    if [ -n "$height_a" ] && [ -n "$height_b" ] && [ "$height_a" = "$height_b" ]; then
        log_pass "Same height: $height_a"
    else
        log_fail "Height mismatch!"
        log_info "  Node A: $height_a"
        log_info "  Node B: $height_b"
        failures=$((failures + 1))
    fi

    # Assert 5: Height >= target
    if [ -n "$height_a" ] && [ "$height_a" -ge "$TARGET_HEIGHT" ]; then
        log_pass "Height >= target ($height_a >= $TARGET_HEIGHT)"
    else
        log_fail "Height below target ($height_a < $TARGET_HEIGHT)"
        failures=$((failures + 1))
    fi

    # Get mining stats
    log_info ""
    log_info "Mining Statistics:"
    local stats_a stats_b
    stats_a=$(rpc_a "mining.getstatus" 2>/dev/null) || stats_a="{}"
    stats_b=$(rpc_b "mining.getstatus" 2>/dev/null) || stats_b="{}"

    local blocks_a blocks_b
    blocks_a=$(echo "$stats_a" | jq -r '.blocks_found // 0' 2>/dev/null) || blocks_a=0
    blocks_b=$(echo "$stats_b" | jq -r '.blocks_found // 0' 2>/dev/null) || blocks_b=0

    log_info "  Node A blocks found: $blocks_a"
    log_info "  Node B blocks found: $blocks_b"

    return $failures
}

# ═══════════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════════

main() {
    echo ""
    echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${CYAN}  Dual-Node Mining Stress Test                                 ${NC}"
    echo -e "${CYAN}  Node A: $MINERS_A miners, Node B: $MINERS_B miners - Fork Pressure      ${NC}"
    echo -e "${CYAN}═══════════════════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "Configuration:"
    echo -e "  Target height:       $TARGET_HEIGHT blocks"
    echo -e "  Miners Node A:       $MINERS_A threads"
    echo -e "  Miners Node B:       $MINERS_B threads"
    echo -e "  Settle time:         ${SETTLE_TIME}s (in-flight block propagation)"
    echo -e "  Convergence timeout: ${CONVERGENCE_TIMEOUT}s"
    echo -e "  Rate limit:          ${RATE_LIMIT_MS}ms"
    echo ""

    # Run test steps
    start_nodes
    echo ""

    start_mining
    echo ""

    wait_for_height
    echo ""

    if ! stop_and_converge; then
        log_fail "Convergence failed!"
        exit 1
    fi
    echo ""

    if ! assert_invariants; then
        echo ""
        echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
        echo -e "${RED}  TEST FAILED - Invariant violations detected                  ${NC}"
        echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
        exit 1
    fi

    echo ""
    echo -e "${GREEN}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${GREEN}  TEST PASSED - Dual-node mining stress                        ${NC}"
    echo -e "${GREEN}═══════════════════════════════════════════════════════════════${NC}"
    echo ""
    echo "Validated:"
    echo "  - Fork pressure: both nodes found blocks concurrently"
    echo "  - Reorg stability: ActivateBestChain handled rapid changes"
    echo "  - Relay correctness: blocks propagated between nodes"
    echo "  - Concurrency safety: no deadlocks or crashes"
    echo ""

    exit 0
}

main "$@"
