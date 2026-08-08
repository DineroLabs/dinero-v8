#!/usr/bin/env bash
# Regression test for #458 — JSON-RPC envelope correctness.
#
# Handlers on the live HTTP path signal failure by RETURNING an object that
# contains "error" rather than throwing. The unified dispatcher used to place
# that object under "result" and set the top-level "error" to null:
#
#   {"error": null, "result": {"error": {"code": -32000, "message": "..."}}}
#
# which is not a valid JSON-RPC response: a hard failure is indistinguishable
# from success to any client that checks the top-level "error", as the entire
# integration suite did. RPCServer::handleSingleRequest already promoted such
# errors (rpc_server.cpp:699); the live dispatcher did not.
#
# This test pins the envelope contract structurally, with jq, rather than by
# substring matching. Substring checks such as looking for "error":{ are
# overbroad — legitimate result data, or any string value containing that text,
# would trip them.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
DATA_DIR="/tmp/dinero_rpc_envelope_$$"
LOG="${DATA_DIR}.log"
PID=""
KEEP_ON_FAIL="${KEEP_ON_FAIL:-0}"

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
fail() {
    printf '[FAIL] %s\n' "$*" >&2
    [[ -f "${LOG}" ]] && { printf -- '--- daemon log tail ---\n' >&2; tail -40 "${LOG}" >&2 || true; }
    exit 1
}

cleanup() {
    if [[ -n "${PID}" ]]; then
        kill "${PID}" 2>/dev/null || true
        # dinerod removes and may briefly recreate its cookie while shutting
        # down.  Wait for the child to finish before removing the datadir;
        # otherwise cleanup races those final writes and a passing test exits
        # non-zero with "Directory not empty".
        wait "${PID}" 2>/dev/null || true
        PID=""
    fi
    [[ "${KEEP_ON_FAIL}" != "1" ]] && rm -rf "${DATA_DIR}" "${LOG}"
    return 0
}
trap cleanup EXIT

command -v curl >/dev/null || fail "curl is required"
command -v jq >/dev/null || fail "jq is required"
[[ -x "${DINEROD}" ]] || fail "dinerod not built at ${DINEROD}"

RPC_PORT=$((49000 + RANDOM % 500))
P2P_PORT=$((RPC_PORT + 1))
WALLET_PORT=$((RPC_PORT + 2))

rpc_call() {
    local cookie_path
    if [[ -f "${DATA_DIR}/.cookie" ]]; then
        cookie_path="${DATA_DIR}/.cookie"
    elif [[ -f "${DATA_DIR}/regtest/.cookie" ]]; then
        cookie_path="${DATA_DIR}/regtest/.cookie"
    else
        return 1
    fi
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    curl -s --user "${cookie}" -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" \
        "http://127.0.0.1:${RPC_PORT}/"
}

mkdir -p "${DATA_DIR}"
"${DINEROD}" --regtest --datadir="${DATA_DIR}" --rpcport="${RPC_PORT}" \
    --port="${P2P_PORT}" --wallet-socket-port="${WALLET_PORT}" \
    --listen=0 --utreexo=1 --p2p.offline=1 >"${LOG}" 2>&1 &
PID=$!

for _ in $(seq 1 90); do
    rpc_call getblockcount '[]' | jq -e '.error == null and .result >= 0' >/dev/null 2>&1 && break
    sleep 1
done

# ─────────────────────────── success envelope ───────────────────────────
RESP="$(rpc_call getblockcount '[]')"
jq -e 'type == "object"' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "success reply is not a JSON object: ${RESP}"
jq -e 'has("result")' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "success reply has no \"result\" member: ${RESP}"
jq -e '.error == null' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "success reply has a non-null top-level \"error\": ${RESP}"
# A successful result must not itself carry an error object — that is the
# malformed shape this issue is about.
jq -e '(.result | type) != "object" or (.result | has("error") | not) or (.result.error == null)' \
    >/dev/null 2>&1 <<<"${RESP}" \
    || fail "success reply buries an error object inside \"result\": ${RESP}"
pass "success: result present, top-level error null, no error buried in result"

# ─────────────────────────── error envelope ─────────────────────────────
# Unknown method: the dispatcher's own error path.
RESP="$(rpc_call this_method_does_not_exist '[]')"
jq -e '.error != null' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "unknown method did not report a top-level error: ${RESP}"
jq -e '.error | type == "object"' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "unknown-method error is not an object: ${RESP}"
jq -e '.error | has("code")' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "unknown-method error has no code: ${RESP}"
pass "unknown method reports a structured top-level error"

# ── handler-returned error must be PROMOTED, not buried ──
# generatetoaddress with an invalid address is rejected by the handler, which
# signals failure by RETURNING an object containing "error". Before the fix,
# that landed under "result" with the top-level "error" null.
RESP="$(rpc_call generatetoaddress '[1,"not_a_valid_address"]')"
info "handler-error reply: $(echo "${RESP}" | tr -d '\n' | cut -c1-220)"

jq -e '.error != null' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "handler-reported failure was NOT promoted to the top-level \"error\". This is the #458 envelope defect: the failure is reported inside \"result\" while \"error\" stays null, so every caller that checks the top level sees success. Reply: ${RESP}"

jq -e '.error | has("code") and has("message")' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "promoted error is missing code/message: ${RESP}"

# And it must not ALSO remain buried in result.
jq -e '(.result | type) != "object" or (.result | has("error") | not)' \
    >/dev/null 2>&1 <<<"${RESP}" \
    || fail "handler error was promoted but also left inside \"result\": ${RESP}"
pass "handler-returned error is promoted to a structured top-level error"

# ── string-valued handler error must be NORMALISED, not promoted verbatim ──
# Handlers in this repo return both shapes:
#     {"error": {"code": ..., "message": ...}}
#     {"error": "some message"}
# ct.setminfee with no parameter takes the second path (ct_fee_rpc.cpp). A bare
# string promoted verbatim would produce another invalid envelope: the JSON-RPC
# error member must be an object with numeric code and string message.
RESP="$(rpc_call ct.setminfee '[]')"
info "string-error reply: $(echo "${RESP}" | tr -d '\n' | cut -c1-220)"

jq -e '.error != null' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "string-valued handler error was not promoted at all: ${RESP}"
jq -e '.error | type == "object"' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "string-valued handler error was promoted VERBATIM as a non-object, which is not a valid JSON-RPC error member: ${RESP}"
jq -e '.error.code | type == "number"' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "normalised error has no numeric code: ${RESP}"
jq -e '.error.message | type == "string"' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "normalised error has no string message: ${RESP}"
# The original text must not be lost in normalisation.
jq -e '.error.message | length > 0' >/dev/null 2>&1 <<<"${RESP}" \
    || fail "normalised error message is empty: ${RESP}"
pass "string-valued handler error is normalised into a structured error"

rpc_call stop '[]' >/dev/null 2>&1 || true
pass "#458 RPC envelope regression test completed"
