#!/usr/bin/env bash
#
# Integration: GETPROOF abuse hardening
#
# Validates gossip proof-serving rate limits:
#  1) Complete a real P2P handshake against a bridge-capable node
#  2) Flood GETPROOF requests past per-window budget
#  3) Assert attacker peer is disconnected
#  4) Assert node remains live (RPC + mining still works)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

FLOOD_COUNT=${FLOOD_COUNT:-48}
DISCONNECT_TIMEOUT=${DISCONNECT_TIMEOUT:-12}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}

# Honour $DINEROD first (and require it to be executable); the chain
# below never consulted it, so it CLOBBERED the caller's choice and an
# arbitrary build directory could not be used.
if [[ -n "${DINEROD:-}" ]]; then
    [[ -x "${DINEROD}" ]] || { echo "dinerod not executable at ${DINEROD}"; exit 1; }
elif [[ -x "${PROJECT_ROOT}/build/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/build/dinerod"
elif [[ -x "${PROJECT_ROOT}/dinerod" ]]; then
    DINEROD="${PROJECT_ROOT}/dinerod"
else
    echo "dinerod not found"
    exit 1
fi

command -v python3 >/dev/null 2>&1 || {
    echo "python3 is required for this test"
    exit 1
}

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

DATADIR=""
EXIT_CODE=0

cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    [[ -n "$DATADIR" ]] && pkill -9 -f "dinerod.*${DATADIR}" 2>/dev/null || true
    sleep 1
    if [[ $EXIT_CODE -ne 0 ]]; then
        echo -e "\n${RED}=== Bridge daemon.log (last 60 lines) ===${NC}"
        [[ -f "${DATADIR}/daemon.log" ]] && tail -60 "${DATADIR}/daemon.log"

        if [[ "$KEEP_TMP_ON_FAIL" == "1" ]]; then
            echo -e "\n${YELLOW}Keeping temp dir for debugging (KEEP_TMP_ON_FAIL=1):${NC}"
            [[ -n "$DATADIR" ]] && echo "  ${DATADIR}"
            return
        fi
    fi

    [[ -n "$DATADIR" && -d "$DATADIR" ]] && rm -rf "$DATADIR"
}
trap 'EXIT_CODE=$?; cleanup' EXIT

fail() { echo -e "${RED}FAILED: $1${NC}"; exit 1; }
pass() { echo -e "${GREEN}  $1${NC}"; }
info() { echo -e "${CYAN}$1${NC}"; }

rpc_call() {
    local port=$1
    local datadir=$2
    local method=$3
    shift 3
    local params="$*"
    local cookie=$(cat "${datadir}/.cookie" 2>/dev/null)
    [[ -z "$cookie" ]] && return 1
    local json_params="[]"
    [[ -n "$params" ]] && json_params="[$params]"
    curl -s -u "$cookie" -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"method\":\"$method\",\"params\":$json_params,\"id\":1}" \
        "http://127.0.0.1:${port}" 2>/dev/null
}

rpc_has_error() {
    local compact
    compact=$(echo "$1" | tr -d '\n\t ')
    [[ "$compact" == *"\"error\":null"* ]] && return 1
    [[ "$compact" == *"\"error\":"* ]] && return 0
    return 1
}

get_int_result() {
    echo "$1" | tr -d '\n\t' | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p'
}

wait_for_ready() {
    local port=$1
    local datadir=$2
    local timeout=$3
    local start=$(date +%s)
    while true; do
        local elapsed=$(($(date +%s) - start))
        [[ $elapsed -gt $timeout ]] && return 1
        if [[ -f "${datadir}/.cookie" ]]; then
            local r
            r=$(rpc_call "$port" "$datadir" "getblockcount" || true)
            if [[ -n "$(get_int_result "$r")" ]]; then
                return 0
            fi
        fi
        sleep 1
    done
}

echo ""
echo "================================================================="
echo "  GETPROOF Abuse Disconnect Integration Test"
echo "================================================================="
echo "  flood_count:         $FLOOD_COUNT"
echo "  disconnect_timeout:  ${DISCONNECT_TIMEOUT}s"
echo ""

# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
RPC_PORT=$(alloc_port_base)
P2P_PORT=$((RPC_PORT + 1))
DATADIR=$(mktemp -d -t dinero_getproof_abuse_XXXXXX)

info "[1/4] Starting bridge-capable node..."
echo "  RPC=$RPC_PORT P2P=$P2P_PORT"
"$DINEROD" --regtest \
    --datadir="$DATADIR" \
    --rpcport="$RPC_PORT" \
    --port="$P2P_PORT" \
    --listen=1 \
    --utreexo=1 \
    --utreexo-bridge=1 \
    >> "${DATADIR}/daemon.log" 2>&1 &

wait_for_ready "$RPC_PORT" "$DATADIR" 30 || fail "Node failed to start"
pass "Bridge node ready"
BASE_CONN_COUNT=$(get_int_result "$(rpc_call "$RPC_PORT" "$DATADIR" "getconnectioncount")")
[[ -z "$BASE_CONN_COUNT" ]] && fail "Could not read baseline connection count"
echo "  Baseline connection count: $BASE_CONN_COUNT"

info "\n[2/4] Handshake + GETPROOF flood from attacker socket..."
ATTACK_OUTPUT=$(
python3 - "$P2P_PORT" "$FLOOD_COUNT" "$DISCONNECT_TIMEOUT" <<'PY'
import hashlib
import os
import random
import socket
import struct
import sys
import time

MAGIC = 0xD1A0C0DE
NODE_NETWORK = 1 << 0
NODE_UTREEXO = 1 << 24

def dsha256(data: bytes) -> bytes:
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()

def mkframe(cmd: str, payload: bytes) -> bytes:
    header = struct.pack("<I12sI", MAGIC, cmd.encode("ascii").ljust(12, b"\x00"), len(payload))
    return header + dsha256(payload)[:4] + payload

def recv_exact(sock: socket.socket, n: int, timeout: float) -> bytes:
    sock.settimeout(timeout)
    out = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise EOFError("peer closed")
        out += chunk
    return out

def recv_frame(sock: socket.socket, timeout: float):
    hdr = recv_exact(sock, 24, timeout)
    magic, cmd_raw, length = struct.unpack("<I12sI", hdr[:20])
    if magic != MAGIC:
        raise RuntimeError(f"bad magic: {hex(magic)}")
    payload = recv_exact(sock, length, timeout) if length else b""
    cmd = cmd_raw.split(b"\x00", 1)[0].decode("ascii", errors="ignore")
    return cmd, payload

def encode_varint(v: int) -> bytes:
    if v < 0xFD:
        return bytes([v])
    if v <= 0xFFFF:
        return b"\xfd" + struct.pack("<H", v)
    if v <= 0xFFFFFFFF:
        return b"\xfe" + struct.pack("<I", v)
    return b"\xff" + struct.pack("<Q", v)

def make_version_payload(port: int) -> bytes:
    services = NODE_NETWORK | NODE_UTREEXO
    out = bytearray()
    out += struct.pack("<I", 70016)          # protocol version
    out += struct.pack("<Q", services)
    out += struct.pack("<Q", int(time.time()))
    # addr_recv
    out += struct.pack("<Q", services)
    out += (b"\x00" * 10) + b"\xff\xff" + b"\x7f\x00\x00\x01"
    out += struct.pack(">H", port)
    # addr_from
    out += struct.pack("<Q", services)
    out += (b"\x00" * 10) + b"\xff\xff" + b"\x7f\x00\x00\x01"
    out += struct.pack(">H", port)
    out += struct.pack("<Q", random.getrandbits(64))
    ua = b"/getproof-flood:1.0/"
    out += encode_varint(len(ua)) + ua
    out += struct.pack("<I", 0)              # start_height
    out += b"\x01"                           # relay
    return bytes(out)

def wait_for(sock: socket.socket, wanted: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        rem = max(0.1, deadline - time.time())
        try:
            cmd, payload = recv_frame(sock, rem)
        except (socket.timeout, TimeoutError):
            continue
        except EOFError:
            return False
        if cmd == "ping" and len(payload) >= 8:
            sock.sendall(mkframe("pong", payload[:8]))
            continue
        if cmd == wanted:
            return True
    return False

def getproof_payload(i: int) -> bytes:
    # 32-byte non-null block hash + 32-byte expected_root(zeros)
    h = bytearray(os.urandom(32))
    h[0] ^= 0x01
    h[1] ^= (i & 0xFF)
    return bytes(h) + (b"\x00" * 32)

def main() -> int:
    port = int(sys.argv[1])
    flood_count = int(sys.argv[2])
    disconnect_timeout = float(sys.argv[3])

    with socket.create_connection(("127.0.0.1", port), timeout=5.0) as sock:
        sock.sendall(mkframe("version", make_version_payload(port)))
        if not wait_for(sock, "version", 5.0):
            print("handshake_failed=missing_version")
            return 2
        sock.sendall(mkframe("verack", b""))
        if not wait_for(sock, "verack", 5.0):
            print("handshake_failed=missing_verack")
            return 2

        for i in range(flood_count):
            sock.sendall(mkframe("getproof", getproof_payload(i)))

        deadline = time.time() + disconnect_timeout
        disconnected = False
        probe_payload = getproof_payload(9999)
        while time.time() < deadline:
            try:
                sock.sendall(mkframe("getproof", probe_payload))
            except OSError:
                disconnected = True
                break

            sock.settimeout(0.25)
            try:
                b = sock.recv(1)
                if b == b"":
                    disconnected = True
                    break
            except socket.timeout:
                pass
            except OSError:
                disconnected = True
                break
            time.sleep(0.10)

    print(f"disconnected={1 if disconnected else 0}")
    return 0 if disconnected else 3

if __name__ == "__main__":
    raise SystemExit(main())
PY
) || fail "Attacker script failed"

echo "  $ATTACK_OUTPUT"
if ! echo "$ATTACK_OUTPUT" | grep -q "disconnected=1"; then
    fail "Expected attacker peer disconnect not observed"
fi
pass "Attacker peer disconnected after GETPROOF flood"

info "\n[3/4] Verifying attacker disconnect from node perspective..."
LOG_FILES="${DATADIR}/daemon.log ${DATADIR}/p2p.log"
LOG_HIT=0
for _ in $(seq 1 30); do
    if cat $LOG_FILES 2>/dev/null | grep -q "Disconnecting peer for repeated getproof abuse"; then
        LOG_HIT=1
        break
    fi
    sleep 0.5
done
[[ "$LOG_HIT" != "1" ]] && fail "Did not observe disconnect-abuse log marker"
pass "Disconnect reason logged"

CONN_RESULT=$(rpc_call "$RPC_PORT" "$DATADIR" "getconnectioncount" || true)
CONN_COUNT=$(get_int_result "$CONN_RESULT")
if [[ -n "$CONN_COUNT" ]]; then
    echo "  Connection count after test: $CONN_COUNT (baseline: $BASE_CONN_COUNT)"
fi

info "\n[4/4] Liveness check (RPC + mining still functional)..."
HEIGHT_BEFORE=$(get_int_result "$(rpc_call "$RPC_PORT" "$DATADIR" "getblockcount")")
[[ -z "$HEIGHT_BEFORE" ]] && fail "Failed to query height before liveness check"

WALLET_RESULT=$(rpc_call "$RPC_PORT" "$DATADIR" "wallet.createhd" '"abuse_liveness"' || true)
ADDR=$(echo "$WALLET_RESULT" | tr -d '\n\t' | sed -n 's/.*"first_address"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
[[ -z "$ADDR" ]] && fail "Failed to create wallet for liveness check"

MINE_RESULT=$(rpc_call "$RPC_PORT" "$DATADIR" "generatetoaddress" "1, \"$ADDR\"")
if rpc_has_error "$MINE_RESULT"; then
    fail "Mining failed after abuse disconnect"
fi

HEIGHT_AFTER=$(get_int_result "$(rpc_call "$RPC_PORT" "$DATADIR" "getblockcount")")
[[ -z "$HEIGHT_AFTER" ]] && fail "Failed to query height after liveness check"
if [[ "$HEIGHT_AFTER" -lt $((HEIGHT_BEFORE + 1)) ]]; then
    fail "Height did not advance after mining (before=$HEIGHT_BEFORE after=$HEIGHT_AFTER)"
fi
pass "Node remained live and mined successfully after abuse handling"

echo ""
echo "================================================================="
echo -e "${GREEN}  GETPROOF ABUSE DISCONNECT TEST PASSED${NC}"
echo "================================================================="
echo "Validated:"
echo "  - Excess GETPROOF traffic triggers peer disconnect"
echo "  - Disconnect event is logged by proof-gossip handler"
echo "  - Node stays healthy after abuse handling"
echo ""

exit 0
