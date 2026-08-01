#!/usr/bin/env bash
#
# Profile-v1 covenant lifecycle on live regtest daemons:
#   * construct and persist CTV/CCV recovery descriptors through RPC;
#   * fund and spend both covenant forms with ordinary wallet fee inputs;
#   * relay and mine the transactions across two nodes;
#   * restart the descriptor-owning wallet and re-derive every watched script;
#   * disconnect a confirmed CCV transition, revalidate it after a longer-fork
#     reorg, relay it again, and confirm it on the winning chain.
#
# Mainnet and testnet covenant activation remain deliberately untouched.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
BASE_PORT="${BASE_PORT:-44320}"
NODE_A_RPC=$((BASE_PORT + 0))
NODE_B_RPC=$((BASE_PORT + 1))
NODE_A_P2P=$((BASE_PORT + 100))
NODE_B_P2P=$((BASE_PORT + 101))
NODE_A_WALLET=$((BASE_PORT + 200))
NODE_B_WALLET=$((BASE_PORT + 201))
GUARD_RPC=$((BASE_PORT + 2))
GUARD_P2P=$((BASE_PORT + 102))
GUARD_WALLET=$((BASE_PORT + 202))
TEST_ROOT="$(mktemp -d "/tmp/dinero-covenant-lifecycle.XXXXXX")"
DATA_A="${TEST_ROOT}/node-a"
DATA_B="${TEST_ROOT}/node-b"
DATA_GUARD="${TEST_ROOT}/mainnet-guard"
LOG_A="${DATA_A}/daemon.log"
LOG_B="${DATA_B}/daemon.log"
LOG_GUARD="${DATA_GUARD}/daemon.log"
PID_A=""
PID_B=""
PID_GUARD=""
KEEP_ON_FAIL=0

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }

fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    if [[ -f "${LOG_A}" ]]; then
        printf -- '--- node A log tail ---\n' >&2
        tail -180 "${LOG_A}" >&2 || true
    fi
    if [[ -f "${LOG_B}" ]]; then
        printf -- '--- node B log tail ---\n' >&2
        tail -180 "${LOG_B}" >&2 || true
    fi
    if [[ -f "${LOG_GUARD}" ]]; then
        printf -- '--- mainnet guard log tail ---\n' >&2
        tail -180 "${LOG_GUARD}" >&2 || true
    fi
    exit 1
}

cookie_file() {
    local datadir="$1"
    if [[ -f "${datadir}/.cookie" ]]; then
        printf '%s\n' "${datadir}/.cookie"
    elif [[ -f "${datadir}/regtest/.cookie" ]]; then
        printf '%s\n' "${datadir}/regtest/.cookie"
    else
        return 1
    fi
}

rpc_call() {
    local port="$1"
    local datadir="$2"
    local method="$3"
    local params="${4:-[]}"
    local cookie_path
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    curl -s --user "$(tr -d '\n' < "${cookie_path}")" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params}}" \
        "http://127.0.0.1:${port}/"
}

rpc_result() {
    local port="$1"
    local datadir="$2"
    local method="$3"
    local params="${4:-[]}"
    local response
    response="$(rpc_call "${port}" "${datadir}" "${method}" "${params}")" \
        || fail "RPC transport failed: ${method}"
    jq -e '.error == null' <<<"${response}" >/dev/null \
        || fail "RPC ${method} failed: ${response}"
    jq '.result' <<<"${response}"
}

rpc_failure_result() {
    local port="$1"
    local datadir="$2"
    local method="$3"
    local params="${4:-[]}"
    local response normalized
    response="$(rpc_call "${port}" "${datadir}" "${method}" "${params}")" \
        || fail "RPC transport failed: ${method}"
    normalized="$(jq -e '
        if .error != null then
            {success:false,
             error:(.error.message? // (.error | tostring)),
             error_code:(.error.code? // null)}
        elif .result.error? != null then
            .result
        else
            empty
        end' <<<"${response}")" \
        || fail "RPC ${method} unexpectedly succeeded: ${response}"
    printf '%s\n' "${normalized}"
}

covenant_result() {
    local port="$1"
    local datadir="$2"
    local method="$3"
    local params="$4"
    local result
    result="$(rpc_result "${port}" "${datadir}" "${method}" "${params}")"
    jq -e '.success == true and .rpc_schema == "din.wallet.covenant.profile.v1"' \
        <<<"${result}" >/dev/null \
        || fail "Covenant RPC ${method} failed closed: ${result}"
    printf '%s\n' "${result}"
}

wait_rpc() {
    local port="$1"
    local datadir="$2"
    for _ in $(seq 1 60); do
        if rpc_call "${port}" "${datadir}" "getblockcount" '[]' \
            | jq -e '.error == null and .result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

height() {
    rpc_call "$1" "$2" "getblockcount" '[]' 2>/dev/null \
        | jq -r '.result // -1' 2>/dev/null || printf '%s\n' "-1"
}

best_hash() {
    rpc_call "$1" "$2" "getbestblockhash" '[]' 2>/dev/null \
        | jq -r '.result // empty' 2>/dev/null || printf '\n'
}

wait_same_tip() {
    for _ in $(seq 1 90); do
        local height_a height_b hash_a hash_b
        height_a="$(height "${NODE_A_RPC}" "${DATA_A}")"
        height_b="$(height "${NODE_B_RPC}" "${DATA_B}")"
        hash_a="$(best_hash "${NODE_A_RPC}" "${DATA_A}")"
        hash_b="$(best_hash "${NODE_B_RPC}" "${DATA_B}")"
        if [[ "${height_a}" -ge 0 &&
              "${height_a}" = "${height_b}" &&
              -n "${hash_a}" &&
              "${hash_a}" = "${hash_b}" ]]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_mempool_contains() {
    local port="$1"
    local datadir="$2"
    local txid="$3"
    for _ in $(seq 1 60); do
        if rpc_call "${port}" "${datadir}" "getrawmempool" '[]' \
            | jq -e --arg txid "${txid}" \
                '.error == null and any(.result[]; . == $txid)' \
                >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_mempool_absent() {
    local port="$1"
    local datadir="$2"
    local txid="$3"
    for _ in $(seq 1 60); do
        if rpc_call "${port}" "${datadir}" "getrawmempool" '[]' \
            | jq -e --arg txid "${txid}" \
                '.error == null and (any(.result[]; . == $txid) | not)' \
                >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

start_a() {
    mkdir -p "${DATA_A}"
    "${DINEROD}" \
        --regtest \
        --datadir="${DATA_A}" \
        --rpcport="${NODE_A_RPC}" \
        --p2pport="${NODE_A_P2P}" \
        --wallet-socket-port="${NODE_A_WALLET}" \
        --connect="127.0.0.1:${NODE_B_P2P}" \
        --listen=1 \
        >>"${LOG_A}" 2>&1 &
    PID_A=$!
    wait_rpc "${NODE_A_RPC}" "${DATA_A}" \
        || fail "node A RPC did not start"
}

start_b() {
    mkdir -p "${DATA_B}"
    "${DINEROD}" \
        --regtest \
        --datadir="${DATA_B}" \
        --rpcport="${NODE_B_RPC}" \
        --p2pport="${NODE_B_P2P}" \
        --wallet-socket-port="${NODE_B_WALLET}" \
        --connect="127.0.0.1:${NODE_A_P2P}" \
        --listen=1 \
        >>"${LOG_B}" 2>&1 &
    PID_B=$!
    wait_rpc "${NODE_B_RPC}" "${DATA_B}" \
        || fail "node B RPC did not start"
}

stop_a() {
    if [[ -n "${PID_A}" ]]; then
        rpc_call "${NODE_A_RPC}" "${DATA_A}" "stop" '[]' >/dev/null 2>&1 \
            || kill "${PID_A}" 2>/dev/null || true
        wait "${PID_A}" 2>/dev/null || true
        PID_A=""
    fi
}

stop_b() {
    if [[ -n "${PID_B}" ]]; then
        rpc_call "${NODE_B_RPC}" "${DATA_B}" "stop" '[]' >/dev/null 2>&1 \
            || kill "${PID_B}" 2>/dev/null || true
        wait "${PID_B}" 2>/dev/null || true
        PID_B=""
    fi
}

stop_guard() {
    if [[ -n "${PID_GUARD}" ]]; then
        rpc_call "${GUARD_RPC}" "${DATA_GUARD}" "stop" '[]' \
            >/dev/null 2>&1 \
            || kill "${PID_GUARD}" 2>/dev/null || true
        wait "${PID_GUARD}" 2>/dev/null || true
        PID_GUARD=""
    fi
}

cleanup() {
    stop_a
    stop_b
    stop_guard
    if [[ "${KEEP_ON_FAIL}" = "1" ]]; then
        info "Preserving failed test artifacts at ${TEST_ROOT}"
    else
        rm -rf "${TEST_ROOT}"
    fi
}
trap cleanup EXIT

wallet_address() {
    local port="$1"
    local datadir="$2"
    rpc_result "${port}" "${datadir}" "wallet.getnewaddress" '[]' \
        | jq -r '.address // . // empty'
}

mine() {
    local port="$1"
    local datadir="$2"
    local address="$3"
    local count="$4"
    rpc_result "${port}" "${datadir}" "generatetoaddress" \
        "[${count},\"${address}\"]" >/dev/null
}

spendable_utxo() {
    local excluded="${1:-}"
    rpc_result "${NODE_A_RPC}" "${DATA_A}" \
        "wallet.listunspent" '[101,9999999]' \
        | jq -c --arg excluded "${excluded}" \
            'map(select(.spendable == true and .txid != $excluded))[0]'
}

fund_script() {
    local script="$1"
    local covenant_amount="$2"
    local change_amount="$3"
    local excluded="${4:-}"
    local utxo input_params output_params raw signed txid decoded vout
    utxo="$(spendable_utxo "${excluded}")"
    [[ "$(jq -r '.txid // empty' <<<"${utxo}")" != "" ]] \
        || fail "no mature wallet UTXO available"
    input_params="$(jq -nc --argjson u "${utxo}" \
        '[{txid:$u.txid,vout:$u.vout}]')"
    output_params="$(jq -nc \
        --arg script "${script}" \
        --arg change "${CHANGE_A}" \
        --argjson covenant "${covenant_amount}" \
        --argjson change_amount "${change_amount}" \
        '[{scriptPubKey:$script,amount:$covenant},{($change):$change_amount}]')"
    raw="$(rpc_result "${NODE_A_RPC}" "${DATA_A}" \
        "wallet.createrawtransaction" \
        "[$(printf '%s' "${input_params}"),$(printf '%s' "${output_params}")]" \
        | jq -r '.hex // empty')"
    [[ -n "${raw}" ]] || fail "wallet.createrawtransaction returned no hex"
    signed="$(rpc_result "${NODE_A_RPC}" "${DATA_A}" \
        "wallet.signrawtransaction" \
        "$(jq -nc --arg raw "${raw}" --argjson u "${utxo}" \
            '[$raw,[{txid:$u.txid,vout:$u.vout,scriptPubKey:$u.scriptPubKey,amount:$u.amount}]]')")"
    [[ "$(jq -r '.complete // false' <<<"${signed}")" = "true" ]] \
        || fail "wallet did not complete covenant funding signature: ${signed}"
    txid="$(rpc_result "${NODE_A_RPC}" "${DATA_A}" \
        "wallet.sendrawtransaction" \
        "$(jq -nc --arg hex "$(jq -r '.hex' <<<"${signed}")" '[$hex]')" \
        | jq -r '.txid // .result // . // empty')"
    [[ -n "${txid}" ]] || fail "covenant funding broadcast returned no txid"
    decoded="$(rpc_result "${NODE_A_RPC}" "${DATA_A}" \
        "wallet.decoderawtransaction" \
        "$(jq -nc --arg hex "$(jq -r '.hex' <<<"${signed}")" '[$hex]')")"
    vout="$(jq -r --arg script "${script}" \
        '.vout[] | select(.scriptPubKey.hex == $script) | .n' \
        <<<"${decoded}" | head -n 1)"
    [[ -n "${vout}" ]] || fail "could not locate covenant funding output"
    printf '%s %s %s\n' "${txid}" "${vout}" "$(jq -r '.txid' <<<"${utxo}")"
}

[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"
command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"

info "Proving the wallet/RPC construction surface fails closed on mainnet"
mkdir -p "${DATA_GUARD}"
"${DINEROD}" \
    --datadir="${DATA_GUARD}" \
    --p2p.offline=1 \
    --rpcport="${GUARD_RPC}" \
    --p2pport="${GUARD_P2P}" \
    --wallet-socket-port="${GUARD_WALLET}" \
    >>"${LOG_GUARD}" 2>&1 &
PID_GUARD=$!
wait_rpc "${GUARD_RPC}" "${DATA_GUARD}" \
    || fail "mainnet guard daemon RPC did not start"
GUARD_RESPONSE="$(rpc_call "${GUARD_RPC}" "${DATA_GUARD}" \
    "wallet.covenant.inspect" \
    '[{"descriptor":"dncov1:00"}]')" \
    || fail "mainnet covenant RPC transport failed"
jq -e \
    'if .error != null then
         (.error.code == -32603) and
         (.error.message | contains("regtest-only")) and
         ((has("result") | not) or .result == null)
     else
         (.result.success == false) and
         (.result.error | contains("regtest-only")) and
         (.result.rpc_schema == "din.wallet.covenant.profile.v1")
     end' \
    <<<"${GUARD_RESPONSE}" >/dev/null \
    || fail "mainnet covenant RPC did not fail closed: ${GUARD_RESPONSE}"
stop_guard
pass "mainnet covenant wallet/RPC construction is unavailable"

info "Starting isolated regtest nodes"
start_a
start_b
rpc_result "${NODE_A_RPC}" "${DATA_A}" "wallet.createhd" \
    '["covenant-lifecycle-a"]' >/dev/null 2>&1 || true
rpc_result "${NODE_B_RPC}" "${DATA_B}" "wallet.createhd" \
    '["covenant-lifecycle-b"]' >/dev/null 2>&1 || true

info "Proving covenant spend construction fails before activation height 20"
ZERO_TXID="0000000000000000000000000000000000000000000000000000000000000000"
PRE_CTV="$(covenant_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.ctvcreate" \
    '{"outputs":[{"value_una":1,"script_pubkey":"51"}]}')"
PRE_CTV_DESCRIPTOR="$(jq -r '.recovery_descriptor' <<<"${PRE_CTV}")"
PRE_CTV_RESULT="$(rpc_failure_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.ctvspend" \
    "$(jq -nc --arg descriptor "${PRE_CTV_DESCRIPTOR}" \
        --arg txid "${ZERO_TXID}" \
        '{descriptor:$descriptor,prevouts:[{txid:$txid,vout:0}]}')")"
jq -e '.success == false and (.error | contains("before activation"))' \
    <<<"${PRE_CTV_RESULT}" >/dev/null \
    || fail "CTV spend construction did not fail closed before activation: ${PRE_CTV_RESULT}"

PRE_OWNER_CCV="$(covenant_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.ccvcreate" \
    '{"counter":0,"data_hex":"00"}')"
PRE_OWNER_CCV_DESCRIPTOR="$(jq -r '.recovery_descriptor' <<<"${PRE_OWNER_CCV}")"
jq -e '.authorization == "bip340-owner" and .permissionless == false and
       (.owner_public_key | length) == 64 and
       (.owner_key_origin | startswith("m/86'"'"'/"))' \
    <<<"${PRE_OWNER_CCV}" >/dev/null \
    || fail "default CCV creation was not wallet-owner authorized: ${PRE_OWNER_CCV}"
PRE_OWNER_RESULT="$(rpc_failure_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.ccvadvance" \
    "$(jq -nc --arg descriptor "${PRE_OWNER_CCV_DESCRIPTOR}" \
        --arg txid "${ZERO_TXID}" \
        '{descriptor:$descriptor,
          inputs:[{txid:$txid,vout:0}],
          covenant_value_una:1,
          next_data_hex:"01"}')")"
jq -e '.success == false and (.error | contains("before activation"))' \
    <<<"${PRE_OWNER_RESULT}" >/dev/null \
    || fail "owner CCV spend construction did not fail closed before activation: ${PRE_OWNER_RESULT}"
PRE_CCV="$(covenant_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.ccvcreate" \
    '{"counter":0,"data_hex":"00","permissionless":true}')"
PRE_CCV_DESCRIPTOR="$(jq -r '.recovery_descriptor' <<<"${PRE_CCV}")"
PRE_CCV_RESULT="$(rpc_failure_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.ccvadvance" \
    "$(jq -nc --arg descriptor "${PRE_CCV_DESCRIPTOR}" \
        --arg txid "${ZERO_TXID}" \
        '{descriptor:$descriptor,
          permissionless:true,
          inputs:[{txid:$txid,vout:0}],
          covenant_value_una:1,
          next_data_hex:"01"}')")"
jq -e '.success == false and (.error | contains("before activation"))' \
    <<<"${PRE_CCV_RESULT}" >/dev/null \
    || fail "CCV spend construction did not fail closed before activation: ${PRE_CCV_RESULT}"
pass "pre-activation spends fail closed and default CCV construction is owner-authorized"

MINER_A="$(wallet_address "${NODE_A_RPC}" "${DATA_A}")"
MINER_B="$(wallet_address "${NODE_B_RPC}" "${DATA_B}")"
CHANGE_A="$(wallet_address "${NODE_A_RPC}" "${DATA_A}")"
CTV_RECIPIENT="$(wallet_address "${NODE_A_RPC}" "${DATA_A}")"
CCV_CHANGE="$(wallet_address "${NODE_A_RPC}" "${DATA_A}")"
[[ -n "${MINER_A}" && -n "${MINER_B}" && -n "${CHANGE_A}" &&
   -n "${CTV_RECIPIENT}" && -n "${CCV_CHANGE}" ]] \
    || fail "wallet address creation failed"

rpc_result "${NODE_B_RPC}" "${DATA_B}" "addnode" \
    "[\"127.0.0.1:${NODE_A_P2P}\",\"add\"]" >/dev/null
for _ in $(seq 1 45); do
    connections="$(rpc_result "${NODE_B_RPC}" "${DATA_B}" \
        "getconnectioncount" '[]' | jq -r '.')"
    [[ "${connections}" -ge 1 ]] && break
    sleep 1
done
[[ "${connections:-0}" -ge 1 ]] || fail "node B did not connect to node A"

info "Mining a mature shared base beyond covenant activation height 20"
mine "${NODE_A_RPC}" "${DATA_A}" "${MINER_A}" 105
wait_same_tip || fail "nodes did not synchronize the mature base"
pass "two-node base synchronized at height $(height "${NODE_A_RPC}" "${DATA_A}")"

CTV_PLAN="$(covenant_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.ctvcreate" \
    "$(jq -nc --arg address "${CTV_RECIPIENT}" \
        '{outputs:[{value_una:100000000,address:$address}],track:true,label:"live-ctv"}')")"
CTV_DESCRIPTOR="$(jq -r '.recovery_descriptor' <<<"${CTV_PLAN}")"
CTV_ID="$(jq -r '.descriptor_id' <<<"${CTV_PLAN}")"
CTV_SCRIPT="$(jq -r '.taproot.script_pubkey' <<<"${CTV_PLAN}")"
read -r CTV_FUND_TXID CTV_FUND_VOUT FIRST_COINBASE_TXID < <(
    fund_script "${CTV_SCRIPT}" "1.001" "98.998")
wait_mempool_contains "${NODE_B_RPC}" "${DATA_B}" "${CTV_FUND_TXID}" \
    || fail "CTV funding transaction did not relay"
mine "${NODE_A_RPC}" "${DATA_A}" "${MINER_A}" 1
wait_same_tip || fail "nodes did not synchronize CTV funding"

CTV_SPEND="$(covenant_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.ctvspend" \
    "$(jq -nc --arg descriptor "${CTV_DESCRIPTOR}" \
        --arg txid "${CTV_FUND_TXID}" --argjson vout "${CTV_FUND_VOUT}" \
        '{descriptor:$descriptor,prevouts:[{txid:$txid,vout:$vout}]}')")"
CTV_SPEND_HEX="$(jq -r '.hex' <<<"${CTV_SPEND}")"
CTV_SPEND_TXID="$(rpc_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.sendrawtransaction" \
    "$(jq -nc --arg hex "${CTV_SPEND_HEX}" '[$hex]')" \
    | jq -r '.txid // .result // . // empty')"
[[ "${CTV_SPEND_TXID}" = "$(jq -r '.txid' <<<"${CTV_SPEND}")" ]] \
    || fail "CTV RPC txid disagrees with accepted transaction"
wait_mempool_contains "${NODE_B_RPC}" "${DATA_B}" "${CTV_SPEND_TXID}" \
    || fail "CTV spend did not relay to node B"
mine "${NODE_B_RPC}" "${DATA_B}" "${MINER_B}" 1
wait_same_tip || fail "nodes did not synchronize mined CTV spend"
wait_mempool_absent "${NODE_A_RPC}" "${DATA_A}" "${CTV_SPEND_TXID}" \
    || fail "CTV spend remained in node A mempool after confirmation"
pass "CTV descriptor funded, spent, relayed, and confirmed"

CCV_PLAN="$(covenant_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.ccvcreate" \
    '{"counter":7,"data_hex":"c0ffee","track":true,"label":"live-ccv-7"}')"
jq -e '.authorization == "bip340-owner" and .permissionless == false' \
    <<<"${CCV_PLAN}" >/dev/null \
    || fail "live CCV profile is not owner-authorized: ${CCV_PLAN}"
CCV_DESCRIPTOR="$(jq -r '.recovery_descriptor' <<<"${CCV_PLAN}")"
CCV_ID="$(jq -r '.descriptor_id' <<<"${CCV_PLAN}")"
CCV_SCRIPT="$(jq -r '.taproot.script_pubkey' <<<"${CCV_PLAN}")"
CCV_OWNER_KEY="$(jq -r '.owner_public_key' <<<"${CCV_PLAN}")"
CCV_OWNER_ORIGIN="$(jq -r '.owner_key_origin' <<<"${CCV_PLAN}")"
read -r CCV_FUND_TXID CCV_FUND_VOUT SECOND_COINBASE_TXID < <(
    fund_script "${CCV_SCRIPT}" "2.0" "97.999" "${FIRST_COINBASE_TXID}")
wait_mempool_contains "${NODE_B_RPC}" "${DATA_B}" "${CCV_FUND_TXID}" \
    || fail "CCV funding transaction did not relay"
mine "${NODE_A_RPC}" "${DATA_A}" "${MINER_A}" 1
wait_same_tip || fail "nodes did not synchronize CCV funding"
CCV_BASE_HEIGHT="$(height "${NODE_A_RPC}" "${DATA_A}")"
CCV_BASE_HASH="$(best_hash "${NODE_A_RPC}" "${DATA_A}")"
pass "CCV state output confirmed on the shared base at height ${CCV_BASE_HEIGHT}"

info "Stopping node B before the CCV transition to create a competing fork"
stop_b
FEE_UTXO="$(spendable_utxo "${SECOND_COINBASE_TXID}")"
[[ "$(jq -r '.txid // empty' <<<"${FEE_UTXO}")" != "" ]] \
    || fail "no mature fee UTXO available for CCV transition"
CCV_ADVANCE_PARAMS="$(jq -nc \
    --arg descriptor "${CCV_DESCRIPTOR}" \
    --arg covenant_txid "${CCV_FUND_TXID}" \
    --argjson covenant_vout "${CCV_FUND_VOUT}" \
    --argjson fee "${FEE_UTXO}" \
    --arg change "${CCV_CHANGE}" \
    '{descriptor:$descriptor,
      inputs:[
        {txid:$covenant_txid,vout:$covenant_vout},
        {txid:$fee.txid,vout:$fee.vout,
         prevout_value_una:(($fee.amount * 100000000) | round),
         script_pubkey:$fee.scriptPubKey}
      ],
      covenant_value_una:200000000,
      next_data_hex:"facade",
      outputs:[{value_una:9999900000,address:$change}],
      track_successor:true,
      label:"live-ccv-8"}')"
CCV_ADVANCE="$(covenant_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.ccvadvance" "${CCV_ADVANCE_PARAMS}")"
CCV_SUCCESSOR_DESCRIPTOR="$(jq -r '.successor.recovery_descriptor' \
    <<<"${CCV_ADVANCE}")"
CCV_SUCCESSOR_ID="$(jq -r '.successor.descriptor_id' <<<"${CCV_ADVANCE}")"
CCV_SUCCESSOR_SCRIPT="$(jq -r '.successor.taproot.script_pubkey' \
    <<<"${CCV_ADVANCE}")"
CCV_UNSIGNED_HEX="$(jq -r '.hex' <<<"${CCV_ADVANCE}")"
CCV_SIGNED="$(rpc_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.signrawtransaction" \
    "$(jq -nc \
        --arg hex "${CCV_UNSIGNED_HEX}" \
        --arg covenant_txid "${CCV_FUND_TXID}" \
        --argjson covenant_vout "${CCV_FUND_VOUT}" \
        --arg covenant_script "${CCV_SCRIPT}" \
        --argjson fee "${FEE_UTXO}" \
        '[$hex,[
          {txid:$covenant_txid,vout:$covenant_vout,
           scriptPubKey:$covenant_script,amount:2.0},
          {txid:$fee.txid,vout:$fee.vout,
           scriptPubKey:$fee.scriptPubKey,amount:$fee.amount}
        ]]')")"
[[ "$(jq -r '.complete // false' <<<"${CCV_SIGNED}")" = "true" ]] \
    || fail "wallet signer did not preserve CCV witness and sign fee input: ${CCV_SIGNED}"
CCV_SIGNED_HEX="$(jq -r '.hex // empty' <<<"${CCV_SIGNED}")"
CCV_TXID="$(rpc_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.sendrawtransaction" \
    "$(jq -nc --arg hex "${CCV_SIGNED_HEX}" '[$hex]')" \
    | jq -r '.txid // .result // . // empty')"
[[ "${CCV_TXID}" = "$(jq -r '.txid' <<<"${CCV_ADVANCE}")" ]] \
    || fail "CCV txid changed while signing witness inputs"
mine "${NODE_A_RPC}" "${DATA_A}" "${MINER_A}" 1
SHORT_HEIGHT="$(height "${NODE_A_RPC}" "${DATA_A}")"
[[ "${SHORT_HEIGHT}" -eq $((CCV_BASE_HEIGHT + 1)) ]] \
    || fail "CCV transition branch height mismatch"
pass "CCV transition with wallet fee input confirmed on the short branch"

info "Restarting from the shared base and mining a longer competing branch"
stop_a
start_b
[[ "$(height "${NODE_B_RPC}" "${DATA_B}")" -eq "${CCV_BASE_HEIGHT}" ]] \
    || fail "node B did not restart at the shared CCV base"
[[ "$(best_hash "${NODE_B_RPC}" "${DATA_B}")" = "${CCV_BASE_HASH}" ]] \
    || fail "node B shared-base hash changed across restart"
mine "${NODE_B_RPC}" "${DATA_B}" "${MINER_B}" 2
WINNING_HEIGHT="$(height "${NODE_B_RPC}" "${DATA_B}")"
[[ "${WINNING_HEIGHT}" -eq $((CCV_BASE_HEIGHT + 2)) ]] \
    || fail "node B did not build the expected longer fork"

start_a
RECOVERED="$(covenant_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.list" '{}')"
[[ "$(jq -r '.count' <<<"${RECOVERED}")" -eq 3 ]] \
    || fail "expected CTV, CCV, and CCV-successor descriptors after restart: ${RECOVERED}"
for descriptor_id in "${CTV_ID}" "${CCV_ID}" "${CCV_SUCCESSOR_ID}"; do
    jq -e --arg id "${descriptor_id}" \
        'any(.descriptors[]; .descriptor_id == $id)' \
        <<<"${RECOVERED}" >/dev/null \
        || fail "descriptor ${descriptor_id} was not recovered after restart"
done
RECOVERED_SUCCESSOR="$(covenant_result "${NODE_A_RPC}" "${DATA_A}" \
    "wallet.covenant.inspect" \
    "$(jq -nc --arg descriptor "${CCV_SUCCESSOR_DESCRIPTOR}" \
        '{descriptor:$descriptor}')")"
[[ "$(jq -r '.taproot.script_pubkey' <<<"${RECOVERED_SUCCESSOR}")" = "${CCV_SUCCESSOR_SCRIPT}" ]] \
    || fail "recovered CCV successor script changed after restart"
[[ "$(jq -r '.owner_public_key' <<<"${RECOVERED_SUCCESSOR}")" = "${CCV_OWNER_KEY}" &&
   "$(jq -r '.owner_key_origin' <<<"${RECOVERED_SUCCESSOR}")" = "${CCV_OWNER_ORIGIN}" ]] \
    || fail "recovered CCV successor lost its owner key or derivation origin"
jq -e --arg id "${CCV_SUCCESSOR_ID}" \
    'any(.descriptors[];
         .descriptor_id == $id and
         .profile == "ccv-owner" and
         .authorization == "bip340-owner" and
         .permissionless == false)' \
    <<<"${RECOVERED}" >/dev/null \
    || fail "wallet list did not preserve owner authorization metadata"
pass "wallet restart recovered all checksummed descriptors and watch scripts"

rpc_result "${NODE_A_RPC}" "${DATA_A}" "addnode" \
    "[\"127.0.0.1:${NODE_B_P2P}\",\"onetry\"]" >/dev/null
wait_same_tip || fail "nodes did not converge to the longer covenant reorg branch"
[[ "$(height "${NODE_A_RPC}" "${DATA_A}")" -eq "${WINNING_HEIGHT}" ]] \
    || fail "node A did not reorg away from the short CCV branch"
wait_mempool_contains "${NODE_A_RPC}" "${DATA_A}" "${CCV_TXID}" \
    || fail "disconnected CCV transition was not revalidated into node A mempool"
# Reorg reconciliation intentionally restores with relay=false: it is not a
# new transaction announcement. Re-submit the identical bytes through node B
# to prove independent post-reorg admission and normal peer propagation.
REORG_REBROADCAST_TXID="$(rpc_result "${NODE_B_RPC}" "${DATA_B}" \
    "wallet.sendrawtransaction" \
    "$(jq -nc --arg hex "${CCV_SIGNED_HEX}" '[$hex]')" \
    | jq -r '.txid // .result // . // empty')"
[[ "${REORG_REBROADCAST_TXID}" = "${CCV_TXID}" ]] \
    || fail "node B did not accept the revalidated CCV transition after reorg"
wait_mempool_contains "${NODE_B_RPC}" "${DATA_B}" "${CCV_TXID}" \
    || fail "CCV transition was absent after explicit post-reorg rebroadcast"
mine "${NODE_B_RPC}" "${DATA_B}" "${MINER_B}" 1
wait_same_tip || fail "nodes did not converge after reconfirming CCV transition"
wait_mempool_absent "${NODE_A_RPC}" "${DATA_A}" "${CCV_TXID}" \
    || fail "CCV transition remained in mempool after reconfirmation"

FINAL_HASH="$(best_hash "${NODE_B_RPC}" "${DATA_B}")"
FINAL_BLOCK="$(rpc_result "${NODE_B_RPC}" "${DATA_B}" \
    "getblock" "$(jq -nc --arg hash "${FINAL_HASH}" '[$hash,1]')")"
jq -e --arg txid "${CCV_TXID}" \
    'any(.tx[]; . == $txid)' <<<"${FINAL_BLOCK}" >/dev/null \
    || fail "CCV transition was absent from the canonical winning block"

pass "CCV transition survived wallet signing, restart, reorg revalidation, relay, and reconfirmation"
pass "profile-v1 covenant wallet/RPC multi-node lifecycle completed"
