#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  RELAY_CLI_ARGS="-testnet -rpcport=..." \
  TARGET_CLI_ARGS="-testnet -rpcport=..." \
  ORIGIN_CLI_ARGS="-testnet -rpcport=..." \
  RELAY_ENDPOINT="<relay-host-or-ip>:<relay-p2p-port>" \
    scripts/deploy/rc9_nat_relay_testnet_probe.sh

Optional:
  DINERO_CLI=/path/to/dinero-cli
  RELAY_LOG=/path/to/relay.log
  TARGET_LOG=/path/to/target.log
  ORIGIN_LOG=/path/to/origin.log
  SKIP_LOG_CHECKS=1

The script is read-only. It assumes the rc9 testnet relay topology is already
running:
  R = public relay node
  T = NAT'd target node with --relayregister=<R>
  O = origin node connected to R after T registered

It validates the observable RPC/log state required before the future
MainnetRelayReady() gate flip.
USAGE
}

die() {
    printf '[FAIL] %s\n' "$*" >&2
    exit 1
}

pass() {
    printf '[PASS] %s\n' "$*"
}

info() {
    printf '[INFO] %s\n' "$*"
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "$1 is required"
}

require_env() {
    local name="$1"
    [[ -n "${!name:-}" ]] || die "$name is required; run with --help for usage"
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi

DINERO_CLI="${DINERO_CLI:-dinero-cli}"
SKIP_LOG_CHECKS="${SKIP_LOG_CHECKS:-0}"

require_cmd jq
require_cmd "$DINERO_CLI"
require_env RELAY_CLI_ARGS
require_env TARGET_CLI_ARGS
require_env ORIGIN_CLI_ARGS
require_env RELAY_ENDPOINT

split_relay_endpoint() {
    local endpoint="$1"
    local host port
    if [[ "$endpoint" == \[*\]:* ]]; then
        host="${endpoint%%]*}"
        host="${host#[}"
        port="${endpoint##*:}"
    else
        host="${endpoint%:*}"
        port="${endpoint##*:}"
    fi
    [[ -n "$host" && -n "$port" && "$host" != "$port" ]] ||
        die "RELAY_ENDPOINT must be host:port or [ipv6]:port"
    [[ "$port" =~ ^[0-9]+$ ]] || die "RELAY_ENDPOINT port must be numeric"
    RELAY_HOST="$host"
    RELAY_PORT="$port"
}

rpc() {
    local role="$1"
    local method="$2"
    local args_var="${role}_CLI_ARGS"
    local args="${!args_var}"

    # Intentional word splitting: CLI args are passed as a shell-style list,
    # for example "-testnet -rpcport=21998 -datadir=/var/lib/dinero-testnet".
    # shellcheck disable=SC2086
    "$DINERO_CLI" $args "$method"
}

json_has_result_or_self() {
    jq -e 'if type == "object" and has("result") then .result else . end' >/dev/null
}

rpc_json() {
    local role="$1"
    local method="$2"
    local out
    out="$(rpc "$role" "$method")" || die "$role $method RPC failed"
    jq -e . >/dev/null <<<"$out" || die "$role $method did not return JSON: $out"
    json_has_result_or_self <<<"$out" || die "$role $method JSON has no usable result"
    printf '%s\n' "$out"
}

expect_jq() {
    local label="$1"
    local filter="$2"
    local json="$3"
    jq -e "$filter" >/dev/null <<<"$json" || die "$label"
    pass "$label"
}

rpc_result_filter='def rpcresult:
  if type == "object" and has("result") then .result else . end;'

expect_relay_localaddress() {
    jq -e --arg host "$RELAY_HOST" --argjson port "$RELAY_PORT" \
        "${rpc_result_filter}"'
        any(((rpcresult).localaddresses // [])[]?;
          ((.address // "") == $host) and ((.port // 0) == $port))' \
        >/dev/null <<<"$RELAY_NETINFO" ||
        die "relay advertises configured endpoint"
    pass "relay advertises configured endpoint"
}

expect_log() {
    local label="$1"
    local logfile="$2"
    local pattern="$3"
    [[ "$SKIP_LOG_CHECKS" == "1" ]] && {
        info "skipping log check: $label"
        return 0
    }
    [[ -n "$logfile" ]] || {
        info "no log provided for: $label"
        return 0
    }
    [[ -f "$logfile" ]] || die "$label log does not exist: $logfile"
    grep -F "$pattern" "$logfile" >/dev/null || die "$label missing pattern: $pattern"
    pass "$label"
}

split_relay_endpoint "$RELAY_ENDPOINT"

info "probing relay endpoint $RELAY_HOST:$RELAY_PORT"

RELAY_NETINFO="$(rpc_json RELAY getnetworkinfo)"
TARGET_NETINFO="$(rpc_json TARGET getnetworkinfo)"
ORIGIN_NETINFO="$(rpc_json ORIGIN getnetworkinfo)"

for role in RELAY TARGET ORIGIN; do
    json_var="${role}_NETINFO"
    expect_jq "$role is on testnet" \
        "${rpc_result_filter}"' ((rpcresult).network // "") == "testnet"' \
        "${!json_var}"
done

expect_jq "relay is listening" \
    "${rpc_result_filter}"' ((rpcresult).listen // false) == true' \
    "$RELAY_NETINFO"

expect_relay_localaddress

STUN_MESSAGE="$(jq -r "${rpc_result_filter}"' (rpcresult).stun.message // "missing"' <<<"$RELAY_NETINFO")"
STUN_ADDR="$(jq -r "${rpc_result_filter}"' (rpcresult).stun.discovered_address // ""' <<<"$RELAY_NETINFO")"
info "relay STUN status: message=${STUN_MESSAGE} discovered_address=${STUN_ADDR:-none}"

RELAY_PEERS="$(rpc_json RELAY getpeerinfo)"
TARGET_PEERS="$(rpc_json TARGET getpeerinfo)"
ORIGIN_PEERS="$(rpc_json ORIGIN getpeerinfo)"

expect_jq "relay sees at least two peers" \
    "${rpc_result_filter}"' [(rpcresult[]?)] | length >= 2' \
    "$RELAY_PEERS"

expect_jq "origin has outbound virtual relay peer" \
    "${rpc_result_filter}"' [(rpcresult[]?) | select((.inbound == false) and
      ((.addr // "") | startswith("relay:")))] | length >= 1' \
    "$ORIGIN_PEERS"

expect_jq "target has inbound virtual relay peer" \
    "${rpc_result_filter}"' [(rpcresult[]?) | select((.inbound == true) and
      ((.addr // "") | startswith("relay:in:")))] | length >= 1' \
    "$TARGET_PEERS"

expect_log "target sent relay register" "${TARGET_LOG:-}" \
    "[P2P] relay-register: sent to"
expect_log "relay accepted target registration" "${RELAY_LOG:-}" \
    "[P2P] relayreg: registered"
expect_log "relay advertised registered target" "${RELAY_LOG:-}" \
    "[P2P] relay-hints: advertised registered target"
expect_log "origin ingested relay hint" "${ORIGIN_LOG:-}" \
    "[P2P] relay-hints: ingested"
expect_log "relay opened circuit" "${RELAY_LOG:-}" \
    "[P2P] relaycon: opened circuit"
expect_log "origin installed outbound virtual relay peer" "${ORIGIN_LOG:-}" \
    "[P2P] relay-orchestrator: opened circuit"
expect_log "target observed inbound virtual relay peer" "${TARGET_LOG:-}" \
    "[P2P] relay-data: created inbound virtual peer"

pass "RC9 NAT relay testnet probe passed"
