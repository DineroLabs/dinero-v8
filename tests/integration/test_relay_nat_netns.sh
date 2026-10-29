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
# DIALBACK=1 topology: three public nodes in distinct /16s, an
# endpoint-independent NAT and an endpoint-dependent (--random) NAT.
NS_P1="dn_p1_${TAG}"
NS_P2="dn_p2_${TAG}"
NS_P3="dn_p3_${TAG}"
NS_P4="dn_p4_${TAG}"  # COMMUNITY_RELAY: a second public node in P1's /16
NS_NAT_EIM="dn_ne_${TAG}"
NS_NAT_RND="dn_nr_${TAG}"
NS_EIM="dn_e_${TAG}"
NS_RND="dn_d_${TAG}"

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
        for ns in "${NS_ORIGIN}" "${NS_RELAY}" "${NS_TARGET}" "${NS_NAT_ORIGIN}" "${NS_NAT_TARGET}" \
                  "${NS_P1}" "${NS_P2}" "${NS_P3}" "${NS_P4}" "${NS_NAT_EIM}" "${NS_NAT_RND}" "${NS_EIM}" "${NS_RND}"; do
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

# Wait until no direct peer with addr exactly matching peer_addr remains. Virtual
# relay peers can contain the relay endpoint in their synthetic relay: address,
# so substring matching would mistake a live virtual circuit for a stale direct
# TCP peer.
wait_peer_gone() {
    local ns="$1"
    local rpc_port="$2"
    local datadir="$3"
    local peer_addr="$4"
    local label="$5"
    local json count
    for _ in $(seq 1 90); do
        json="$(rpc_call "${ns}" "${rpc_port}" "${datadir}" "getpeerinfo" '[]' 2>/dev/null || true)"
        count="$(jq -r "[.result[]? | select((.addr // \"\") == \"${peer_addr}\")] | length" <<<"${json}" 2>/dev/null || printf -- '1')"
        if [[ "${count}" =~ ^[0-9]+$ && "${count}" -eq 0 ]]; then
            pass "${label}"
            return 0
        fi
        sleep 1
    done
    fail "${label}: peer '${peer_addr}' still present"
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

# Like wait_log but matches an extended regex (for patterns that span tokens,
# e.g. asserting "headers" and "relay:" appear on the same line in any order).
wait_log_regex() {
    local logfile="$1"
    local pattern="$2"
    local label="$3"
    for _ in $(seq 1 90); do
        if [[ -f "${logfile}" ]] && grep -Eq "${pattern}" "${logfile}"; then
            pass "${label}"
            return 0
        fi
        sleep 1
    done
    fail "${label}: missing log pattern /${pattern}/"
}

# Negative assertion: the given pattern must NOT appear in the log. Used to
# guard the data plane against the ERR_IDLE_CLOSE class — a relay virtual peer
# can be created and then die when the QUIC handshake never completes, which
# the plumbing-only assertions above would miss.
assert_log_absent() {
    local logfile="$1"
    local pattern="$2"
    local label="$3"
    if [[ -f "${logfile}" ]] && grep -Fq "${pattern}" "${logfile}"; then
        fail "${label}: forbidden log pattern present '${pattern}'"
    fi
    pass "${label}"
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
    shift
    # Extra args go to MASQUERADE: "--random" gives every flow its own
    # source port, i.e. endpoint-dependent (symmetric) mapping.
    ip netns exec "${ns}" sh -c 'echo 1 > /proc/sys/net/ipv4/ip_forward'
    ip netns exec "${ns}" iptables -P FORWARD DROP
    ip netns exec "${ns}" iptables -A FORWARD -i lan0 -o wan0 -j ACCEPT
    ip netns exec "${ns}" iptables -A FORWARD -i wan0 -o lan0 \
        -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
    ip netns exec "${ns}" iptables -t nat -A POSTROUTING -o wan0 -j MASQUERADE "$@"
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
        --p2p.stun.enabled="${STUN_ENABLED:-0}" \
        --portmap=0 \
        "$@" \
        >"${logfile}" 2>&1 &
    printf '%s\n' "$!"
}

# ── DIALBACK=1: dial-back verification and NAT classification ─────────────
# Public nodes P1..P3 sit in distinct /16s so min_groups=2 is met only by
# genuinely different networks. E sits behind an endpoint-independent NAT, D
# behind an endpoint-dependent one (MASQUERADE --random); inbound is blocked
# for both. Expected: the public nodes prove each other reachable; E and D get
# peer votes for their gateway address but it is never verified, listed or
# gossiped; STUN on each P2P port classifies the two NATs correctly.
if [[ "${DIALBACK:-0}" == "1" ]]; then
    require_cmd tcpdump
    require_cmd python3
    # Routable-looking WAN addresses (DoD blocks, unused on the internet; the
    # bridge has no uplink): the node refuses to advertise private ones at all,
    # which would make the gossip check vacuous. A real router's address is
    # routable, and that is the address that must stay unadvertised.
    P1_IP="11.11.0.10"; P2_IP="22.22.0.10"; P3_IP="33.33.0.10"
    EIM_WAN="55.44.0.2"; RND_WAN="55.55.0.2"
    P1_RPC=$((BASE_PORT + 11)); P2_RPC=$((BASE_PORT + 12)); P3_RPC=$((BASE_PORT + 13))
    E_RPC=$((BASE_PORT + 14)); D_RPC=$((BASE_PORT + 15))
    P2P_PORT=$((BASE_PORT + 111))  # one port for all: each node has its own IP
    rm -rf "${RUN_DIR}"; mkdir -p "${RUN_DIR}"
    for ns in "${NS_P1}" "${NS_P2}" "${NS_P3}" "${NS_NAT_EIM}" "${NS_NAT_RND}" "${NS_EIM}" "${NS_RND}"; do
        create_ns "${ns}"
    done
    ip link add "${BRIDGE}" type bridge
    ip link set "${BRIDGE}" up
    # Distinct /16s on one L2 segment: everything off-LAN is on-link via wan0.
    wan_16() {
        setup_wan_peer "$1" "$2" "br$2" "$3/16"
        ip -n "$1" route add default dev wan0
    }
    wan_16 "${NS_P1}" "p1${TAG}" "${P1_IP}"
    wan_16 "${NS_P2}" "p2${TAG}" "${P2_IP}"
    wan_16 "${NS_P3}" "p3${TAG}" "${P3_IP}"
    wan_16 "${NS_NAT_EIM}" "ne${TAG}" "${EIM_WAN}"
    wan_16 "${NS_NAT_RND}" "nr${TAG}" "${RND_WAN}"
    setup_lan_pair "${NS_EIM}" "${NS_NAT_EIM}" "e${TAG}" "nel${TAG}" \
        "10.144.1.2/24" "10.144.1.1/24" "10.144.1.1"
    setup_lan_pair "${NS_RND}" "${NS_NAT_RND}" "d${TAG}" "nrl${TAG}" \
        "10.155.1.2/24" "10.155.1.1/24" "10.155.1.1"
    configure_nat "${NS_NAT_EIM}"
    configure_nat "${NS_NAT_RND}" --random
    info "DIALBACK topology: P1 ${P1_IP}, P2 ${P2_IP}, P3 ${P3_IP}; E behind EIM NAT ${EIM_WAN}; D behind --random NAT ${RND_WAN}"

    # Two STUN servers on different IPs (P2, P3): answer every Binding request
    # with the source address in XOR-MAPPED-ADDRESS.
    cat >"${RUN_DIR}/stun.py" <<'STUN'
import socket, struct, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind((sys.argv[1], 3478))
while True:
    d, (ip, port) = s.recvfrom(2048)
    if len(d) < 20 or d[0:2] != b"\x00\x01" or d[4:8] != b"\x21\x12\xa4\x42":
        continue
    xip = bytes(a ^ b for a, b in zip(socket.inet_aton(ip), d[4:8]))
    attr = struct.pack("!HHBBH", 0x0020, 8, 0, 1, port ^ 0x2112) + xip
    s.sendto(struct.pack("!HH", 0x0101, len(attr)) + d[4:20] + attr, (ip, port))
STUN
    for pair in "${NS_P2}:${P2_IP}" "${NS_P3}:${P3_IP}"; do
        ip netns exec "${pair%%:*}" python3 "${RUN_DIR}/stun.py" "${pair##*:}" &
        PIDS+=("$!")
    done
    STUN_SERVERS="${P2_IP}:3478,${P3_IP}:3478"

    # Everything E sends (only what it sends: verifiers' reachres replies
    # carry the dialed gateway endpoint inbound): its gateway address may only
    # leave E inside addr gossip, so "gateway IP + listen port" must never appear.
    ip netns exec "${NS_EIM}" tcpdump -i eth0 -s 0 -U -w "${RUN_DIR}/e.pcap" "tcp and src host 10.144.1.2" 2>/dev/null &
    TCPDUMP_PID="$!"
    PIDS+=("${TCPDUMP_PID}")

    export STUN_ENABLED=1
    LOG_ORIGIN="${RUN_DIR}/e.log"; LOG_RELAY="${RUN_DIR}/p1.log"; LOG_TARGET="${RUN_DIR}/d.log"
    declare -A NS_OF=([p1]="${NS_P1}" [p2]="${NS_P2}" [p3]="${NS_P3}" [e]="${NS_EIM}" [d]="${NS_RND}")
    declare -A RPC_OF=([p1]="${P1_RPC}" [p2]="${P2_RPC}" [p3]="${P3_RPC}" [e]="${E_RPC}" [d]="${D_RPC}")
    for n in p1 p2 p3 e d; do
        pid="$(start_node "${NS_OF[$n]}" "${RUN_DIR}/${n}" "${RPC_OF[$n]}" "${P2P_PORT}" "${RUN_DIR}/${n}.log" \
            --quic=1 --p2p.stun.servers="${STUN_SERVERS}")"
        PIDS+=("${pid}")
    done
    for n in p1 p2 p3 e d; do
        wait_rpc "${NS_OF[$n]}" "${RPC_OF[$n]}" "${RUN_DIR}/${n}" || fail "${n} RPC did not come up"
    done
    nrpc() { rpc_call "${NS_OF[$1]}" "${RPC_OF[$1]}" "${RUN_DIR}/$1" "$2" "$3"; }
    addnode() { nrpc "$1" addnode "[\"$2:${P2P_PORT}\",\"onetry\"]" >/dev/null || fail "$1 addnode $2"; }
    addnode p1 "${P2_IP}"; addnode p1 "${P3_IP}"; addnode p2 "${P3_IP}"
    for n in e d; do
        addnode "${n}" "${P1_IP}"; addnode "${n}" "${P2_IP}"; addnode "${n}" "${P3_IP}"
    done

    # NAT classification, measured through each node's P2P UDP socket.
    nat_of() {
        local n="$1" want="$2" got=""
        for _ in $(seq 1 60); do
            got="$(nrpc "${n}" getnetworkinfo '[]' | jq -r 'select(.result.nat.servers >= 2) | .result.nat.mapping' 2>/dev/null || true)"
            [[ -n "${got}" ]] && break
            sleep 1
        done
        [[ "${got}" == "${want}" ]] || fail "${n}: nat.mapping='${got}', expected ${want}"
        pass "${n}: nat.mapping=${want} ($(nrpc "${n}" getnetworkinfo '[]' | jq -c '.result.nat'))"
    }
    nat_of e endpoint_independent
    nat_of d endpoint_dependent

    # Public nodes prove each other reachable (2 distinct /16s) within 5 min.
    verified() {  # verified <node> <ip>: our own TCP address at P2P_PORT is verified
        nrpc "$1" getnetworkinfo '[]' | jq -e --arg a "$2:${P2P_PORT}" \
            '.result.self_reachability[] | select(.address==$a and .transport=="tcp" and .state=="verified")' >/dev/null 2>&1
    }
    for pair in "p1:${P1_IP}" "p2:${P2_IP}" "p3:${P3_IP}"; do
        n="${pair%%:*}"; ip="${pair##*:}"; ok=0
        for _ in $(seq 1 300); do
            if verified "${n}" "${ip}"; then ok=1; break; fi
            sleep 1
        done
        [[ "${ok}" == 1 ]] || fail "${n}: ${ip}:${P2P_PORT} not verified within 5 min: $(nrpc "${n}" getnetworkinfo '[]' | jq -c '.result.self_reachability')"
        pass "${n}: ${ip}:${P2P_PORT} verified by dial-back"
    done

    # The NAT'd nodes: peers voted for the gateway address (it is a candidate),
    # dial-back could not reach it, so it is never verified, listed or gossiped.
    for pair in "e:${EIM_WAN}" "d:${RND_WAN}"; do
        n="${pair%%:*}"; gw="${pair##*:}"; seen=0
        for _ in $(seq 1 120); do
            if nrpc "${n}" getnetworkinfo '[]' | jq -e --arg a "${gw}:${P2P_PORT}" \
                '.result.self_reachability[] | select(.address==$a and .transport=="tcp")' >/dev/null 2>&1; then
                seen=1; break
            fi
            sleep 1
        done
        [[ "${seen}" == 1 ]] || fail "${n}: peers never voted for gateway ${gw} (scenario did not exercise the vote path)"
    done
    sleep 60  # several reachability ticks with the gateway candidate pending
    kill "${TCPDUMP_PID}" 2>/dev/null || true
    sleep 1
    # Positive control: E's version to P1 carries P1's address and port in
    # addrRecv, so a working capture must contain that pattern.
    python3 - "${RUN_DIR}/e.pcap" "${P1_IP}" "${P2P_PORT}" <<'SCAN' ||
import socket, struct, sys
data = open(sys.argv[1], "rb").read()
sys.exit(0 if socket.inet_aton(sys.argv[2]) + struct.pack("!H", int(sys.argv[3])) in data else 1)
SCAN
        fail "packet capture on E saw no P2P traffic (capture broken, gossip check would be vacuous)"
    python3 - "${RUN_DIR}/e.pcap" "${EIM_WAN}" "${P2P_PORT}" <<'SCAN' ||
import socket, struct, sys
data = open(sys.argv[1], "rb").read()
needle = socket.inet_aton(sys.argv[2]) + struct.pack("!H", int(sys.argv[3]))
sys.exit(1 if needle in data else 0)
SCAN
        fail "NAT'd node gossips gateway address ${EIM_WAN}:${P2P_PORT}"
    pass "E never put its gateway address ${EIM_WAN}:${P2P_PORT} on the wire"
    for pair in "e:${EIM_WAN}" "d:${RND_WAN}"; do
        n="${pair%%:*}"; gw="${pair##*:}"
        info_json="$(nrpc "${n}" getnetworkinfo '[]')"
        jq -e --arg gw "${gw}" '[.result.localaddresses[] | select(.address==$gw)] | length == 0' \
            <<<"${info_json}" >/dev/null || fail "NAT'd node ${n} lists gateway ${gw} in localaddresses"
        jq -e --arg a "${gw}:${P2P_PORT}" \
            '[.result.self_reachability[] | select(.address==$a and .state=="verified")] | length == 0' \
            <<<"${info_json}" >/dev/null || fail "NAT'd node ${n}: gateway ${gw} verified despite blocked inbound"
        pass "${n}: gateway ${gw} voted for, unverified, not in localaddresses"
    done
    exit 0
fi

# ── COMMUNITY_RELAY=1 / TWO_RELAYS=1: verified home-style nodes relay ────
# P1..P4 are ordinary public nodes: no p2p.relay, no relayregister, no
# mining. Dial-back verification alone must turn their relay role on. P4
# shares P1's /16, so a NAT'd node that ignored netgroups could pick both.
# E (endpoint-independent NAT) and D (--random NAT) cannot reach each other
# directly; D must find E through a signed hint and reach it via a relay.
# TWO_RELAYS=1 then kills one of E's relays: E must re-register and D must
# still reach E within 90 s.
if [[ "${COMMUNITY_RELAY:-0}" == "1" || "${TWO_RELAYS:-0}" == "1" ]]; then
    P1_IP="11.11.0.10"; P4_IP="11.11.0.20"; P2_IP="22.22.0.10"; P3_IP="33.33.0.10"
    EIM_WAN="55.44.0.2"; RND_WAN="55.55.0.2"
    P2P_PORT=$((BASE_PORT + 111))
    rm -rf "${RUN_DIR}"; mkdir -p "${RUN_DIR}"
    for ns in "${NS_P1}" "${NS_P2}" "${NS_P3}" "${NS_P4}" "${NS_NAT_EIM}" "${NS_NAT_RND}" "${NS_EIM}" "${NS_RND}"; do
        create_ns "${ns}"
    done
    ip link add "${BRIDGE}" type bridge
    ip link set "${BRIDGE}" up
    wan_16() {
        setup_wan_peer "$1" "$2" "br$2" "$3/16"
        ip -n "$1" route add default dev wan0
    }
    wan_16 "${NS_P1}" "p1${TAG}" "${P1_IP}"
    wan_16 "${NS_P2}" "p2${TAG}" "${P2_IP}"
    wan_16 "${NS_P3}" "p3${TAG}" "${P3_IP}"
    wan_16 "${NS_P4}" "p4${TAG}" "${P4_IP}"
    wan_16 "${NS_NAT_EIM}" "ne${TAG}" "${EIM_WAN}"
    wan_16 "${NS_NAT_RND}" "nr${TAG}" "${RND_WAN}"
    setup_lan_pair "${NS_EIM}" "${NS_NAT_EIM}" "e${TAG}" "nel${TAG}" \
        "10.144.1.2/24" "10.144.1.1/24" "10.144.1.1"
    setup_lan_pair "${NS_RND}" "${NS_NAT_RND}" "d${TAG}" "nrl${TAG}" \
        "10.155.1.2/24" "10.155.1.1/24" "10.155.1.1"
    configure_nat "${NS_NAT_EIM}"
    configure_nat "${NS_NAT_RND}" --random
    info "COMMUNITY_RELAY topology: P1 ${P1_IP}, P4 ${P4_IP} (same /16), P2 ${P2_IP}, P3 ${P3_IP}; E behind ${EIM_WAN}, D behind ${RND_WAN}"

    LOG_ORIGIN="${RUN_DIR}/d.log"; LOG_RELAY="${RUN_DIR}/p1.log"; LOG_TARGET="${RUN_DIR}/e.log"
    declare -A NS_OF=([p1]="${NS_P1}" [p2]="${NS_P2}" [p3]="${NS_P3}" [p4]="${NS_P4}" [e]="${NS_EIM}" [d]="${NS_RND}")
    declare -A RPC_OF=([p1]=$((BASE_PORT + 11)) [p2]=$((BASE_PORT + 12)) [p3]=$((BASE_PORT + 13))
                       [p4]=$((BASE_PORT + 16)) [e]=$((BASE_PORT + 14)) [d]=$((BASE_PORT + 15)))
    declare -A IP_OF=([p1]="${P1_IP}" [p2]="${P2_IP}" [p3]="${P3_IP}" [p4]="${P4_IP}")
    declare -A PID_OF=()
    for n in p1 p2 p3 p4 e d; do
        pid="$(start_node "${NS_OF[$n]}" "${RUN_DIR}/${n}" "${RPC_OF[$n]}" "${P2P_PORT}" "${RUN_DIR}/${n}.log" --quic=1)"
        PIDS+=("${pid}"); PID_OF[$n]="${pid}"
    done
    for n in p1 p2 p3 p4 e d; do
        wait_rpc "${NS_OF[$n]}" "${RPC_OF[$n]}" "${RUN_DIR}/${n}" || fail "${n} RPC did not come up"
    done
    nrpc() { rpc_call "${NS_OF[$1]}" "${RPC_OF[$1]}" "${RUN_DIR}/$1" "$2" "$3"; }
    addnode() { nrpc "$1" addnode "[\"$2:${P2P_PORT}\",\"onetry\"]" >/dev/null || fail "$1 addnode $2"; }
    addnode p1 "${P2_IP}"; addnode p1 "${P3_IP}"; addnode p1 "${P4_IP}"
    addnode p2 "${P3_IP}"; addnode p2 "${P4_IP}"; addnode p3 "${P4_IP}"
    for n in e d; do
        for p in p1 p2 p3 p4; do addnode "${n}" "${IP_OF[$p]}"; done
    done
    for p in p1 p2 p3 p4; do
        nrpc "${p}" getnetworkinfo '[]' | jq -e '.result.relay.local == false' >/dev/null ||
            fail "${p} relays before anyone verified it (scenario would prove nothing)"
    done
    # A node at genesis is in initial block download, and a syncing relay
    # refuses new circuits: give everyone a recent tip first.
    p1_miner="$(nrpc p1 wallet.createhd '["miner"]' | jq -r '.result.first_address')"
    nrpc p1 generatetoaddress "[3, \"${p1_miner}\"]" >/dev/null || fail "P1 could not mine"
    for n in p1 p2 p3 p4 e d; do
        ok=0
        for _ in $(seq 1 120); do
            [[ "$(nrpc "${n}" getblockcount '[]' | jq -r '.result')" -ge 3 ]] && { ok=1; break; }
            sleep 1
        done
        [[ "${ok}" == 1 ]] || fail "${n} did not sync P1's first blocks"
    done

    # 1. Dial-back verification alone turns the relay role on.
    for p in p1 p2 p3 p4; do
        ok=0
        for _ in $(seq 1 300); do
            if nrpc "${p}" getnetworkinfo '[]' | jq -e --arg a "${IP_OF[$p]}:${P2P_PORT}" \
                '.result.relay.local == true and
                 any(.result.self_reachability[]; .address==$a and .state=="verified")' >/dev/null 2>&1; then
                ok=1; break
            fi
            sleep 1
        done
        [[ "${ok}" == 1 ]] || fail "${p} never became a verified relay: $(nrpc "${p}" getnetworkinfo '[]' | jq -c '{relay: .result.relay.local, sr: .result.self_reachability}')"
        pass "${p}: ${IP_OF[$p]} verified by dial-back, relay role on (no relay setting)"
    done

    # 2. E keeps two confirmed community relays in distinct /16s.
    e_relays() { nrpc e getnetworkinfo '[]' | jq -r '.result.relay.registered[]' 2>/dev/null; }
    two_distinct() {  # stdin: relay endpoints; true when 2 in distinct /16s
        awk -F'[.:]' 'NF {g[$1"."$2]++; n++} END {exit !(n == 2 && length(g) == 2)}'
    }
    ok=0
    for _ in $(seq 1 240); do
        if e_relays | two_distinct; then ok=1; break; fi
        sleep 1
    done
    [[ "${ok}" == 1 ]] || fail "E is not registered with 2 relays in distinct /16s: $(e_relays | tr '\n' ' ')"
    pass "E registered with community relays $(e_relays | tr '\n' ' ')(distinct /16s, all confirmed)"

    # 3. D reaches E through a relay: a relayed, identity-proven peer that is E.
    e_id="$(nrpc e getnetworkinfo '[]' | jq -r '.result.node_id_hex')"
    [[ "${#e_id}" == 40 ]] || fail "E has no node id"
    d_reaches_e() {
        nrpc d getpeerinfo '[]' | jq -e --arg id "${e_id}" \
            'any(.result[]; .transport=="relay" and .identity_proven and .node_id==$id)' >/dev/null 2>&1
    }
    ok=0
    for _ in $(seq 1 240); do
        if d_reaches_e; then ok=1; break; fi
        sleep 1
    done
    [[ "${ok}" == 1 ]] || fail "D never reached E through a relay: $(nrpc d getpeerinfo '[]' | jq -c '[.result[] | {addr, transport, node_id}]')"
    pass "D reached E through a community relay (relayed peer, identity proven over dineroid v2)"

    # 4. Blocks travel over the relayed connection: E mines, and D's relayed
    #    peer (E) announces the new height.
    miner="$(nrpc e wallet.createhd '["miner"]' | jq -r '.result.first_address')"
    nrpc e generatetoaddress "[3, \"${miner}\"]" >/dev/null || fail "E could not mine"  # heights 4-6
    e_height="$(nrpc e getblockcount '[]' | jq -r '.result')"
    ok=0
    for _ in $(seq 1 90); do
        if nrpc d getpeerinfo '[]' | jq -e --arg id "${e_id}" --argjson h "${e_height}" \
            'any(.result[]; .transport=="relay" and .node_id==$id and .bestknownheight >= $h)' >/dev/null 2>&1 &&
           [[ "$(nrpc d getblockcount '[]' | jq -r '.result')" -ge "${e_height}" ]]; then
            ok=1; break
        fi
        sleep 1
    done
    [[ "${ok}" == 1 ]] || fail "D did not get E's blocks (height ${e_height}) with E's relayed peer at that height"
    pass "E mined ${e_height} blocks; D synced them and E's relayed peer reports height ${e_height}"

    if [[ "${TWO_RELAYS:-0}" == "1" ]]; then
        victim_ep="$(e_relays | head -1)"
        victim=""
        for p in p1 p2 p3 p4; do [[ "${IP_OF[$p]}:${P2P_PORT}" == "${victim_ep}" ]] && victim="${p}"; done
        [[ -n "${victim}" ]] || fail "E's relay ${victim_ep} is not one of the public nodes"
        kill "${PID_OF[$victim]}"
        info "killed ${victim} (${victim_ep}), one of E's two relays"
        t0="$(date +%s)"; ok=0
        for _ in $(seq 1 90); do
            if ! e_relays | grep -qx "${victim_ep}" && e_relays | two_distinct && d_reaches_e; then
                ok=1; break
            fi
            sleep 1
        done
        [[ "${ok}" == 1 ]] || fail "after losing ${victim_ep}: E relays '$(e_relays | tr '\n' ' ')', D reaches E: $(d_reaches_e && echo yes || echo no)"
        pass "within $(( $(date +%s) - t0 )) s E re-registered ($(e_relays | tr '\n' ' ')) and D still reaches E"
    fi
    exit 0
fi

setup_topology

# ── optional WAN impairment (tc netem) to reproduce real-internet conditions ──
# Clean netns is sub-millisecond + lossless, so the relay-tunneled QUIC
# handshake always completes there. Real fleet links have latency/jitter/loss.
# Gated by DINERO_NETNS_NETEM_DELAY / _JITTER / _LOSS / _RATE — applied to the
# relay's WAN egress and both NATs' WAN egress, so the relay tunnel sees the
# impairment in BOTH directions. Use this to probe whether the live
# ERR_IDLE_CLOSE data-plane failure is latency/loss-triggered.
apply_netem() {
    local ns="$1" dev="$2"
    local spec=""
    if [[ -n "${DINERO_NETNS_NETEM_DELAY:-}" ]]; then
        spec="delay ${DINERO_NETNS_NETEM_DELAY} ${DINERO_NETNS_NETEM_JITTER:-}"
    fi
    if [[ -n "${DINERO_NETNS_NETEM_LOSS:-}" ]]; then
        spec="${spec} loss ${DINERO_NETNS_NETEM_LOSS}"
    fi
    if [[ -n "${DINERO_NETNS_NETEM_RATE:-}" ]]; then
        spec="${spec} rate ${DINERO_NETNS_NETEM_RATE}"
    fi
    if [[ -z "${spec// /}" ]]; then
        return 0
    fi
    if ip netns exec "${ns}" tc qdisc add dev "${dev}" root netem ${spec} 2>/dev/null; then
        info "netem on ${ns}/${dev}: ${spec}"
    else
        info "netem on ${ns}/${dev} FAILED to apply (continuing without it)"
    fi
}
if [[ -n "${DINERO_NETNS_NETEM_DELAY:-}${DINERO_NETNS_NETEM_LOSS:-}${DINERO_NETNS_NETEM_RATE:-}" ]]; then
    info "Applying WAN impairment via tc netem (delay='${DINERO_NETNS_NETEM_DELAY:-}' jitter='${DINERO_NETNS_NETEM_JITTER:-}' loss='${DINERO_NETNS_NETEM_LOSS:-}' rate='${DINERO_NETNS_NETEM_RATE:-}')"
    # setup_wan_peer renames each node's WAN veth to "wan0" inside its netns.
    apply_netem "${NS_RELAY}"      "wan0"
    apply_netem "${NS_NAT_ORIGIN}" "wan0"
    apply_netem "${NS_NAT_TARGET}" "wan0"
fi

if tcp_probe "${NS_ORIGIN}" "${TARGET_NAT_WAN_IP}" "${TARGET_P2P}"; then
    fail "origin unexpectedly opened a direct TCP connection to target NAT public address"
fi
pass "Hostile NAT blocks unsolicited origin-to-target TCP"

# ── DIRECT_QUIC=1: a NAT'd node syncs over direct QUIC (UDP) ─────────────
# The origin sits behind MASQUERADE with inbound blocked; it dials the public
# node over QUIC on UDP and conntrack carries the replies back -- the same
# path a home node behind a router uses. No relay is involved.
if [[ "${DIRECT_QUIC:-0}" == "1" ]]; then
    RELAY_PID="$(start_node "${NS_RELAY}" "${DATA_RELAY}" "${RELAY_RPC}" "${RELAY_P2P}" "${LOG_RELAY}" \
        "--externalip=${RELAY_IP}:${RELAY_P2P}")"
    ORIGIN_PID="$(start_node "${NS_ORIGIN}" "${DATA_ORIGIN}" "${ORIGIN_RPC}" "${ORIGIN_P2P}" "${LOG_ORIGIN}" \
        "--quicconnect=${RELAY_IP}:${RELAY_P2P}")"
    PIDS+=("${RELAY_PID}" "${ORIGIN_PID}")
    wait_rpc "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" || fail "public node RPC did not come up"
    wait_rpc "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" || fail "origin RPC did not come up"

    miner="$(rpc_call "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" "wallet.createhd" '["miner"]' |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["result"]["first_address"])')"
    rpc_call "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" "generatetoaddress" "[10, \"${miner}\"]" \
        >/dev/null || fail "public node could not mine"
    synced=0
    for _ in $(seq 1 90); do
        h="$(rpc_call "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" "getblockcount" '[]' |
            python3 -c 'import json,sys; print(json.load(sys.stdin)["result"])')"
        if [[ "${h}" -ge 10 ]]; then synced=1; break; fi
        sleep 1
    done
    [[ "${synced}" == 1 ]] || fail "NAT'd origin did not sync over direct QUIC (height ${h})"
    pass "NAT'd origin synced 10 blocks over direct QUIC through MASQUERADE"

    rpc_call "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" "getpeerinfo" '[]' | python3 -c '
import json,sys
peers=json.load(sys.stdin)["result"]
assert any(p.get("transport")=="quic" and not p.get("inbound") for p in peers), peers' ||
        fail "origin has no outbound transport=quic peer"
    rpc_call "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" "getpeerinfo" '[]' | python3 -c '
import json,sys
peers=json.load(sys.stdin)["result"]
nat_wan=sys.argv[1]
assert any(p.get("transport")=="quic" and p.get("inbound") and p["addr"].startswith(nat_wan + ":")
           for p in peers), peers' "${ORIGIN_NAT_WAN_IP}" ||
        fail "public node does not see the origin (as its NAT address) over QUIC"
    pass "Both sides report the direct QUIC peer; the public node sees the NAT's WAN address"
    exit 0
fi

# The relay is operator-pinned (p2p.relay=1): its 10.88/16 address is
# private, so dial-back can never verify it and auto mode would not relay.
RELAY_PID="$(start_node "${NS_RELAY}" "${DATA_RELAY}" "${RELAY_RPC}" "${RELAY_P2P}" "${LOG_RELAY}" \
    "--externalip=${RELAY_IP}:${RELAY_P2P}" --p2p.relay=1)"
ORIGIN_PID="$(start_node "${NS_ORIGIN}" "${DATA_ORIGIN}" "${ORIGIN_RPC}" "${ORIGIN_P2P}" "${LOG_ORIGIN}")"
TARGET_PID="$(start_node "${NS_TARGET}" "${DATA_TARGET}" "${TARGET_RPC}" "${TARGET_P2P}" "${LOG_TARGET}" \
    "--relayregister=${RELAY_IP}:${RELAY_P2P}")"
PIDS+=("${RELAY_PID}" "${ORIGIN_PID}" "${TARGET_PID}")

wait_rpc "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" || fail "relay RPC did not come up"
wait_rpc "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" || fail "origin RPC did not come up"
wait_rpc "${NS_TARGET}" "${TARGET_RPC}" "${DATA_TARGET}" || fail "target RPC did not come up"
pass "All three regtest daemons are running inside isolated namespaces"
# A relay at genesis is in initial block download and refuses new circuits;
# give the chain a recent tip first.
relay_miner="$(rpc_call "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" "wallet.createhd" '["miner"]' |
    jq -r '.result.first_address')"
rpc_call "${NS_RELAY}" "${RELAY_RPC}" "${DATA_RELAY}" "generatetoaddress" "[3, \"${relay_miner}\"]" \
    >/dev/null || fail "relay could not mine"

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

# ── DATA PLANE: the circuit must CARRY data, not merely exist ────────
# A relay circuit forming (asserted above) is NOT proof that data flows.
# The ERR_IDLE_CLOSE failure class leaves a created-but-dead virtual peer
# whose QUIC handshake never completes. Require the encrypted relay stream
# to actually carry, end-to-end over the virtual peer: (1) QUIC handshake
# completion, (2) the dineroid identity exchange (multi-message encrypted
# app traffic), and (3) headers sync — and require that NO node logs an
# ERR_IDLE_CLOSE idle teardown of the circuit.
wait_log "${LOG_ORIGIN}" "relay-handshake: QUIC handshake ready for relay:" \
    "origin: QUIC handshake completed over relay virtual peer"
wait_log "${LOG_TARGET}" "relay-handshake: QUIC handshake ready for relay:in:" \
    "target: QUIC handshake completed over relay virtual peer"
wait_log "${LOG_ORIGIN}" "dineroid: identity proven for relay:" \
    "origin: dineroid identity proven OVER relay virtual peer (encrypted stream carries app data)"
wait_log "${LOG_TARGET}" "dineroid: identity proven for relay:in:" \
    "target: dineroid identity proven OVER relay virtual peer (encrypted stream carries app data)"
wait_log_regex "${LOG_ORIGIN}" "headers[^\"]*relay:|relay:[^\"]*headers" \
    "origin: headers sync ran over relay virtual peer"
wait_log_regex "${LOG_TARGET}" "headers[^\"]*relay:in:|relay:in:[^\"]*headers" \
    "target: headers sync ran over relay virtual peer"
# Negative guard: the QUIC relay session must not have idle-closed before/while
# carrying data. Checked here (before the disconnect sub-tests below, which
# intentionally tear circuits down).
assert_log_absent "${LOG_ORIGIN}" "ERR_IDLE_CLOSE" \
    "origin: relay QUIC session did NOT hit ERR_IDLE_CLOSE"
assert_log_absent "${LOG_TARGET}" "ERR_IDLE_CLOSE" \
    "target: relay QUIC session did NOT hit ERR_IDLE_CLOSE"
assert_log_absent "${LOG_RELAY}" "ERR_IDLE_CLOSE" \
    "relay: no ERR_IDLE_CLOSE on the relay node"

# ── durable relay discovery restart ────────────────────────────────
# The origin learned the target exclusively through RELAY_HINTS. After a
# graceful restart it must restore that still-fresh hint, reconnect to the
# public relay from peers.dat, and reopen the encrypted virtual circuit without
# requiring the relay to gossip the target again first.
kill -TERM "${ORIGIN_PID}"
for _ in $(seq 1 30); do
    if ! kill -0 "${ORIGIN_PID}" 2>/dev/null; then break; fi
    sleep 1
done
if kill -0 "${ORIGIN_PID}" 2>/dev/null; then
    fail "origin did not stop cleanly for relay-hint restart test"
fi
# Do not retain the exited PID in cleanup: the kernel may reuse it before the
# test ends, and killing a recycled PID would target an unrelated process.
PIDS=("${RELAY_PID}" "${TARGET_PID}")
RELAY_HINTS_FILE="$(find "${DATA_ORIGIN}" -name relay_hints.dat -type f -print -quit)"
[[ -n "${RELAY_HINTS_FILE}" ]] || fail "origin did not persist relay_hints.dat"
grep -q '^# DINERO_RELAY_HINTS_V1' "${RELAY_HINTS_FILE}" || \
    fail "origin relay_hints.dat has no recognized version header"
mv "${LOG_ORIGIN}" "${LOG_ORIGIN}.before-restart"
ORIGIN_PID="$(start_node "${NS_ORIGIN}" "${DATA_ORIGIN}" "${ORIGIN_RPC}" "${ORIGIN_P2P}" "${LOG_ORIGIN}")"
PIDS+=("${ORIGIN_PID}")
wait_rpc "${NS_ORIGIN}" "${ORIGIN_RPC}" "${DATA_ORIGIN}" || \
    fail "origin RPC did not return after restart"
wait_log "${LOG_ORIGIN}" "Loaded 1 durable relay hint(s)" \
    "origin restored fresh relay reachability metadata after restart"
wait_log "${LOG_ORIGIN}" "[P2P] relay-orchestrator: opened circuit" \
    "origin reopened target circuit from durable relay metadata"
wait_log "${LOG_ORIGIN}" "relay-handshake: QUIC handshake ready for relay:" \
    "restarted origin restored encrypted relay data plane"

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
