#!/usr/bin/env bash
# First run from a snapshot: no wallet may be created or restored while the
# node is still loading its starting snapshot.
#
# Before the snapshot is imported the selected height is a placeholder (0).
# wallet.createhd sealed that as the wallet's birth, below the snapshot base,
# where neither the Orchard receiver check nor the snapshot-base origin
# applies, so the wallet never recovered its coins. wallet.restore switched the
# selected wallet against the same placeholder height, and the daemon's
# implicit "default" wallet was created with birth 0.
#
# A fresh regtest datadir started with a header-only snapshot stays Pending
# indefinitely (offline, its base header never arrives). In that state:
#   - no implicit default wallet is created,
#   - wallet.createhd refuses with "still loading its starting snapshot",
#   - wallet.restore refuses for rescan=false and rescan=true,
#   - listwallets stays empty.
# Control: a fresh regtest datadir WITHOUT a snapshot still auto-creates
# "default" and accepts wallet.createhd (full sync / regtest are unaffected).
#
# Requires: curl, jq, python3. DINEROD env or ${ROOT_DIR}/build/dinerod.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
[[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}" >&2; exit 2; }
for tool in curl jq python3; do
    command -v "${tool}" >/dev/null || { echo "${tool} is required" >&2; exit 2; }
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/snapshot_pending_wallet_guard.XXXXXX")"
PIDS=()
cleanup() {
    for pid in "${PIDS[@]:-}"; do [[ -n "${pid}" ]] && kill "${pid}" 2>/dev/null; done
    for pid in "${PIDS[@]:-}"; do [[ -n "${pid}" ]] && wait "${pid}" 2>/dev/null; done
    rm -rf "${WORK}"
}
trap cleanup EXIT

FAILURES=0
pass() { echo "[PASS] $*"; }
fail() { echo "[FAIL] $*"; FAILURES=$((FAILURES + 1)); }

BASE_PORT=$(( 30000 + (RANDOM % 8000) ))

rpc_call() {
    local port="$1" datadir="$2" method="$3" params="${4:-[]}"
    curl -s --max-time 30 --user "$(tr -d '\n' < "${datadir}/.cookie")" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${port}/"
}

start_node() {   # datadir rpc_port [extra args...]
    local datadir="$1" port="$2"; shift 2
    mkdir -p "${datadir}"
    "${DINEROD}" --regtest --datadir="${datadir}" --rpcport="${port}" --port=$((port + 1)) \
        --stratumport=$((port + 2)) --wallet-socket-port=$((port + 3)) --listen=0 "$@" \
        > "${datadir}.log" 2>&1 &
    PIDS+=("$!")
    local i
    for (( i = 0; i < 60; i++ )); do
        if [[ -f "${datadir}/.cookie" ]] && rpc_call "${port}" "${datadir}" getblockcount >/dev/null 2>&1; then
            # Let wallet startup (the implicit-default decision) finish.
            sleep 3
            return 0
        fi
        sleep 1
    done
    echo "node at ${datadir} did not start; log tail:" >&2
    tail -n 40 "${datadir}.log" >&2
    return 1
}

MNEMONIC="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"

# --- Pending: a header-only snapshot that can never load offline ------------
SNAP="${WORK}/header-only.dat"
python3 - "${SNAP}" <<'EOF'
import os, struct, sys
# SnapshotMetadata: magic, version, block_hash[32], block_height, utxo_count,
# timestamp, reserved (little-endian). Version 6, base height 500.
header = struct.pack('<II', 0x4F545855, 6) + os.urandom(32) + struct.pack('<IQQQ', 500, 1, 0, 0)
open(sys.argv[1], 'wb').write(header)
EOF
P_DIR="${WORK}/pending"; P_PORT=${BASE_PORT}
start_node "${P_DIR}" "${P_PORT}" --assumeutxo_snapshot="${SNAP}" || exit 1

if grep -aq "\[snapshot\] pending" "${P_DIR}.log"; then
    pass "node is in snapshot bootstrap Pending"
else
    echo "[SETUP-ERROR] node did not enter snapshot Pending; the test cannot exercise the guard" >&2
    tail -n 40 "${P_DIR}.log" >&2
    exit 1
fi

wallets=$(rpc_call "${P_PORT}" "${P_DIR}" listwallets | jq -c '.result')
[[ "${wallets}" == "[]" ]] && pass "no implicit default wallet while Pending" \
    || fail "implicit wallet created while Pending: ${wallets}"

check_refused() {   # label method params
    local label="$1" method="$2" params="$3" reply msg
    reply=$(rpc_call "${P_PORT}" "${P_DIR}" "${method}" "${params}")
    msg=$(echo "${reply}" | jq -r '.error.message // .result.error // empty')
    if [[ "${msg}" == *"still loading its starting snapshot"* ]]; then
        pass "${label} refused while Pending"
    else
        fail "${label} not refused while Pending: $(echo "${reply}" | jq -c '.' | cut -c1-300)"
    fi
}
check_refused "wallet.createhd" wallet.createhd '["early"]'
check_refused "wallet.restore rescan=false" wallet.restore \
    "{\"name\":\"early_restore\",\"mnemonic\":\"${MNEMONIC}\",\"rescan\":false}"
check_refused "wallet.restore rescan=true" wallet.restore \
    "{\"name\":\"early_restore2\",\"mnemonic\":\"${MNEMONIC}\"}"

wallets=$(rpc_call "${P_PORT}" "${P_DIR}" listwallets | jq -c '.result')
[[ "${wallets}" == "[]" ]] && pass "no wallet exists after the refused calls" \
    || fail "a wallet exists after the refused calls: ${wallets}"

# --- Control: no snapshot, height 0 is a real birth -------------------------
C_DIR="${WORK}/control"; C_PORT=$((BASE_PORT + 10))
start_node "${C_DIR}" "${C_PORT}" || exit 1
wallets=$(rpc_call "${C_PORT}" "${C_DIR}" listwallets | jq -c '[.result[].name]')
[[ "${wallets}" == '["default"]' ]] && pass "control: implicit default wallet still created" \
    || fail "control: expected the implicit default wallet, got ${wallets}"
created=$(rpc_call "${C_PORT}" "${C_DIR}" wallet.createhd '["late"]' | jq -r '.result.wallet_name // .result.name // .error.message')
[[ "${created}" == "late" ]] && pass "control: wallet.createhd still works without a snapshot" \
    || fail "control: wallet.createhd failed without a snapshot: ${created}"

if (( FAILURES > 0 )); then
    echo "${FAILURES} check(s) failed"
    exit 1
fi
echo "all checks passed"
