#!/usr/bin/env bash
# state_commitment_v1 evidence: cross-node agreement + deep reorg, with REAL
# shielded activity (commitment tree, anchor history, AND nullifiers).
#
# Why this exists, beyond the existing invertibility test:
#
#   * That test runs ONE node over a chain with no shielded transactions, so
#     the nullifier set stays empty and is never exercised.
#   * Mainnet cannot supply this either: its nullifier set is 0/0 (no shielded
#     spends since the epoch reset), and a snapshot-bootstrapped node is
#     headers-only so it cannot reindex.
#   * The value that would go in a block header must agree across nodes that
#     built their state independently. If it does not, activation is a split.
#
# Shape:
#   1. two regtest nodes, B peered to A
#   2. A mines; shield x2 and unshield x1 -> tree + anchors + a real nullifier
#   3. B syncs -> assert shielded_root(A) == shielded_root(B)   [cross-node]
#   4. deep reorg: invalidate a block BELOW the shielded activity on both,
#      forcing every shielded tx to be disconnected and reconnected
#   5. assert both roots return to their pre-reorg value          [invertible]
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
[[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/dinero_shroot_multinode_XXXXXX")"
A_RPC=37811; A_P2P=37812; B_RPC=37821; B_P2P=37822
PIDS=""
info() { echo "[INFO] $*"; }
pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*" >&2; cleanup; exit 1; }
cleanup() {
    for p in ${PIDS}; do kill "${p}" 2>/dev/null || true; done
    sleep 2
}
trap cleanup EXIT

start_node() { # <name> <datadir> <rpcport> <p2pport> [connect]
    local name=$1 dd=$2 rp=$3 pp=$4 peer=${5:-}
    mkdir -p "${dd}"
    local args=(--regtest --datadir="${dd}" --rpcport="${rp}" --p2pport="${pp}" --listen)
    [[ -n "${peer}" ]] && args+=(--connect="${peer}")
    "${DINEROD}" "${args[@]}" </dev/null > "${WORK}/${name}.log" 2>&1 &
    local pid=$!
    # Plain append + local: macOS ships bash 3.2, which has no ${arr[-1]}.
    PIDS="${PIDS} ${pid}"
    info "${name} started pid ${pid} rpc=${rp}"
}

rpc() { # <rpcport> <datadir> <method> [params-json]
    local rp=$1 dd=$2 m=$3 p=${4:-[]}
    local cookie
    cookie="$(cat "${dd}/regtest/.cookie" 2>/dev/null || cat "${dd}/.cookie" 2>/dev/null || true)"
    # No cookie yet = daemon still starting. Return empty rather than calling
    # curl with an empty --user, which prompts for a password and hangs the run.
    [[ -n "${cookie}" ]] || return 0
    curl -s -m 120 --user "${cookie}" </dev/null \
        --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${m}\",\"params\":${p}}" \
        -H 'content-type: application/json' "http://127.0.0.1:${rp}/"
}
jget() { python3 -c "import json,sys
d=json.load(sys.stdin)
r=d.get('result')
print(r.get('$1','') if isinstance(r,dict) else (r if r is not None else ''))" 2>/dev/null; }

wait_rpc() { # <rpcport> <datadir> <name>
    for _ in $(seq 1 60); do
        [[ -n "$(rpc "$1" "$2" getblockcount | jget '')" ]] && { info "$3 RPC up"; return 0; }
        sleep 2
    done
    fail "$3 RPC never came up"
}
height() { rpc "$1" "$2" getblockcount | jget ''; }
sroot()  { rpc "$1" "$2" daemon.shieldedroot | jget shielded_root; }

# ── 1. two nodes ──────────────────────────────────────────────────────────
start_node A "${WORK}/a" "${A_RPC}" "${A_P2P}"
wait_rpc "${A_RPC}" "${WORK}/a" A
start_node B "${WORK}/b" "${B_RPC}" "${B_P2P}" "127.0.0.1:${A_P2P}"
wait_rpc "${B_RPC}" "${WORK}/b" B

ADDR="$(rpc "${A_RPC}" "${WORK}/a" wallet.getnewaddress | jget address)"
[[ -n "${ADDR}" ]] || fail "could not get an address from A"
info "A mining address ${ADDR:0:24}..."

# ── 2. build REAL shielded state on A ─────────────────────────────────────
rpc "${A_RPC}" "${WORK}/a" mining.generatetoaddress "[130,\"${ADDR}\"]" >/dev/null
info "A at height $(height "${A_RPC}" "${WORK}/a") (coinbase matured)"

# Everything below this height must be disconnected by the deep reorg.
REORG_FROM=$(( $(height "${A_RPC}" "${WORK}/a") + 1 ))

for amt in 100 150; do
    r="$(rpc "${A_RPC}" "${WORK}/a" wallet.shield "[${amt}]")"
    echo "${r}" | tr -d '\n\t ' | grep -q '"error":null' || fail "shield ${amt} failed: ${r}"
    rpc "${A_RPC}" "${WORK}/a" mining.generatetoaddress "[2,\"${ADDR}\"]" >/dev/null
done
u="$(rpc "${A_RPC}" "${WORK}/a" wallet.unshield '{"amount":100.0}')"
echo "${u}" | tr -d '\n\t ' | grep -q '"error":null' || fail "unshield failed: ${u}"
NULLIFIER="$(echo "${u}" | jget nullifier_hex)"
[[ -n "${NULLIFIER}" ]] || fail "unshield produced no nullifier — the spend path was not exercised"
info "real nullifier created: ${NULLIFIER:0:16}..."
rpc "${A_RPC}" "${WORK}/a" mining.generatetoaddress "[4,\"${ADDR}\"]" >/dev/null

TIP=$(height "${A_RPC}" "${WORK}/a")
info "A tip ${TIP}; shielded activity spans ${REORG_FROM}..${TIP}"

# ── 3. cross-node agreement ───────────────────────────────────────────────
for _ in $(seq 1 60); do
    [[ "$(height "${B_RPC}" "${WORK}/b")" == "${TIP}" ]] && break
    sleep 2
done
[[ "$(height "${B_RPC}" "${WORK}/b")" == "${TIP}" ]] || \
    fail "B never synced to ${TIP} (got $(height "${B_RPC}" "${WORK}/b"))"

RA="$(sroot "${A_RPC}" "${WORK}/a")"
RB="$(sroot "${B_RPC}" "${WORK}/b")"
[[ -n "${RA}" && -n "${RB}" ]] || fail "daemon.shieldedroot unavailable (binary predates it)"
info "A root ${RA}"
info "B root ${RB}"
[[ "${RA}" == "${RB}" ]] || fail "CROSS-NODE DIVERGENCE at height ${TIP}
  A = ${RA}
  B = ${RB}
Two nodes built shielded state independently and disagree. Committing this in
a header would fork the chain between them."
pass "cross-node agreement at height ${TIP}: ${RA}"

# ── 4. deep reorg through every shielded transaction ──────────────────────
DEEP="$(rpc "${A_RPC}" "${WORK}/a" getblockhash "[${REORG_FROM}]" | jget '')"
[[ -n "${DEEP}" ]] || fail "could not resolve block at ${REORG_FROM}"
info "invalidating height ${REORG_FROM} on both nodes (disconnects $(( TIP - REORG_FROM + 1 )) blocks incl. every shielded tx)"
rpc "${A_RPC}" "${WORK}/a" invalidateblock "[\"${DEEP}\"]" >/dev/null
rpc "${B_RPC}" "${WORK}/b" invalidateblock "[\"${DEEP}\"]" >/dev/null
sleep 5
info "post-disconnect: A=$(height "${A_RPC}" "${WORK}/a") B=$(height "${B_RPC}" "${WORK}/b")"

rpc "${A_RPC}" "${WORK}/a" reconsiderblock "[\"${DEEP}\"]" >/dev/null
rpc "${B_RPC}" "${WORK}/b" reconsiderblock "[\"${DEEP}\"]" >/dev/null
for _ in $(seq 1 60); do
    [[ "$(height "${A_RPC}" "${WORK}/a")" == "${TIP}" && "$(height "${B_RPC}" "${WORK}/b")" == "${TIP}" ]] && break
    sleep 2
done
[[ "$(height "${A_RPC}" "${WORK}/a")" == "${TIP}" ]] || fail "A did not restore tip"
[[ "$(height "${B_RPC}" "${WORK}/b")" == "${TIP}" ]] || fail "B did not restore tip"

RA2="$(sroot "${A_RPC}" "${WORK}/a")"
RB2="$(sroot "${B_RPC}" "${WORK}/b")"
[[ "${RA2}" == "${RA}" ]] || fail "A root NOT invertible across a ${TIP} - ${REORG_FROM} block reorg
  before = ${RA}
  after  = ${RA2}"
[[ "${RB2}" == "${RB}" ]] || fail "B root NOT invertible across the deep reorg
  before = ${RB}
  after  = ${RB2}"
[[ "${RA2}" == "${RB2}" ]] || fail "nodes diverged after the reorg: ${RA2} != ${RB2}"

pass "deep reorg through $(( TIP - REORG_FROM + 1 )) blocks incl. a real shielded spend: root invertible on both nodes"
pass "state_commitment_v1 candidate root = ${RA2}"
exit 0
