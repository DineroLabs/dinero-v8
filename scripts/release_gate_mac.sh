#!/bin/bash
# release_gate_mac.sh — Local release gate for Dinero macOS builds
# Must pass before any GitHub Release or server deployment.
# Usage: ./scripts/release_gate_mac.sh
set -uo pipefail

# ═══════════════════════════════════════════════════════════════════
# Configuration
# ═══════════════════════════════════════════════════════════════════
DINERO_SRC="${DINERO_SRC:-/Users/haydarevich/src/dinero}"
DINERO_BUILD="$DINERO_SRC/build"
QT_SRC="${QT_SRC:-/Users/haydarevich/src/dinero-qt}"
QT_BUILD="$QT_SRC/build"
DINEROD="$DINERO_BUILD/dinerod"
QT_DINEROD="$QT_BUILD/bin/dinero-qt.app/Contents/Resources/dinerod"
RPC_PORT=20998
P2P_PORT=20999
TEMP_DIR=""
DAEMON_PID=""
LOG_FILE=""
TEST_SEED="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
TEST_PASSWORD="gate-test-pw-2026"
JOBS="${JOBS:-8}"

PASS=0; FAIL=0; WARN=0
FIRST_ADDRESS=""

# ═══════════════════════════════════════════════════════════════════
# Helpers
# ═══════════════════════════════════════════════════════════════════
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; BOLD='\033[1m'; NC='\033[0m'

gate() {
    local name="$1"; shift
    if "$@"; then
        ((PASS++))
        echo -e "${GREEN}PASS${NC}: $name"
    else
        ((FAIL++))
        echo -e "${RED}FAIL${NC}: $name"
    fi
}

warn_gate() {
    local name="$1"; shift
    if "$@"; then
        ((PASS++))
        echo -e "${GREEN}PASS${NC}: $name"
    else
        ((WARN++))
        echo -e "${YELLOW}WARN${NC}: $name"
    fi
}

cleanup() {
    if [ -n "$DAEMON_PID" ]; then
        kill "$DAEMON_PID" 2>/dev/null
        wait "$DAEMON_PID" 2>/dev/null
    fi
    # Also kill anything still on our gate ports
    kill_gate_ports
    if [ -n "$TEMP_DIR" ] && [ -d "$TEMP_DIR" ]; then
        rm -rf "$TEMP_DIR"
    fi
}
trap cleanup EXIT

kill_gate_ports() {
    local pids
    pids=$(lsof -ti :$RPC_PORT -ti :$P2P_PORT 2>/dev/null | sort -u)
    if [ -n "$pids" ]; then
        echo "$pids" | xargs kill 2>/dev/null
        sleep 1
        # Force-kill stragglers
        pids=$(lsof -ti :$RPC_PORT -ti :$P2P_PORT 2>/dev/null | sort -u)
        [ -n "$pids" ] && echo "$pids" | xargs kill -9 2>/dev/null && sleep 1
    fi
}

get_cookie() {
    local datadir="$1"
    local cookie=""
    for path in "$datadir/.cookie" "$datadir/mainnet/.cookie"; do
        if [ -f "$path" ]; then
            cookie=$(cat "$path")
            cookie="${cookie#*:}"  # strip __cookie__: prefix
            echo "$cookie"
            return 0
        fi
    done
    return 1
}

rpc() {
    local method="$1"
    local params="${2:-[]}"
    local cookie
    cookie=$(get_cookie "$TEMP_DIR") || { echo '{"error":"no cookie"}'; return 1; }
    curl -s --max-time 30 -u "__cookie__:$cookie" \
        -X POST -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"1.0\",\"method\":\"$method\",\"params\":$params}" \
        "http://127.0.0.1:$RPC_PORT/"
}

start_daemon() {
    local extra_args=("$@")
    # Ensure ports are free before starting
    kill_gate_ports
    LOG_FILE="$TEMP_DIR/gate.log"
    "$DINEROD" --datadir="$TEMP_DIR" --rpc --rpcport="$RPC_PORT" \
        --port="$P2P_PORT" "${extra_args[@]}" > "$LOG_FILE" 2>&1 &
    DAEMON_PID=$!

    # Wait for cookie file first (max 15s)
    for i in $(seq 1 15); do
        sleep 1
        if [ -f "$TEMP_DIR/.cookie" ]; then
            break
        fi
    done

    # Wait for RPC ready (max 30s total)
    for i in $(seq 1 30); do
        if rpc "getblockchaininfo" 2>/dev/null | grep -q '"blocks"'; then
            return 0
        fi
        sleep 1
    done
    echo "Daemon failed to start within 45s"
    # Show last few log lines for diagnosis
    [ -f "$LOG_FILE" ] && tail -5 "$LOG_FILE" >&2
    return 1
}

stop_daemon() {
    if [ -n "$DAEMON_PID" ]; then
        kill "$DAEMON_PID" 2>/dev/null
        wait "$DAEMON_PID" 2>/dev/null || true
        DAEMON_PID=""
    fi
    sleep 2
}

# ═══════════════════════════════════════════════════════════════════
echo -e "${BOLD}═══════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  Dinero Release Gate — macOS${NC}"
echo -e "${BOLD}═══════════════════════════════════════════════════════${NC}"
echo ""

# Pre-flight: kill anything left on gate ports from previous runs
kill_gate_ports

# ═══════════════════════════════════════════════════════════════════
# Gate 1: Build Fresh
# ═══════════════════════════════════════════════════════════════════
echo -e "${BOLD}[Gate 1] Build fresh binaries${NC}"

gate_1_build() {
    echo "  Building dinerod + dinero-cli..."
    cmake --build "$DINERO_BUILD" --target dinerod dinero-cli -j"$JOBS" > /dev/null 2>&1 || return 1
    echo "  Building dinero-qt..."
    cmake --build "$QT_BUILD" --target dinero-qt -j"$JOBS" > /dev/null 2>&1 || return 1
    [ -x "$DINEROD" ] && [ -d "$QT_BUILD/bin/dinero-qt.app" ]
}
gate "Build dinerod + dinero-cli + dinero-qt" gate_1_build

# ═══════════════════════════════════════════════════════════════════
# Gate 2: Embedded Daemon Hash Parity
# ═══════════════════════════════════════════════════════════════════
echo -e "${BOLD}[Gate 2] Embedded daemon hash parity${NC}"

gate_2_hash() {
    if [ ! -f "$QT_DINEROD" ]; then
        echo "  Embedded dinerod not found at $QT_DINEROD"
        echo "  Copying fresh binary..."
        cp "$DINEROD" "$QT_DINEROD"
    fi

    local hash_standalone hash_embedded
    hash_standalone=$(shasum -a 256 "$DINEROD" | awk '{print $1}')
    hash_embedded=$(shasum -a 256 "$QT_DINEROD" | awk '{print $1}')

    echo "  Standalone: $hash_standalone"
    echo "  Embedded:   $hash_embedded"

    if [ "$hash_standalone" != "$hash_embedded" ]; then
        echo "  MISMATCH — copying fresh binary into bundle..."
        cp "$DINEROD" "$QT_DINEROD"
        hash_embedded=$(shasum -a 256 "$QT_DINEROD" | awk '{print $1}')
        [ "$hash_standalone" = "$hash_embedded" ] || return 1
        echo "  Fixed: $hash_embedded"
    fi
    return 0
}
gate "Embedded dinerod hash matches standalone" gate_2_hash

# ═══════════════════════════════════════════════════════════════════
# Gate 3: Deterministic Wallet Restore
# ═══════════════════════════════════════════════════════════════════
echo -e "${BOLD}[Gate 3] Deterministic wallet restore${NC}"

TEMP_DIR=$(mktemp -d /tmp/din-gate-XXXXXX)
start_daemon --connect=0 --listen=0 --dnsseed=0

gate_3_restore() {
    # Restore with test seed
    local result
    result=$(rpc "wallet.restore" "[\"gate_test\",\"$TEST_SEED\",\"\",\"\",\"bip86\"]")

    local success
    success=$(echo "$result" | python3 -c "import sys,json; r=json.load(sys.stdin); print(r.get('result',{}).get('success',''))" 2>/dev/null)
    if [ "$success" != "True" ]; then
        echo "  Restore failed: $result"
        return 1
    fi

    FIRST_ADDRESS=$(echo "$result" | python3 -c "import sys,json; r=json.load(sys.stdin); print(r.get('result',{}).get('first_address',''))" 2>/dev/null)
    echo "  First address: $FIRST_ADDRESS"

    # Must start with din1p (Taproot)
    if [[ ! "$FIRST_ADDRESS" =~ ^din1p ]]; then
        echo "  Address does not start with din1p"
        return 1
    fi

    # Restore again with expected_first_address (identity guard)
    stop_daemon
    rm -rf "$TEMP_DIR"
    TEMP_DIR=$(mktemp -d /tmp/din-gate-XXXXXX)
    start_daemon --connect=0 --listen=0 --dnsseed=0

    local result2
    result2=$(rpc "wallet.restore" "[\"gate_test2\",\"$TEST_SEED\",\"\",\"\",\"bip86\",\"$FIRST_ADDRESS\"]")
    local success2
    success2=$(echo "$result2" | python3 -c "import sys,json; r=json.load(sys.stdin); print(r.get('result',{}).get('success',''))" 2>/dev/null)
    if [ "$success2" != "True" ]; then
        echo "  Identity guard restore failed: $result2"
        return 1
    fi
    echo "  Identity guard: matched"

    # Altered mnemonic with same expected address — must FAIL
    local altered_seed="abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon zoo"
    local result3
    result3=$(rpc "wallet.restore" "[\"gate_test3\",\"$altered_seed\",\"\",\"\",\"bip86\",\"$FIRST_ADDRESS\"]")
    local success3
    success3=$(echo "$result3" | python3 -c "import sys,json; r=json.load(sys.stdin); print(r.get('result',{}).get('success',''))" 2>/dev/null)
    if [ "$success3" = "True" ]; then
        echo "  DANGER: Altered mnemonic accepted with wrong expected address!"
        return 1
    fi
    echo "  Altered mnemonic correctly rejected"
    return 0
}
gate "Deterministic wallet restore + identity guard" gate_3_restore

# ═══════════════════════════════════════════════════════════════════
# Gate 4: Unlock Wording + State
# ═══════════════════════════════════════════════════════════════════
echo -e "${BOLD}[Gate 4] Unlock wording and state${NC}"

gate_4_unlock() {
    # Encrypt wallet
    local enc_result
    enc_result=$(rpc "wallet.encrypt" "[\"$TEST_PASSWORD\"]")
    echo "  Encrypted wallet"

    # Wrong password — must say "Invalid password"
    local wrong_result
    wrong_result=$(rpc "wallet.unlock" "[\"wrong-password-123\"]")
    if echo "$wrong_result" | grep -qi "passphrase"; then
        echo "  ERROR: Response contains 'passphrase' instead of 'password'"
        echo "  $wrong_result"
        return 1
    fi
    if ! echo "$wrong_result" | grep -qi "Invalid password"; then
        echo "  ERROR: Wrong password response missing 'Invalid password'"
        echo "  $wrong_result"
        return 1
    fi
    echo "  Wrong password: correct wording"

    # Correct password — must unlock (check for success:true or unlocked:true)
    local right_result
    right_result=$(rpc "wallet.unlock" "[\"$TEST_PASSWORD\"]")
    if ! echo "$right_result" | grep -qE '"success"\s*:\s*true|"unlocked"\s*:\s*true'; then
        echo "  ERROR: Correct password did not unlock"
        echo "  $right_result"
        return 1
    fi
    echo "  Correct password: unlocked"
    return 0
}
gate "Unlock wording (password not passphrase) + state" gate_4_unlock

# ═══════════════════════════════════════════════════════════════════
# Gate 5: Birthday Height
# ═══════════════════════════════════════════════════════════════════
echo -e "${BOLD}[Gate 5] Birthday height respected${NC}"

gate_5_birthday() {
    local wallet_db=""
    # Find a wallet DB from gate tests
    for name in wallet_gate_test2 wallet_gate_test wallet_default; do
        if [ -f "$TEMP_DIR/wallets/${name}.db" ]; then
            wallet_db="$TEMP_DIR/wallets/${name}.db"
            break
        fi
    done
    if [ -z "$wallet_db" ]; then
        wallet_db=$(find "$TEMP_DIR/wallets" -name "*.db" 2>/dev/null | head -1)
    fi

    if [ -z "$wallet_db" ] || [ ! -f "$wallet_db" ]; then
        echo "  No wallet DB found in $TEMP_DIR/wallets/"
        ls -la "$TEMP_DIR/wallets/" 2>/dev/null
        return 1
    fi
    echo "  Using DB: $(basename "$wallet_db")"

    # Verify wallet_meta has birthday_height column
    local has_column
    has_column=$(sqlite3 "$wallet_db" "PRAGMA table_info(wallet_meta)" 2>/dev/null | grep -c "birthday_height")
    if [ "${has_column:-0}" -eq 0 ]; then
        echo "  ERROR: birthday_height column missing from wallet_meta"
        return 1
    fi
    echo "  birthday_height column exists in wallet_meta"

    # Query the value (0 is valid for isolated node with no chain)
    local birthday
    birthday=$(sqlite3 "$wallet_db" "SELECT COALESCE(birthday_height, 'NULL') FROM wallet_meta WHERE id = 1" 2>/dev/null)
    echo "  birthday_height value: ${birthday:-<no row>}"

    if [ -z "$birthday" ]; then
        echo "  ERROR: No wallet_meta row found"
        return 1
    fi

    # Must be a number (not NULL) — restore should always set it
    if [ "$birthday" = "NULL" ]; then
        echo "  ERROR: birthday_height is NULL (restore should set it)"
        return 1
    fi

    return 0
}
gate "Birthday height in wallet_meta" gate_5_birthday

# Stop isolated daemon for network tests
stop_daemon

# ═══════════════════════════════════════════════════════════════════
# Gate 6: Mainnet Peer + Sync (60s soak)
# ═══════════════════════════════════════════════════════════════════
echo -e "${BOLD}[Gate 6] Mainnet peer connection + sync (60s)${NC}"

gate_6_peers() {
    rm -rf "$TEMP_DIR"
    TEMP_DIR=$(mktemp -d /tmp/din-gate-XXXXXX)
    start_daemon --listen

    echo "  Waiting 60s for peer connections and header download..."
    sleep 60

    # Check peer count
    local peer_count
    peer_count=$(rpc "getpeerinfo" | python3 -c "import sys,json; r=json.load(sys.stdin); print(len(r.get('result',[])))" 2>/dev/null)
    echo "  Peers: ${peer_count:-0}"
    if [ "${peer_count:-0}" -lt 2 ]; then
        echo "  ERROR: < 2 peers after 60s"
        return 1
    fi

    # Check headers downloaded
    local headers
    headers=$(rpc "getblockchaininfo" | python3 -c "import sys,json; r=json.load(sys.stdin); print(r.get('result',{}).get('headers',0))" 2>/dev/null)
    echo "  Headers: ${headers:-0}"
    if [ "${headers:-0}" -lt 1 ]; then
        echo "  ERROR: No headers downloaded after 60s"
        return 1
    fi

    # Check for error loops in log
    local error_count
    error_count=$(grep -cE 'bad-utreexo-root|FATAL|DB corruption|assert.*failed' "$LOG_FILE" 2>/dev/null || true)
    error_count=${error_count:-0}
    echo "  Error pattern count: $error_count"
    if [ "$error_count" -ge 3 ]; then
        echo "  ERROR: Repeating error patterns in log ($error_count occurrences)"
        grep -E 'bad-utreexo-root|FATAL|DB corruption|assert.*failed' "$LOG_FILE" | head -5
        return 1
    fi

    return 0
}
gate "Mainnet peers (>=2) + headers downloading + no error loops" gate_6_peers

# ═══════════════════════════════════════════════════════════════════
# Gate 7: Restart Resilience (3 cycles)
# ═══════════════════════════════════════════════════════════════════
echo -e "${BOLD}[Gate 7] Restart resilience (3 cycles)${NC}"

gate_7_restart() {
    local prev_height=0

    for cycle in 1 2 3; do
        stop_daemon
        start_daemon --listen || { echo "  Cycle $cycle: daemon failed to start"; return 1; }

        # Verify RPC works (cookie auth)
        local info
        info=$(rpc "getblockchaininfo")
        if echo "$info" | grep -q "Unauthorized"; then
            echo "  Cycle $cycle: cookie auth failure"
            return 1
        fi

        local height
        height=$(echo "$info" | python3 -c "import sys,json; r=json.load(sys.stdin); print(r.get('result',{}).get('blocks',0))" 2>/dev/null)
        echo "  Cycle $cycle: height=$height (prev=$prev_height)"

        if [ "${height:-0}" -lt "$prev_height" ]; then
            echo "  Cycle $cycle: height regressed!"
            return 1
        fi
        prev_height="${height:-0}"
    done

    echo "  All 3 restart cycles passed"
    return 0
}
gate "Restart resilience (3 cycles, no auth failure, no regression)" gate_7_restart

# ═══════════════════════════════════════════════════════════════════
# Gate 8: Doctor (warn-only)
# ═══════════════════════════════════════════════════════════════════
echo -e "${BOLD}[Gate 8] Doctor diagnostic (warn-only)${NC}"

gate_8_doctor() {
    stop_daemon  # Doctor runs offline
    local doctor_result exit_code
    doctor_result=$("$DINEROD" doctor --json --datadir="$TEMP_DIR" 2>&1)
    exit_code=$?

    echo "  Doctor exit code: $exit_code"
    if [ "$exit_code" -eq 0 ]; then
        echo "  Healthy"
    elif [ "$exit_code" -eq 1 ]; then
        echo "  Warnings found (non-critical)"
    elif [ "$exit_code" -eq 2 ]; then
        echo "  Critical findings"
        echo "$doctor_result" | python3 -c "
import sys,json
try:
    d=json.load(sys.stdin)
    for c in d.get('checks',[]):
        if c.get('severity','') == 'critical':
            print(f\"  - {c.get('id','?')}: {c.get('message','?')}\")
except: pass" 2>/dev/null
        return 1
    else
        echo "  Doctor internal error"
        return 1
    fi
    return 0
}
warn_gate "Doctor diagnostic" gate_8_doctor

# ═══════════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════════
echo ""
echo -e "${BOLD}═══════════════════════════════════════════════════════${NC}"
echo -e "${BOLD}  Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}, ${YELLOW}$WARN warnings${NC}"
echo -e "${BOLD}═══════════════════════════════════════════════════════${NC}"

if [ "$FAIL" -gt 0 ]; then
    echo -e "${RED}RELEASE BLOCKED — fix failures before releasing${NC}"
    exit 1
else
    echo -e "${GREEN}RELEASE GATE PASSED — safe to tag and deploy${NC}"
    exit 0
fi
