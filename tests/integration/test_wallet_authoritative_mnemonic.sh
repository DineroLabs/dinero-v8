#!/usr/bin/env bash
# Issue #520: a fresh daemon must create its default wallet from authoritative
# BIP39 recovery material, preserve that material across restart, and restore
# the exact active WalletManager identity in a separate datadir.
set -euo pipefail

# Resolve dinerod: honour $DINEROD when set (and require it to be
# executable), else fall back to the in-tree build for manual runs.
# Without this the assignment below CLOBBERED $DINEROD, so an arbitrary
# build directory could not be used and ctest failed with a path the
# caller never chose.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
else
    DINEROD="${DINEROD:?set DINEROD to the dinerod binary path}"
    # Say WHAT WAS TRIED. Naming only the resolved path reads as
    # "the build is missing" when the real cause is that $DINEROD
    # was never set and this fallback does not exist.
    [[ -x "${DINEROD}" ]] || {
        echo "dinerod not found (tried: \$DINEROD unset, ${DINEROD})" >&2
        echo "set DINEROD=/path/to/dinerod to override" >&2
        exit 1
    }
fi
ROOT="$(mktemp -d -t dinero_mnemonic_XXXXXX)"
SOURCE_DIR="${ROOT}/source"
RESTORE_DIR="${ROOT}/restore"
SOURCE_LOG="${ROOT}/source.log"
SOURCE_RESTART_LOG="${ROOT}/source-restart.log"
RESTORE_LOG="${ROOT}/restore.log"
SOURCE_PID=""
RESTORE_PID=""
KEEP_ON_FAIL=0

stop_pid() {
    local pid="$1"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null || true
        for _ in $(seq 1 100); do
            kill -0 "${pid}" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "${pid}" 2>/dev/null || true
    fi
}

stop_all() {
    stop_pid "${SOURCE_PID}"
    stop_pid "${RESTORE_PID}"
    SOURCE_PID=""
    RESTORE_PID=""
}

fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    stop_all
    for log in "${SOURCE_LOG}" "${SOURCE_RESTART_LOG}" "${RESTORE_LOG}"; do
        if [[ -f "${log}" ]]; then
            printf -- '--- %s tail ---\n' "${log}" >&2
            tail -120 "${log}" >&2 || true
        fi
    done
    exit 1
}

cleanup() {
    stop_all
    [[ "${KEEP_ON_FAIL}" -eq 0 ]] && rm -rf "${ROOT}" || true
}
trap cleanup EXIT

for tool in curl jq lsof; do
    command -v "${tool}" >/dev/null || fail "${tool} is required"
done
[[ -x "${DINEROD}" ]] || fail "dinerod not executable at ${DINEROD}"

for _ in $(seq 1 80); do
    candidate=$((36000 + RANDOM % 10000))
    collision=0
    for offset in 0 1 2 10 11 12; do
        if lsof -nP -iTCP:"$((candidate + offset))" -sTCP:LISTEN >/dev/null 2>&1; then
            collision=1
            break
        fi
    done
    if [[ "${collision}" -eq 0 ]]; then
        SOURCE_RPC="${candidate}"
        SOURCE_P2P="$((candidate + 1))"
        SOURCE_WALLET="$((candidate + 2))"
        RESTORE_RPC="$((candidate + 10))"
        RESTORE_P2P="$((candidate + 11))"
        RESTORE_WALLET="$((candidate + 12))"
        break
    fi
done
[[ -n "${SOURCE_RPC:-}" ]] || fail "no collision-free port ranges available"

cookie_file() {
    local datadir="$1"
    [[ -f "${datadir}/.cookie" ]] && { printf '%s\n' "${datadir}/.cookie"; return; }
    [[ -f "${datadir}/regtest/.cookie" ]] && { printf '%s\n' "${datadir}/regtest/.cookie"; return; }
    return 1
}

rpc_raw() {
    local datadir="$1" port="$2" method="$3" params="${4:-[]}" cookie_path cookie
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    cookie="$(tr -d '\n' < "${cookie_path}")"
    jq -nc --arg method "${method}" --argjson params "${params}" \
        '{jsonrpc:"2.0",id:1,method:$method,params:$params}' |
        curl -fsS --max-time 30 --user "${cookie}" \
            -H 'Content-Type: application/json' --data-binary @- \
            "http://127.0.0.1:${port}/"
}

wait_ready() {
    local datadir="$1" port="$2" pid="$3"
    for _ in $(seq 1 120); do
        kill -0 "${pid}" 2>/dev/null || return 1
        if rpc_raw "${datadir}" "${port}" getblockcount | jq -e '.error == null' >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

start_source() {
    local logfile="$1"
    "${DINEROD}" --regtest --datadir="${SOURCE_DIR}" \
        --rpcport="${SOURCE_RPC}" --port="${SOURCE_P2P}" \
        --wallet-socket-port="${SOURCE_WALLET}" --listen=0 \
        >"${logfile}" 2>&1 &
    SOURCE_PID=$!
    wait_ready "${SOURCE_DIR}" "${SOURCE_RPC}" "${SOURCE_PID}" \
        || fail "source daemon did not become ready"
}

start_restore() {
    "${DINEROD}" --regtest --datadir="${RESTORE_DIR}" \
        --rpcport="${RESTORE_RPC}" --port="${RESTORE_P2P}" \
        --wallet-socket-port="${RESTORE_WALLET}" --listen=0 \
        >"${RESTORE_LOG}" 2>&1 &
    RESTORE_PID=$!
    wait_ready "${RESTORE_DIR}" "${RESTORE_RPC}" "${RESTORE_PID}" \
        || fail "restore daemon did not become ready"
}

start_source "${SOURCE_LOG}"

if grep -Eq 'Failed to auto-create default wallet|Wallet already exists: default' "${SOURCE_LOG}"; then
    fail "fresh startup still follows the create-then-fail BIP39 path"
fi

EXPORT="$(rpc_raw "${SOURCE_DIR}" "${SOURCE_RPC}" wallet.exportseed)"
jq -e '
    .error == null and
    .result.authoritative == true and
    .result.backup_required == true and
    .result.backup_acknowledged == false and
    .result.passphrase_required == false and
    (.result.mnemonic | type == "string" and (split(" ") | length) == 12)
' <<<"${EXPORT}" >/dev/null || fail "fresh default wallet did not export authoritative BIP39 material: ${EXPORT}"
MNEMONIC="$(jq -r '.result.mnemonic' <<<"${EXPORT}")"

WALLET_INFO="$(rpc_raw "${SOURCE_DIR}" "${SOURCE_RPC}" wallet.getwalletinfo)"
jq -e '
    .error == null and
    .result.mnemonic_backup_available == true and
    .result.backup_required == true and
    .result.backup_acknowledged == false and
    .result.recovery_status == "mnemonic_backup_required"
' <<<"${WALLET_INFO}" >/dev/null \
    || fail "wallet status does not expose the client backup warning contract: ${WALLET_INFO}"

ALIAS_EXPORT="$(rpc_raw "${SOURCE_DIR}" "${SOURCE_RPC}" wallet.exportmnemonic)"
[[ "$(jq -r '.result.mnemonic // empty' <<<"${ALIAS_EXPORT}")" == "${MNEMONIC}" ]] \
    || fail "wallet.exportmnemonic disagrees with wallet.exportseed"
printf '[PASS] fresh default wallet exposes one authoritative mnemonic through both RPC names\n'

SOURCE_LIST="$(rpc_raw "${SOURCE_DIR}" "${SOURCE_RPC}" wallet.listaddresses)"
FIRST_ADDRESS="$(jq -r '.result[] | select(.change == 0 and .index == 0 and .type == "p2tr") | .address' <<<"${SOURCE_LIST}")"
[[ -n "${FIRST_ADDRESS}" ]] || fail "fresh wallet has no persisted BIP86 index-0 address"
for index in 1 2 3 4; do
    response="$(rpc_raw "${SOURCE_DIR}" "${SOURCE_RPC}" wallet.getnewaddress '["taproot","backup-identity"]')"
    address="$(jq -r '.result.address // empty' <<<"${response}")"
    [[ -n "${address}" ]] || fail "source external derivation ${index} failed: ${response}"
done

# Compare identities by persisted derivation index, never by the order in which
# getnewaddress happened to return them. Startup may prefill its receive window.
SOURCE_LIST="$(rpc_raw "${SOURCE_DIR}" "${SOURCE_RPC}" wallet.listaddresses)"
SOURCE_EXTERNAL="$(jq -c '[.result[] | select(.type == "p2tr" and .change == 0 and .index < 5) | {index,address}] | sort_by(.index)' <<<"${SOURCE_LIST}")"
[[ "$(jq 'length' <<<"${SOURCE_EXTERNAL}")" -eq 5 ]] \
    || fail "source wallet did not persist external indices 0..4: ${SOURCE_EXTERNAL}"

WRONG_ACK="$(rpc_raw "${SOURCE_DIR}" "${SOURCE_RPC}" wallet.acknowledgeseedbackup \
    '["abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about",false]')"
jq -e '.error != null and ((.error.message // .error // "") | contains("does not match"))' \
    <<<"${WRONG_ACK}" >/dev/null || fail "wrong mnemonic unexpectedly acknowledged: ${WRONG_ACK}"

ACK_PARAMS="$(jq -nc --arg mnemonic "${MNEMONIC}" '[ $mnemonic, false ]')"
ACK="$(rpc_raw "${SOURCE_DIR}" "${SOURCE_RPC}" wallet.acknowledgeseedbackup "${ACK_PARAMS}")"
jq -e '.error == null and .result.success == true and .result.backup_acknowledged == true' \
    <<<"${ACK}" >/dev/null || fail "correct mnemonic was not acknowledged: ${ACK}"
ACKED_INFO="$(rpc_raw "${SOURCE_DIR}" "${SOURCE_RPC}" wallet.getwalletinfo)"
jq -e '
    .error == null and
    .result.mnemonic_backup_available == true and
    .result.backup_required == false and
    .result.backup_acknowledged == true and
    .result.recovery_status == "mnemonic_acknowledged"
' <<<"${ACKED_INFO}" >/dev/null \
    || fail "wallet status did not clear the backup warning after acknowledgment: ${ACKED_INFO}"
printf '[PASS] acknowledgment rejects unrelated words and accepts the exact active mnemonic\n'

stop_pid "${SOURCE_PID}"
SOURCE_PID=""
start_source "${SOURCE_RESTART_LOG}"
RESTART_EXPORT="$(rpc_raw "${SOURCE_DIR}" "${SOURCE_RPC}" wallet.exportseed)"
jq -e --arg mnemonic "${MNEMONIC}" '
    .error == null and
    .result.mnemonic == $mnemonic and
    .result.authoritative == true and
    .result.backup_acknowledged == true and
    .result.backup_required == false
' <<<"${RESTART_EXPORT}" >/dev/null || fail "mnemonic/acknowledgment did not survive restart: ${RESTART_EXPORT}"
printf '[PASS] authoritative recovery material and acknowledgment survive restart\n'

start_restore
RESTORE_PARAMS="$(jq -nc --arg mnemonic "${MNEMONIC}" \
    '{name:"default",mnemonic:$mnemonic,passphrase:"",password:"",policy:"bip86",replace_existing:true,rescan:false}')"
RESTORED="$(rpc_raw "${RESTORE_DIR}" "${RESTORE_RPC}" wallet.restore "${RESTORE_PARAMS}")"
jq -e --arg first "${FIRST_ADDRESS}" '
    .error == null and
    .result.success == true and
    .result.first_address == $first and
    .result.mnemonic_exportable == true and
    .result.addresses_restored == 20 and
    .result.gap_limit == 20
' <<<"${RESTORED}" >/dev/null || fail "separate-datadir restore failed: ${RESTORED}"

RESTORED_LIST="$(rpc_raw "${RESTORE_DIR}" "${RESTORE_RPC}" wallet.listaddresses)"
for index in 0 1 2 3 4; do
    expected="$(jq -r --argjson index "${index}" '.[] | select(.index == $index) | .address' <<<"${SOURCE_EXTERNAL}")"
    actual="$(jq -r --argjson index "${index}" '.result[] | select(.type == "p2tr" and .change == 0 and .index == $index) | .address' <<<"${RESTORED_LIST}")"
    [[ "${actual}" == "${expected}" ]] \
        || fail "restored external index ${index} differs: ${actual} != ${expected}"
done

jq -e '
    ([.result[] | select(.type == "p2tr" and .change == 0)] | length) >= 20 and
    ([.result[] | select(.type == "p2tr" and .change == 1)] | length) == 20 and
    ([.result[] | select(.type == "p2tr" and .change == 1) | .index] | sort) == ([range(0;20)]) and
    ([.result[] | select(.type == "p2tr" and .change == 1) | .path] | all(test("^m/86.?/1448.?/0.?/1/[0-9]+$")))
' <<<"${RESTORED_LIST}" >/dev/null || fail "restore did not reproduce the full external/change gap window"

RESTORED_EXPORT="$(rpc_raw "${RESTORE_DIR}" "${RESTORE_RPC}" wallet.exportseed)"
jq -e --arg mnemonic "${MNEMONIC}" '.error == null and .result.mnemonic == $mnemonic and .result.authoritative == true' \
    <<<"${RESTORED_EXPORT}" >/dev/null || fail "restored wallet did not preserve authoritative mnemonic identity"
printf '[PASS] backup restores exact multi-index external identity and the 20-address change window\n'

printf 'ALL WALLET AUTHORITATIVE-MNEMONIC ASSERTIONS PASSED\n'
