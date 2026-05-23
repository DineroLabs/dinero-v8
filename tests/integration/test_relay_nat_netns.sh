#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="${DINEROD:-${ROOT_DIR}/build/dinerod}"
if [[ ! -x "${DINEROD}" && -x "${ROOT_DIR}/build-release/dinerod" ]]; then
    DINEROD="${ROOT_DIR}/build-release/dinerod"
fi

SKIP_CODE=77
BASE_PORT="${BASE_PORT:-38600}"
ORIGIN_RPC=$((BASE_PORT + 1))
RELAY_RPC=$((BASE_PORT + 2))
TARGET_RPC=$((BASE_PORT + 3))
ORIGIN_P2P=$((BASE_PORT + 101))
RELAY_P2P=$((BASE_PORT + 102))
TARGET_P2P=$((BASE_PORT + 103))

WAN_CIDR="10.88.0.0/24"
RELAY_IP="10.88.0.10"
ORIGIN_NAT_WAN_IP="10.88.0.2"
TARGET_NAT_WAN_IP="10.88.0.3"
ORIGIN_NAT_LAN_IP="10.88.1.1"
ORIGIN_IP="10.88.1.2"
TARGET_NAT_LAN_IP="10.88.2.1"
TARGET_IP="10.88.2.2"

TAG="${DINERO_NETNS_TAG:-$$}"
TAG="$(printf '%s' "${TAG}" | tr -cd '[:alnum:]' | cut -c1-6)"
if [[ -z "${TAG}" ]]; then
    TAG="dn$$"
fi

NS_ORIGIN="dn_o_${TAG}"
NS_RELAY="dn_r_${TAG}"
NS_TARGET="dn_t_${TAG}"
NS_NAT_ORIGIN="dn_no_${TAG}"
NS_NAT_TARGET="dn_nt_${TAG}"
BRIDGE="dnbr${TAG}"

RUN_DIR="${DINERO_NETNS_RUN_DIR:-/tmp/dinero_relay_nat_netns_${TAG}}"
DATA_ORIGIN="${RUN_DIR}/origin"
DATA_RELAY="${RUN_DIR}/relay"
DATA_TARGET="${RUN_DIR}/target"
LOG_ORIGIN="${RUN_DIR}/origin.log"
LOG_RELAY="${RUN_DIR}/relay.log"
LOG_TARGET="${RUN_DIR}/target.log"
KEEP_ON_FAIL=0
PIDS=()

info() { printf '[INFO] %s\n' "$*"; }
pass() { printf '[PASS] %s\n' "$*"; }
skip() {
    printf '[SKIP] %s\n' "$*"
    exit "${SKIP_CODE}"
}
fail() {
    KEEP_ON_FAIL=1
    printf '[FAIL] %s\n' "$*" >&2
    for log in "${LOG_ORIGIN}" "${LOG_RELAY}" "${LOG_TARGET}"; do
        if [[ -f "${log}" ]]; then
            printf -- '--- %s tail ---\n' "${log}" >&2
            tail -100 "${log}" >&2 || true
        fi
    done
    printf '[INFO] Preserved run directory: %s\n' "${RUN_DIR}" >&2
    exit 1
}

cleanup() {
    set +e
    for pid in "${PIDS[@]:-}"; do
        kill "${pid}" 2>/dev/null || true
    done
    sleep 1
    for pid in "${PIDS[@]:-}"; do
        kill -9 "${pid}" 2>/dev/null || true
    done
    if command -v ip >/dev/null 2>&1; then
        for ns in "${NS_ORIGIN}" "${NS_RELAY}" "${NS_TARGET}" "${NS_NAT_ORIGIN}" "${NS_NAT_TARGET}"; do
            if ip netns list 2>/dev/null | awk '{print $1}' | grep -qx "${ns}"; then
                for pid in $(ip netns pids "${ns}" 2>/dev/null); do
                    kill "${pid}" 2>/dev/null || true
                done
                ip netns delete "${ns}" 2>/dev/null || true
            fi
        done
        ip link delete "${BRIDGE}" 2>/dev/null || true
    fi
    if [[ "${KEEP_ON_FAIL}" != "1" ]]; then
        rm -rf "${RUN_DIR}"
    fi
}
trap cleanup EXIT

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || skip "$1 is required"
}

if [[ "$(uname -s)" != "Linux" ]]; then
    skip "Linux network namespaces are required"
fi
if [[ "${DINERO_RUN_NETNS_NAT:-0}" != "1" ]]; then
    skip "set DINERO_RUN_NETNS_NAT=1 to run the privileged relay NAT harness"
fi
if [[ "$(id -u)" -ne 0 ]]; then
    skip "root privileges are required for ip netns and NAT firewall setup"
fi

require_cmd ip
require_cmd iptables
require_cmd curl
require_cmd jq
require_cmd timeout
require_cmd awk
require_cmd grep
require_cmd tail
require_cmd bash
[[ -x "${DINEROD}" ]] || skip "dinerod not built at ${DINEROD}"

cookie_file() {
    local datadir="$1"
    if [[ -f "${datadir}/.cookie" ]]; then
        printf '%s\n' "${datadir}/.cookie"
        return 0
    fi
    if [[ -f "${datadir}/regtest/.cookie" ]]; then
        printf '%s\n' "${datadir}/regtest/.cookie"
        return 0
    fi
    return 1
}

rpc_call() {
    local ns="$1"
    local rpc_port="$2"
    local datadir="$3"
    local method="$4"
    local params_json="$5"
    local cookie_path
    cookie_path="$(cookie_file "${datadir}" 2>/dev/null || true)"
    [[ -n "${cookie_path}" ]] || return 1
    local cookie
    cookie="$(tr -d '\n' < "${cookie_path}")"
    [[ -n "${cookie}" ]] || return 1
    ip netns exec "${ns}" curl -sS --fail --user "${cookie}" \
        -H 'Content-Type: application/json' \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
        "http://127.0.0.1:${rpc_port}/"
}

wait_rpc() {
    local ns="$1"
    local rpc_port="$2"
    local datadir="$3"
    for _ in $(seq 1 90); do
        if rpc_call "${ns}" "${rpc_port}" "${datadir}" "getblockcount" '[]' |
            jq -e '.result >= 0' >/dev/null 2>&1; then
            return 0
        fi
        sleep 1
    done
    return 1
}

wait_connection_count() {
    local ns="$1"
    local rpc_port="$2"
    local datadir="$3"
    local min_count="$4"
    local label="$5"
    local json count
    for _ in $(seq 1 90); do
        json="$(rpc_call "${ns}" "${rpc_port}" "${datadir}" "getconnectioncount" '[]' 2>/dev/null || true)"
        count="$(jq -r '.result // -1' <<<"${json}" 2>/dev/null || printf -- '-1')"
        if [[ "${count}" =~ ^-?[0-9]+$ && "${count}" -ge "${min_count}" ]]; then
            pass "${label}: ${count} connection(s)"
            return 0
        fi
        sleep 1
    done
    fail "${label}: expected at least ${min_count} connection(s)"
}

wait_relay_peerinfo() {
    local ns="$1"
    local rpc_port="$2"
    local datadir="$3"
    local jq_filter="$4"
    local label="$5"
    local json count
    for _ in $(seq 1 90); do
        json="$(rpc_call "${ns}" "${rpc_port}" "${datadir}" "getpeerinfo" '[]' 2>/dev/null || true)"
        count="$(jq -r "${jq_filter}" <<<"${json}" 2>/dev/null || printf -- '0')"
        if [[ "${count}" =~ ^[0-9]+$ && "${count}" -ge 1 ]]; then
            pass "${label}: ${count} matching peer(s)"
            return 0
        fi
        sleep 1
    done
    fail "${label}: expected at least one matching relay peer"
}

wait_networkinfo_jq() {
    local ns="$1"
    local rpc_port="$2"
    local datadir="$3"
    local jq_filter="$4"
    local label="$5"
    local json
    for _ in $(seq 1 90); do
        json="$(rpc_call "${ns}" "${rpc_port}" "${datadir}" "getnetworkinfo" '[]' 2>/dev/null || true)"
        if jq -e "${jq_filter}" <<<"${json}" >/dev/null 2>&1; then
            pass "${label}"
            return 0
        fi
        sleep 1
    done
    fail "${label}: getnetworkinfo did not satisfy '${jq_filter}'"
}

# Wait until no peer whose addr contains addr_substr remains — used to
# confirm a disconnect fully settled before reconnecting.
wait_peer_gone() {
    local ns="$1"
    local rpc_port="$2"
    local datadir="$3"
    local addr_substr="$4"
    local label="$5"
    local json count
    for _ in $(seq 1 90); do
        json="$(rpc_call "${ns}" "${rpc_port}" "${datadir}" "getpeerinfo" '[]' 2>/dev/null || true)"
        count="$(jq -r "[.result[]? | select((.addr // \"\") | contains(\"${addr_substr}\"))] | length" <<<"${json}" 2>/dev/null || printf -- '1')"
        if [[ "${count}" =~ ^[0-9]+$ && "${count}" -eq 0 ]]; then
            pass "${label}"
            return 0
        fi
        sleep 1
    done
    fail "${label}: peer '${addr_substr}' still present"
}

wait_log() {
    local logfile="$1"
    local pattern="$2"
    local label="$3"
    for _ in $(seq 1 90); do
        if [[ -f "${logfile}" ]] && grep -Fq "${pattern}" "${logfile}"; then
            pass "${label}"
            return 0
        fi
        sleep 1
    done
    fail "${label}: missing log pattern '${pattern}'"
}

tcp_probe() {
    local ns="$1"
    local host="$2"
    local port="$3"
    ip netns exec "${ns}" timeout 2 bash -c ":</dev/tcp/${host}/${port}" >/dev/null 2>&1
}

create_ns() {
    local ns="$1"
    ip netns add "${ns}"
    ip -n "${ns}" link set lo up
}

setup_wan_peer() {
    local ns="$1"
    local root_if="$2"
    local bridge_if="$3"
    local ip_cidr="$4"

    ip link add "${root_if}" type veth peer name "${bridge_if}"
    ip link set "${bridge_if}" master "${BRIDGE}"
    ip link set "${bridge_if}" up
    ip link set "${root_if}" netns "${ns}"
    ip -n "${ns}" link set "${root_if}" name wan0
    ip -n "${ns}" addr add "${ip_cidr}" dev wan0
    ip -n "${ns}" link set wan0 up
}

setup_lan_pair() {
    local node_ns="$1"
    local nat_ns="$2"
    local node_if="$3"
    local nat_if="$4"
    local node_ip_cidr="$5"
    local nat_ip_cidr="$6"
    local gateway="$7"

    ip link add "${node_if}" type veth peer name "${nat_if}"
    ip link set "${node_if}" netns "${node_ns}"
    ip link set "${nat_if}" netns "${nat_ns}"
    ip -n "${node_ns}" link set "${node_if}" name eth0
    ip -n "${nat_ns}" link set "${nat_if}" name lan0
    ip -n "${node_ns}" addr add "${node_ip_cidr}" dev eth0
    ip -n "${nat_ns}" addr add "${nat_ip_cidr}" dev lan0
    ip -n "${node_ns}" link set eth0 up
    ip -n "${nat_ns}" link set lan0 up
    ip -n "${node_ns}" route add default via "${gateway}"
}

configure_nat() {
    local ns="$1"
    ip netns exec "${ns}" sh -c 'echo 1 > /proc/sys/net/ipv4/ip_forward'
    ip netns exec "${ns}" iptables -P FORWARD DROP
    ip netns exec "${ns}" iptables -A FORWARD -i lan0 -o wan0 -j ACCEPT
    ip netns exec "${ns}" iptables -A FORWARD -i wan0 -o lan0 \
        -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
    ip netns exec "${ns}" iptables -t nat -A POSTROUTING -o wan0 -j MASQUERADE
}

setup_topology() {
    rm -rf "${RUN_DIR}"
    mkdir -p "${RUN_DIR}"

    for ns in "${NS_ORIGIN}" "${NS_RELAY}" "${NS_TARGET}" "${NS_NAT_ORIGIN}" "${NS_NAT_TARGET}"; do
        create_ns "${ns}"
    done

    ip link add "${BRIDGE}" type bridge
    ip link set "${BRIDGE}" up

    setup_wan_peer "${NS_RELAY}" "r${TAG}" "br_r${TAG}" "${RELAY_IP}/24"
    setup_wan_peer "${NS_NAT_ORIGIN}" "no${TAG}" "br_no${TAG}" "${ORIGIN_NAT_WAN_IP}/24"
    setup_wan_peer "${NS_NAT_TARGET}" "nt${TAG}" "br_nt${TAG}" "${TARGET_NAT_WAN_IP}/24"

    setup_lan_pair "${NS_ORIGIN}" "${NS_NAT_ORIGIN}" "o${TAG}" "nol${TAG}" \
        "${ORIGIN_IP}/24" "${ORIGIN_NAT_LAN_IP}/24" "${ORIGIN_NAT_LAN_IP}"
    setup_lan_pair "${NS_TARGET}" "${NS_NAT_TARGET}" "t${TAG}" "ntl${TAG}" \
        "${TARGET_IP}/24" "${TARGET_NAT_LAN_IP}/24" "${TARGET_NAT_LAN_IP}"

    configure_nat "${NS_NAT_ORIGIN}"
    configure_nat "${NS_NAT_TARGET}"
    info "Created WAN ${WAN_CIDR} with origin and target behind separate hostile NAT namespaces"
}

start_node() {
    local ns="$1"
    local datadir="$2"
    local rpc_port="$3"
    local p2p_port="$4"
    local logfile="$5"
    shift 5

    mkdir -p "${datadir}"
    ip netns exec "${ns}" "${DINEROD}" \
        --regtest \
        --datadir="${datadir}" \
        --rpcport="${rpc_port}" \
        --port="${p2p_port}" \
        --listen=1 \
        --p2p.stun.enabled=0 \
        --portmap=0 \
        "$@" \
        >"${logfile}" 2>&1 &
    printf '%s\n' "$!"
}

setup_topology

if tcp_probe "${NS_ORIGIN}" "${TARGET_NAT_WAN_IP}" "${TARGET_P2P}"; then
    fail "origin unexpectedly opened a direct TCP connection to target NAT public address"
fi
pass "Hostile NAT blocks unsolicited origin-to-target TCP"

PIDS+=("$(start_node "${NS_RELAY}" "${DATA_RELAY}" "${RELAY_RPC}" "${RELAY_P2P}" "${LOG_RELAY}" \
    "--externalip=${RELAY_IP}:${RELAY_P2P}")")
PIDS+=("$(start_node "${NS_ORIGIN}" "${DATA_ORIGIN}" "${ORIGIN_RPC}" "${ORIGIN_P2P}" "${LOG_ORIGIN}")")
PIDS+=("$(start_node "${NS_TARGET}" "${DATA_TARGET}" "${TARGET_RPC}" "${TARGET_P2P}" "${LOG_TARGET}" \
    "--relayregister=${RELAY_IP}:${RELAY_P2P}")")

wait_rpc "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" || fail "relay RPC did not come up"
wait_rpc "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" || fail "origin RPC did not come up"
wait_rpc "${NS_TARGET}" "${TARGET_RPC}" "${DATA_TARGET}" || fail "target RPC did not come up"
pass "All three regtest daemons are running inside isolated namespaces"

if tcp_probe "${NS_ORIGIN}" "${TARGET_NAT_WAN_IP}" "${TARGET_P2P}"; then
    fail "origin unexpectedly opened TCP to target through the target NAT public address"
fi
if tcp_probe "${NS_ORIGIN}" "${TARGET_IP}" "${TARGET_P2P}"; then
    fail "origin unexpectedly opened TCP to target private address across NAT boundaries"
fi
pass "Target P2P listener is not directly reachable from origin"

if ! tcp_probe "${NS_ORIGIN}" "${RELAY_IP}" "${RELAY_P2P}"; then
    fail "origin cannot reach relay public P2P endpoint through outbound NAT"
fi
if ! tcp_probe "${NS_TARGET}" "${RELAY_IP}" "${RELAY_P2P}"; then
    fail "target cannot reach relay public P2P endpoint through outbound NAT"
fi
pass "Both NATed nodes can open outbound TCP to the relay"

rpc_call "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" \
    "addnode" "[\"${RELAY_IP}:${RELAY_P2P}\",\"onetry\"]" >/dev/null
wait_connection_count "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" 1 \
    "origin connected outbound to relay"
wait_connection_count "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" 1 \
    "relay accepted origin before target registration"

rpc_call "${NS_TARGET}" "${TARGET_RPC}" "${DATA_TARGET}" \
    "addnode" "[\"${RELAY_IP}:${RELAY_P2P}\",\"onetry\"]" >/dev/null
wait_connection_count "${NS_TARGET}" "${TARGET_RPC}" "${DATA_TARGET}" 1 \
    "target connected outbound to relay"
wait_connection_count "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" 2 \
    "relay accepted both NATed peers"

wait_log "${LOG_TARGET}" "[P2P] relay-register: sent to ${RELAY_IP}:${RELAY_P2P}" \
    "target sent RELAY_REGISTER to configured relay"
wait_log "${LOG_RELAY}" "[P2P] relayreg: registered" \
    "relay accepted target registration"
wait_networkinfo_jq "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" \
    '(.result.relay.directory.entries // 0) >= 1' \
    "relay getnetworkinfo exposes registered relay directory entry"
wait_log "${LOG_RELAY}" "[P2P] relay-hints: advertised registered target" \
    "relay advertised target reachability hint to origin"
wait_log "${LOG_ORIGIN}" "[P2P] relay-hints: ingested" \
    "origin ingested relay hint for target"
wait_networkinfo_jq "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" \
    '((.result.relay.hints.received_self // 0) + (.result.relay.hints.received_relay // 0)) >= 1' \
    "origin getnetworkinfo exposes relay hint counters"
wait_log "${LOG_RELAY}" "[P2P] relaycon: opened circuit" \
    "relay opened origin-to-target circuit"
wait_log "${LOG_ORIGIN}" "[P2P] relay-orchestrator: opened circuit" \
    "origin installed outbound virtual relay peer"
wait_log "${LOG_TARGET}" "[P2P] relay-data: created inbound virtual peer" \
    "target observed inbound virtual relay peer"

wait_relay_peerinfo "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" \
    '[.result[]? | select((.inbound == false) and ((.addr // "") | startswith("relay:")))] | length' \
    "origin getpeerinfo reports outbound virtual relay peer"
wait_relay_peerinfo "${NS_TARGET}" "${TARGET_RPC}" "${DATA_TARGET}" \
    '[.result[]? | select((.inbound == true) and ((.addr // "") | startswith("relay:in:")))] | length' \
    "target getpeerinfo reports inbound virtual relay peer"

# ── on-connect registry catch-up ────────────────────────────────────
# A peer that connects to the relay AFTER a target registered must be
# caught up with the existing registry: per-registration advertisement
# is deduped and refreshes are an hour apart. Drop the origin's direct
# relay link and reconnect — the relay must replay its registry to the
# fresh connection.
rpc_call "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" \
    "disconnectnode" "[\"${RELAY_IP}:${RELAY_P2P}\"]" >/dev/null 2>&1 || true
wait_peer_gone "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" \
    "${RELAY_IP}:${RELAY_P2P}" "origin dropped its direct relay link"
rpc_call "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" \
    "addnode" "[\"${RELAY_IP}:${RELAY_P2P}\",\"onetry\"]" >/dev/null
wait_log "${LOG_RELAY}" "[P2P] relay-hints: sent registry catch-up" \
    "relay replayed its registry to the reconnecting origin"

# ── relay-directory disconnect grace ────────────────────────────────
# When a registered target disconnects from the fleet relay, the relay
# must hide the target from lookups/catch-up immediately but retain it
# behind a short grace timer so a brief reconnect can restore the entry.
rpc_call "${NS_TARGET}" "${TARGET_RPC}" "${DATA_TARGET}" \
    "disconnectnode" "[\"${RELAY_IP}:${RELAY_P2P}\"]" >/dev/null 2>&1 || true
wait_peer_gone "${NS_TARGET}" "${TARGET_RPC}" "${DATA_TARGET}" \
    "${RELAY_IP}:${RELAY_P2P}" "target dropped its direct relay link"
wait_networkinfo_jq "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" \
    '(.result.relay.directory.grace_pending // 0) >= 1' \
    "relay marks disconnected registration grace-pending"
rpc_call "${NS_TARGET}" "${TARGET_RPC}" "${DATA_TARGET}" \
    "addnode" "[\"${RELAY_IP}:${RELAY_P2P}\",\"onetry\"]" >/dev/null
wait_connection_count "${NS_TARGET}" "${TARGET_RPC}" "${DATA_TARGET}" 1 \
    "target reconnected outbound to relay"
wait_networkinfo_jq "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" \
    '(.result.relay.directory.entries // 0) >= 1 and (.result.relay.directory.grace_pending // 0) == 0' \
    "relay restores registration after target reconnect"

echo "RELAY_NAT_NETNS_DIAL_THROUGH=PASS"
