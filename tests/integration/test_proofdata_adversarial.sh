#!/usr/bin/env bash
#
# Integration: PROOFDATA adversarial hardening
#
# Validates proof-gossip proofdata handling against:
#  1) bogus payloads (malformed proofdata)
#  2) replayed payloads (duplicate unsolicited proofdata)
#  3) flood behavior (rate-limit + disconnect)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

FLOOD_COUNT=${FLOOD_COUNT:-96}
DISCONNECT_TIMEOUT=${DISCONNECT_TIMEOUT:-12}
KEEP_TMP_ON_FAIL=${KEEP_TMP_ON_FAIL:-1}
MIN_REQUIRED_FLOOD_COUNT=72
if [[ "$FLOOD_COUNT" -lt "$MIN_REQUIRED_FLOOD_COUNT" ]]; then
    FLOOD_COUNT=$MIN_REQUIRED_FLOOD_COUNT
fi

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
        echo -e "\n${RED}=== Bridge daemon.log (last 80 lines) ===${NC}"
        [[ -f "${DATADIR}/daemon.log" ]] && tail -80 "${DATADIR}/daemon.log"

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
    local cookie
    cookie=$(cat "${datadir}/.cookie" 2>/dev/null)
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

json_result_field() {
    local json="$1"
    local key="$2"
    python3 - "$key" "$json" <<'PY'
import json
import sys

key = sys.argv[1]
try:
    payload = json.loads(sys.argv[2])
    result = payload.get("result", {})
    value = result.get(key)
    if isinstance(value, bool):
        print("1" if value else "0")
    elif isinstance(value, (int, float)):
        print(int(value))
    else:
        print("")
except Exception:
    print("")
PY
}

wait_for_ready() {
    local port=$1
    local datadir=$2
    local timeout=$3
    local start
    start=$(date +%s)
    while true; do
        local elapsed=$(( $(date +%s) - start ))
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

get_gossip_stats() {
    local port=$1
    local datadir=$2
    local out
    out=$(rpc_call "$port" "$datadir" "blockchain.getutreexogossipstats" || true)
    if rpc_has_error "$out" || [[ -z "$out" ]]; then
        out=$(rpc_call "$port" "$datadir" "utreexo.getgossip" || true)
    fi
    [[ -z "$out" ]] && return 1
    if rpc_has_error "$out"; then
        return 1
    fi
    echo "$out"
}

get_stat() {
    local json="$1"
    local key="$2"
    local v
    v=$(json_result_field "$json" "$key")
    [[ -n "$v" ]] || v=0
    echo "$v"
}

echo ""
echo "================================================================="
echo "  PROOFDATA Adversarial Hardening Test"
echo "================================================================="
echo "  flood_count:         $FLOOD_COUNT"
echo "  disconnect_timeout:  ${DISCONNECT_TIMEOUT}s"
echo ""

# shellcheck source=lib/port_alloc.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/port_alloc.sh"
RPC_PORT=$(alloc_port_base)
P2P_PORT=$((RPC_PORT + 1))
DATADIR=$(mktemp -d -t dinero_proofdata_abuse_XXXXXX)

info "[1/6] Starting bridge-capable node..."
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

info "\n[2/6] Preparing chain state with fresh proof cache entry..."
WALLET_RESULT=$(rpc_call "$RPC_PORT" "$DATADIR" "wallet.createhd" '"proof_abuse"' || true)
ADDR=$(echo "$WALLET_RESULT" | tr -d '\n\t' | sed -n 's/.*"first_address"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
[[ -z "$ADDR" ]] && fail "Failed to create wallet"

MINE_RESULT=$(rpc_call "$RPC_PORT" "$DATADIR" "generatetoaddress" "3, \"$ADDR\"" || true)
rpc_has_error "$MINE_RESULT" && fail "Failed to mine setup blocks"
TARGET_HEIGHT=$(get_int_result "$(rpc_call "$RPC_PORT" "$DATADIR" "getblockcount" || true)")
[[ -z "$TARGET_HEIGHT" ]] && fail "Could not query block height"
TARGET_HASH=$(echo "$(rpc_call "$RPC_PORT" "$DATADIR" "getblockhash" "$TARGET_HEIGHT" || true)" | tr -d '\n\t' | sed -n 's/.*"result"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
[[ -z "$TARGET_HASH" ]] && fail "Could not fetch target block hash"
pass "Prepared target block hash: ${TARGET_HASH:0:16}... (height=$TARGET_HEIGHT)"

BASE_STATS=$(get_gossip_stats "$RPC_PORT" "$DATADIR" || true)
[[ -z "$BASE_STATS" ]] && fail "Failed to read baseline gossip stats"

BASE_INVALID=$(get_stat "$BASE_STATS" "invalid_proofdata_payloads")
BASE_REPLAYED=$(get_stat "$BASE_STATS" "proofdata_replayed")
BASE_RATE_LIMITED=$(get_stat "$BASE_STATS" "proofdata_rate_limited")
BASE_DISCONNECTS=$(get_stat "$BASE_STATS" "peers_disconnected_for_abuse")

echo "  Baseline stats: invalid=$BASE_INVALID replayed=$BASE_REPLAYED rate_limited=$BASE_RATE_LIMITED disconnects=$BASE_DISCONNECTS"

info "\n[3/6] Running proofdata adversarial peer sequence (bogus/replay/flood)..."
ATTACK_OUTPUT=$(
python3 - "$P2P_PORT" "$TARGET_HASH" "$FLOOD_COUNT" "$DISCONNECT_TIMEOUT" <<'PY'
import binascii
import hashlib
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
    out += struct.pack("<I", 70016)
    out += struct.pack("<Q", services)
    out += struct.pack("<Q", int(time.time()))
    out += struct.pack("<Q", services)
    out += (b"\x00" * 10) + b"\xff\xff" + b"\x7f\x00\x00\x01"
    out += struct.pack(">H", port)
    out += struct.pack("<Q", services)
    out += (b"\x00" * 10) + b"\xff\xff" + b"\x7f\x00\x00\x01"
    out += struct.pack(">H", port)
    out += struct.pack("<Q", random.getrandbits(64))
    ua = b"/proofdata-adversarial:1.0/"
    out += encode_varint(len(ua)) + ua
    out += struct.pack("<I", 0)
    out += b"\x01"
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


def try_fetch_proofdata(sock: socket.socket, block_hash_hex: str) -> bytes:
    b = bytes.fromhex(block_hash_hex)
    candidates = [b, b[::-1]]
    for candidate in candidates:
        req_payload = candidate + (b"\x00" * 32)
        sock.sendall(mkframe("getproof", req_payload))

        deadline = time.time() + 2.5
        while time.time() < deadline:
            try:
                cmd, payload = recv_frame(sock, 0.4)
            except (socket.timeout, TimeoutError):
                continue
            except EOFError:
                return b""
            if cmd == "ping" and len(payload) >= 8:
                sock.sendall(mkframe("pong", payload[:8]))
                continue
            if cmd == "proofdata":
                return payload
    return b""


def main() -> int:
    port = int(sys.argv[1])
    block_hash_hex = sys.argv[2]
    flood_count = int(sys.argv[3])
    disconnect_timeout = float(sys.argv[4])

    with socket.create_connection(("127.0.0.1", port), timeout=5.0) as sock:
        sock.sendall(mkframe("version", make_version_payload(port)))
        if not wait_for(sock, "version", 5.0):
            print("handshake_failed=missing_version")
            return 2
        sock.sendall(mkframe("verack", b""))
        if not wait_for(sock, "verack", 5.0):
            print("handshake_failed=missing_verack")
            return 2

        proof_payload = try_fetch_proofdata(sock, block_hash_hex)
        if not proof_payload:
            print("proof_fetch_failed=1")
            return 2

        # 1) Bogus proofdata (too short to be valid).
        sock.sendall(mkframe("proofdata", b"\x01\x02\x03\x04\x05\x06\x07\x08"))
        time.sleep(0.1)

        # 2) Replay proofdata (send a previously-delivered valid proof payload back).
        sock.sendall(mkframe("proofdata", proof_payload))
        sock.sendall(mkframe("proofdata", proof_payload))
        time.sleep(0.1)

        # 3) Flood proofdata to trigger rate limiting / disconnect.
        flood_payload = proof_payload[:64] if len(proof_payload) >= 64 else (proof_payload + b"\x00" * (64 - len(proof_payload)))
        for _ in range(flood_count):
            sock.sendall(mkframe("proofdata", flood_payload))

        disconnected = False
        deadline = time.time() + disconnect_timeout
        probe = flood_payload
        while time.time() < deadline:
            try:
                sock.sendall(mkframe("proofdata", probe))
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
            time.sleep(0.1)

    print(f"proof_len={len(proof_payload)}")
    print(f"disconnected={1 if disconnected else 0}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
PY
) || fail "Attacker script failed"

echo "  $ATTACK_OUTPUT"
if echo "$ATTACK_OUTPUT" | grep -q "disconnected=1"; then
    pass "Attacker peer observed disconnect after proofdata flood"
else
    pass "Flood completed; validating disconnect via server metrics/logs"
fi

info "\n[4/6] Validating gossip metrics moved on all adversarial paths..."
POST_STATS=$(get_gossip_stats "$RPC_PORT" "$DATADIR" || true)
[[ -z "$POST_STATS" ]] && fail "Failed to read post-attack gossip stats"

POST_INVALID=$(get_stat "$POST_STATS" "invalid_proofdata_payloads")
POST_REPLAYED=$(get_stat "$POST_STATS" "proofdata_replayed")
POST_RATE_LIMITED=$(get_stat "$POST_STATS" "proofdata_rate_limited")
POST_DISCONNECTS=$(get_stat "$POST_STATS" "peers_disconnected_for_abuse")

echo "  Post stats: invalid=$POST_INVALID replayed=$POST_REPLAYED rate_limited=$POST_RATE_LIMITED disconnects=$POST_DISCONNECTS"

(( POST_INVALID > BASE_INVALID )) || fail "invalid_proofdata_payloads did not increase"
(( POST_REPLAYED > BASE_REPLAYED )) || fail "proofdata_replayed did not increase"
(( POST_RATE_LIMITED > BASE_RATE_LIMITED )) || fail "proofdata_rate_limited did not increase"
(( POST_DISCONNECTS > BASE_DISCONNECTS )) || fail "peers_disconnected_for_abuse did not increase"
pass "Bogus/replayed/flood paths reflected in gossip metrics"

info "\n[5/6] Verifying daemon log markers for proofdata abuse handling..."
LOG_HIT=0
for _ in $(seq 1 30); do
    if grep -q "Disconnecting peer for repeated proofdata abuse" "${DATADIR}/daemon.log" 2>/dev/null; then
        LOG_HIT=1
        break
    fi
    sleep 0.5
done
[[ "$LOG_HIT" != "1" ]] && fail "Did not observe proofdata abuse disconnect marker in log"
pass "Proofdata abuse disconnect marker observed"

info "\n[6/6] Liveness check (RPC + mining still functional)..."
HEIGHT_BEFORE=$(get_int_result "$(rpc_call "$RPC_PORT" "$DATADIR" "getblockcount" || true)")
[[ -z "$HEIGHT_BEFORE" ]] && fail "Failed to query height before liveness check"

MINE_RESULT_2=$(rpc_call "$RPC_PORT" "$DATADIR" "generatetoaddress" "1, \"$ADDR\"" || true)
rpc_has_error "$MINE_RESULT_2" && fail "Mining failed after proofdata abuse handling"

HEIGHT_AFTER=$(get_int_result "$(rpc_call "$RPC_PORT" "$DATADIR" "getblockcount" || true)")
[[ -z "$HEIGHT_AFTER" ]] && fail "Failed to query height after liveness check"
if [[ "$HEIGHT_AFTER" -lt $((HEIGHT_BEFORE + 1)) ]]; then
    fail "Height did not advance after mining (before=$HEIGHT_BEFORE after=$HEIGHT_AFTER)"
fi
pass "Node remained live and mined successfully after abuse handling"

echo ""
echo "================================================================="
echo -e "${GREEN}  PROOFDATA ADVERSARIAL TEST PASSED${NC}"
echo "================================================================="
echo "Validated:"
echo "  - Bogus proofdata payloads are rejected and counted"
echo "  - Replayed proofdata is detected and counted"
echo "  - Proofdata flood triggers rate-limit and peer disconnect"
echo "  - Node remains healthy after abuse handling"
echo ""

exit 0
