#!/usr/bin/env bash
#
# Live-path reachability test for issue #490.
#
# WHAT THIS ANSWERS
# -----------------
# ConsensusFuzzer found that ConsensusUTXOSet::ApplyBlock/UndoBlock records a
# Utreexo deletion before confirming the removal succeeded and ignores
# remove()'s return value, so undo deltas can claim deletions that never
# happened. That API has no non-test callers.
#
# The open question was whether the LIVE stateful path -- BlockValidator's
# ConnectBlock/DisconnectBlock, reached through a real dinerod -- shares the
# defect. It could not be answered by unit test: BlockValidator::ConnectBlock
# transitively pulls ChainstateService, WalletWorker, vault, RPC handlers and
# Mempool, so it cannot be linked into a standalone consensus binary. This test
# answers it at the only level where the live path is reachable.
#
# SAME TOPOLOGY, NOT SAME BLOCKS
# ------------------------------
# The fuzzer's blocks cannot enter a real daemon: random Merkle roots, no valid
# proof-of-work, unsigned P2WPKH spends, simplified coinbase and header
# construction. Reproducing them verbatim is impossible and claiming otherwise
# would be dishonest.
#
# What IS reproduced is the deterministic operation TOPOLOGY that seed 6 failed
# under, using valid regtest blocks:
#
#   * forest sizes crossing the failing range -- seed 6's remove failures
#     occurred at numLeaves_ 125..154, so the run asserts the live leaf count
#     actually crosses that window rather than assuming it does;
#   * the same shape of rounds -- grow the chain, spend to force deletions,
#     then reorg by depth 1..3, which is TestSimpleReorg's depth distribution;
#   * repeated rounds, because seed 6 failed at round 9 rather than round 1.
#
# WHAT IS ASSERTED
# ----------------
# At each fork point the full observable consensus state is digested: tip hash,
# UTXO set summary, Utreexo commitment, roots, stats, and the internal forest
# serialization. After invalidating back to that fork, the digest must match
# EXACTLY. Then a competing branch is built and the node restarted, and the
# state must still match what was recorded.
#
# Whole RPC results are digested rather than hand-picked fields. That is
# strictly stronger -- any field diverging fails the test -- and it cannot
# silently stop checking something when a field is renamed.
#
# A failure here would demonstrate the live path shares the defect. A pass is
# evidence it does not, bounded by this topology.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# CTest injects the exact in-tree target path; keep the conventional fallback
# for developers running this directly.
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"

# Enough blocks to push the live leaf count past seed 6's failing window (154)
# with margin, while leaving mature coinbases available to spend.
PRELOAD_BLOCKS="${PRELOAD_BLOCKS:-170}"
ROUNDS="${ROUNDS:-6}"
BLOCKS_PER_ROUND="${BLOCKS_PER_ROUND:-10}"
# Seed 6's failing remove() calls spanned this leaf-count window.
FAILING_WINDOW_LO=125
FAILING_WINDOW_HI=154

DATA_DIR="/tmp/dinero_utreexo_reorg_state_$$"
LOG_FILE="${DATA_DIR}.log"
FOREST_DUMP="${DATA_DIR}.forest"
RPC_PORT=""
P2P_PORT=""
NODE_PID=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }

fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2

    # Stop the daemon and wait for exit BEFORE reading its log. stdout is
    # block-buffered when redirected to a file, so tailing a running daemon
    # prints nothing -- the exact defect fixed in #479.
    if [[ -n "${NODE_PID}" ]]; then
        kill "${NODE_PID}" 2>/dev/null || true
        for _ in $(seq 1 50); do
            kill -0 "${NODE_PID}" 2>/dev/null || break
            sleep 0.2
        done
        kill -9 "${NODE_PID}" 2>/dev/null || true
    fi

    if [[ -f "${LOG_FILE}" ]]; then
        printf -- '--- last 120 log lines ---\n' >&2
        tail -120 "${LOG_FILE}" >&2 || true
        printf -- '--- utreexo/reorg lines ---\n' >&2
        grep -nE "Utreexo|Restore|Disconnect|reorg|remove" "${LOG_FILE}" | tail -60 >&2 || true
    fi
    printf -- '--- preserved for inspection ---\n  datadir: %s\n  log:     %s\n' \
        "${DATA_DIR}" "${LOG_FILE}" >&2
    exit 1
}

cleanup() {
    if [[ -n "${NODE_PID}" ]]; then
        kill "${NODE_PID}" 2>/dev/null || true
        for _ in $(seq 1 50); do
            kill -0 "${NODE_PID}" 2>/dev/null || break
            sleep 0.2
        done
        kill -9 "${NODE_PID}" 2>/dev/null || true
    fi
    # Preserve evidence on failure; clean up only on success.
    if [[ "${KEEP_ON_FAIL}" -eq 0 ]]; then
        rm -rf "${DATA_DIR}" "${LOG_FILE}" "${FOREST_DUMP}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

require_tools() {
    command -v curl >/dev/null || fail "curl is required"
    command -v jq >/dev/null || fail "jq is required"
    command -v lsof >/dev/null || fail "lsof is required for collision-free port selection"
    command -v shasum >/dev/null || command -v sha256sum >/dev/null \
        || fail "shasum or sha256sum is required"
    [[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
}

sha256_stdin() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | awk '{print $1}'
    else
        shasum -a 256 | awk '{print $1}'
    fi
}

pick_ports() {
    local candidate
    for _ in $(seq 1 40); do
        candidate=$((36000 + RANDOM % 12000))
        if ! lsof -nP -iTCP:"${candidate}" -sTCP:LISTEN >/dev/null 2>&1 \
           && ! lsof -nP -iTCP:"$((candidate + 100))" -sTCP:LISTEN >/dev/null 2>&1; then
            RPC_PORT="${candidate}"
            P2P_PORT="$((candidate + 100))"
            return 0
        fi
    done
    fail "unable to find a free RPC/P2P port pair after 40 attempts"
}

cookie_file() {
    if [[ -f "${DATA_DIR}/.cookie" ]]; then printf '%s\n' "${DATA_DIR}/.cookie"; return 0; fi
    if [[ -f "${DATA_DIR}/regtest/.cookie" ]]; then printf '%s\n' "${DATA_DIR}/regtest/.cookie"; return 0; fi
    return 1
}

rpc_raw() {
    local method="$1"
    local params_json="$2"
    local cookie_path cookie
    cookie_path="$(cookie_file 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

# Strict envelope checking (post-#458 semantics): a handler failure surfaces as
# a non-null TOP-LEVEL "error". Checking only for a present "result", or
# grepping for the substring "error", would let real failures through.
rpc_ok() {
    local method="$1"
    local params_json="$2"
    local response
    response="$(rpc_raw "${method}" "${params_json}")" || return 1
    jq -e '.error == null and has("result")' <<<"${response}" >/dev/null 2>&1 || {
        printf '%s\n' "${response}" >&2
        return 1
    }
    printf '%s\n' "${response}"
}

rpc_result() {
    local method="$1"
    local params_json="${2:-[]}"
    local response
    response="$(rpc_ok "${method}" "${params_json}")" \
        || fail "RPC ${method} failed (see envelope above)"
    jq -c '.result' <<<"${response}"
}

start_node() {
    mkdir -p "${DATA_DIR}"
    # --listen=0 and no peers: fully offline. Nothing may influence this node's
    # chain except our own RPC calls, or the state comparisons are meaningless.
    "${DINEROD}" \
        --regtest \
        --datadir="${DATA_DIR}" \
        --rpcport="${RPC_PORT}" \
        --port="${P2P_PORT}" \
        --listen=0 \
        >>"${LOG_FILE}" 2>&1 &
    NODE_PID="$!"
}

stop_node() {
    [[ -n "${NODE_PID}" ]] || return 0
    kill "${NODE_PID}" 2>/dev/null || true
    # Wait for actual exit rather than sleeping a guessed interval.
    for _ in $(seq 1 100); do
        if ! kill -0 "${NODE_PID}" 2>/dev/null; then
            NODE_PID=""
            return 0
        fi
        sleep 0.2
    done
    kill -9 "${NODE_PID}" 2>/dev/null || true
    NODE_PID=""
}

wait_rpc() {
    for _ in $(seq 1 120); do
        if rpc_raw "getblockcount" '[]' 2>/dev/null | jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    fail "daemon RPC did not become ready"
}

wait_height() {
    local want="$1"
    for _ in $(seq 1 240); do
        local h
        h="$(rpc_raw "getblockcount" '[]' 2>/dev/null | jq -r '.result // empty')"
        if [[ -n "${h}" && "${h}" -eq "${want}" ]]; then
            return 0
        fi
        sleep 0.25
    done
    fail "height did not reach ${want} (currently $(rpc_raw "getblockcount" '[]' | jq -r '.result // "?"'))"
}

# Digest of the full observable consensus state. Whole RPC results are hashed
# rather than selected fields, so nothing silently stops being compared.
capture_state() {
    {
        printf 'tip=%s\n'         "$(rpc_result 'getbestblockhash')"
        printf 'height=%s\n'      "$(rpc_result 'getblockcount')"
        printf 'txoutset=%s\n'    "$(rpc_result 'blockchain.gettxoutsetinfo')"
        printf 'commitment=%s\n'  "$(rpc_result 'blockchain.getutreexocommitment')"
        printf 'roots=%s\n'       "$(rpc_result 'blockchain.getutreexoroots')"
        printf 'stats=%s\n'       "$(rpc_result 'blockchain.getutreexostats')"
        # dumpforestinternal writes the forest's internal serialization to a
        # file. Hashing that file is the strongest available check: it covers
        # node contents and deletion bookkeeping, not just the roots.
        rm -f "${FOREST_DUMP}" 2>/dev/null || true
        rpc_result 'utreexo.dumpforestinternal' "[\"${FOREST_DUMP}\"]" >/dev/null
        if [[ -s "${FOREST_DUMP}" ]]; then
            printf 'forest=%s\n' "$(sha256_stdin < "${FOREST_DUMP}")"
        else
            printf 'forest=<empty>\n'
        fi
    } | sha256_stdin
}

# Best-effort live leaf count, used to prove the run actually reaches the
# forest sizes seed 6 failed at instead of assuming it does.
leaf_count() {
    local stats
    stats="$(rpc_result 'blockchain.getutreexostats' 2>/dev/null || echo '{}')"
    jq -r '
        [.. | objects | to_entries[]
         | select(.key | test("leaf|num_leaves|leaves"; "i"))
         | .value | numbers] | first // empty
    ' <<<"${stats}" 2>/dev/null || true
}

# ---------------------------------------------------------------------------

require_tools
pick_ports
info "RPC port ${RPC_PORT}, P2P port ${P2P_PORT} (offline, --listen=0)"

start_node
wait_rpc
info "daemon started (pid ${NODE_PID})"

rpc_raw "wallet.createhd" '["reorgstate"]' >/dev/null 2>&1 || true
ADDRESS="$(rpc_raw 'wallet.getnewaddress' '[]' | jq -r '.result.address // .result // empty')"
[[ -n "${ADDRESS}" ]] || fail "could not obtain a mining address"
info "mining address ${ADDRESS}"

info "preloading ${PRELOAD_BLOCKS} blocks to cross the failing forest window"
rpc_result 'generatetoaddress' "[${PRELOAD_BLOCKS},\"${ADDRESS}\"]" >/dev/null
wait_height "${PRELOAD_BLOCKS}"

LEAVES="$(leaf_count)"
if [[ -n "${LEAVES}" ]]; then
    info "live leaf count after preload: ${LEAVES}"
    if [[ "${LEAVES}" -lt "${FAILING_WINDOW_LO}" ]]; then
        fail "leaf count ${LEAVES} never reached seed 6's failing window (${FAILING_WINDOW_LO}..${FAILING_WINDOW_HI}); this run would not cover the defect"
    fi
    pass "forest crossed the failing window (>= ${FAILING_WINDOW_LO})"
else
    info "leaf count not exposed by getutreexostats; continuing without that assertion"
fi

# ---------------------------------------------------------------------------
# Anti-vacuity: prove the digest actually MEASURES something before relying on
# it to prove equality. If every RPC silently returned the same value -- or
# capture_state degraded to hashing constants -- then "state matched after
# reorg" would be trivially true and the whole test would prove nothing.
#
# A single extra block must change the digest.
# ---------------------------------------------------------------------------
SENSITIVITY_BEFORE="$(capture_state)"
SENSITIVITY_HEIGHT="$(rpc_result 'getblockcount')"
rpc_result 'generatetoaddress' "[1,\"${ADDRESS}\"]" >/dev/null
wait_height "$((SENSITIVITY_HEIGHT + 1))"
SENSITIVITY_AFTER="$(capture_state)"
[[ "${SENSITIVITY_BEFORE}" != "${SENSITIVITY_AFTER}" ]] || fail \
    "state digest did not change after mining a block -- capture_state is not
  measuring anything, so every equality assertion below would pass vacuously."
pass "state digest is sensitive to chain changes (not vacuous)"

# ---------------------------------------------------------------------------
# Rounds: grow, spend to force deletions, reorg by depth 1..3, assert exact
# restoration. Seed 6 failed at round 9, not round 1, so repetition matters.
# ---------------------------------------------------------------------------
for round in $(seq 1 "${ROUNDS}"); do
    depth=$(((round % 3) + 1))
    info "round ${round}/${ROUNDS} (reorg depth ${depth})"

    rpc_result 'generatetoaddress' "[${BLOCKS_PER_ROUND},\"${ADDRESS}\"]" >/dev/null

    # Force real deletions: spending mature coinbases removes leaves from the
    # accumulator, which is the operation whose failed removal corrupts undo
    # data in the test-only API.
    for _ in 1 2 3; do
        rpc_raw 'wallet.sendtoaddress' "[\"${ADDRESS}\",1.0]" >/dev/null 2>&1 || true
    done
    rpc_result 'generatetoaddress' "[1,\"${ADDRESS}\"]" >/dev/null

    FORK_HEIGHT="$(rpc_result 'getblockcount')"
    FORK_TIP="$(rpc_result 'getbestblockhash')"
    FORK_STATE="$(capture_state)"
    info "  fork point height=${FORK_HEIGHT} state=${FORK_STATE:0:16}..."

    # Extend, then invalidate back to the fork.
    rpc_result 'generatetoaddress' "[${depth},\"${ADDRESS}\"]" >/dev/null
    wait_height "$((FORK_HEIGHT + depth))"

    INVALIDATE_HASH="$(rpc_result 'blockchain.getblockhash' "[$((FORK_HEIGHT + 1))]" | tr -d '"')"
    [[ -n "${INVALIDATE_HASH}" ]] || fail "could not resolve block hash at height $((FORK_HEIGHT + 1))"
    rpc_result 'blockchain.invalidateblock' "[\"${INVALIDATE_HASH}\"]" >/dev/null
    wait_height "${FORK_HEIGHT}"

    RESTORED_TIP="$(rpc_result 'getbestblockhash')"
    [[ "${RESTORED_TIP}" == "${FORK_TIP}" ]] \
        || fail "round ${round}: tip after invalidate is ${RESTORED_TIP}, expected ${FORK_TIP}"

    RESTORED_STATE="$(capture_state)"
    [[ "${RESTORED_STATE}" == "${FORK_STATE}" ]] || fail \
        "round ${round}: state digest after disconnect does not match the fork point.
  expected ${FORK_STATE}
  actual   ${RESTORED_STATE}
  This is the live-path equivalent of the #490 failure: disconnecting blocks
  left the accumulator in a different state than before they were connected."
    pass "round ${round}: exact state restoration after depth-${depth} disconnect"

    # Build the competing branch, so subsequent rounds operate on a chain that
    # has genuinely reorged rather than one that merely rewound.
    rpc_result 'generatetoaddress' "[$((depth + 1)),\"${ADDRESS}\"]" >/dev/null
    wait_height "$((FORK_HEIGHT + depth + 1))"
done

FINAL_STATE="$(capture_state)"
FINAL_TIP="$(rpc_result 'getbestblockhash')"
FINAL_HEIGHT="$(rpc_result 'getblockcount')"
info "pre-restart: height=${FINAL_HEIGHT} state=${FINAL_STATE:0:16}..."

LEAVES="$(leaf_count)"
[[ -z "${LEAVES}" ]] || info "final live leaf count: ${LEAVES}"

# ---------------------------------------------------------------------------
# Restart: accumulator state must survive a full shutdown/reload unchanged.
# ---------------------------------------------------------------------------
info "restarting daemon"
stop_node
start_node
wait_rpc
wait_height "${FINAL_HEIGHT}"

RESTART_TIP="$(rpc_result 'getbestblockhash')"
[[ "${RESTART_TIP}" == "${FINAL_TIP}" ]] \
    || fail "tip changed across restart: ${RESTART_TIP} != ${FINAL_TIP}"

RESTART_STATE="$(capture_state)"
[[ "${RESTART_STATE}" == "${FINAL_STATE}" ]] || fail \
    "state digest changed across restart.
  expected ${FINAL_STATE}
  actual   ${RESTART_STATE}"
pass "state identical across restart"

pass "live BlockValidator path preserved exact accumulator state across ${ROUNDS} reorg rounds"
info "Scope: this exercises the live stateful path over seed 6's operation"
info "topology. It is evidence of non-reachability for that topology, not a"
info "proof over all sequences. See issue #490."
exit 0
