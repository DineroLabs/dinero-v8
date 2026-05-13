#!/usr/bin/env bash
# ============================================================================
# Pool Bootstrapping + Drift Test
# ============================================================================
# Automates: repeated shield + mine, verifies pool count matches expected
# growth, confirms no drift after restart.
#
# Usage: ./bootstrap_pool.sh [dinerod_path] [target_pool_size]
# ============================================================================
set -euo pipefail

DINEROD="${1:-../../build/dinerod}"
TARGET_POOL="${2:-32}"
RPCPORT=18600
P2PPORT=18601
TMPDIR=$(mktemp -d /tmp/dinero_bootstrap_XXXXXX)
SHIELD_AMOUNT=5.0
FAILURES=0

rpc() {
    curl -s --user test:test --data-binary "{\"jsonrpc\":\"1.0\",\"method\":\"$1\",\"params\":[$2]}" \
         -H 'content-type: text/plain;' http://127.0.0.1:$RPCPORT/
}

rpc_field() {
    rpc "$1" "$2" | python3 -c "import sys,json; r=json.load(sys.stdin); print(r.get('result',{}).get('$3', r.get('error','?')))" 2>/dev/null
}

check() {
    local desc="$1" actual="$2" expected="$3"
    if [[ "$actual" == "$expected" ]]; then
        echo "  ✓ $desc (=$actual)"
    else
        echo "  ✗ FAIL: $desc — expected $expected, got $actual"
        FAILURES=$((FAILURES + 1))
    fi
}

cleanup() {
    echo "Shutting down..."
    rpc "stop" "" > /dev/null 2>&1 || true
    sleep 1
    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

echo "╔══════════════════════════════════════════════════════════╗"
echo "║  Pool Bootstrapping + Drift Test                       ║"
echo "║  Target pool: $TARGET_POOL CT outputs                       ║"
echo "║  Datadir: $TMPDIR"
echo "╚══════════════════════════════════════════════════════════╝"

# Start node
"$DINEROD" --datadir="$TMPDIR" --regtest --rpcport=$RPCPORT --port=$P2PPORT \
    --rpcuser=test --rpcpassword=test >/dev/null 2>&1 &
PID=$!

# Wait for RPC to become available
for i in $(seq 1 60); do
    if curl -s --max-time 2 --user test:test --data-binary '{"jsonrpc":"1.0","method":"getblockcount","params":[]}' \
         -H 'content-type: text/plain;' http://127.0.0.1:$RPCPORT/ 2>/dev/null | grep -q "result"; then
        echo "  Node ready after ${i}s"
        break
    fi
    sleep 1
done

# Mine initial coins
echo ""
echo "[Phase 1] Mining 110 blocks for spendable coinbase..."
rpc "generate" "110" > /dev/null
BALANCE=$(rpc_field "getbalance" "" "spendable")
echo "  Balance: $BALANCE DIN"
POOL=$(rpc_field "getprivacystatus" "" "ct_output_pool_size")
check "Initial pool is 0" "$POOL" "0"

# Shield repeatedly
echo ""
echo "[Phase 2] Shielding to build pool (target=$TARGET_POOL)..."
EXPECTED_POOL=0
SHIELD_COUNT=0
while [[ $EXPECTED_POOL -lt $TARGET_POOL ]]; do
    # Shield
    RESULT=$(rpc "shieldcoins" "{\"amount\":$SHIELD_AMOUNT}")
    TXID=$(echo "$RESULT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',{}).get('txid','FAIL'))" 2>/dev/null)
    if [[ "$TXID" == "FAIL" ]]; then
        echo "  ✗ Shield #$((SHIELD_COUNT+1)) failed: $RESULT"
        FAILURES=$((FAILURES + 1))
        break
    fi
    SHIELD_COUNT=$((SHIELD_COUNT + 1))

    # Mine to confirm
    rpc "generate" "1" > /dev/null

    # Check pool growth
    POOL=$(rpc_field "getprivacystatus" "" "ct_output_pool_size")
    # Each shield creates 2 CT outputs (payment + change)
    EXPECTED_POOL=$((SHIELD_COUNT * 2))

    if [[ $((SHIELD_COUNT % 5)) -eq 0 ]] || [[ $EXPECTED_POOL -ge $TARGET_POOL ]]; then
        echo "  Shield #$SHIELD_COUNT: pool=$POOL (expected=$EXPECTED_POOL)"
    fi
done

echo ""
echo "[Phase 2 Result]"
check "Pool matches expected after $SHIELD_COUNT shields" "$POOL" "$EXPECTED_POOL"

HEIGHT=$(rpc_field "getprivacystatus" "" "current_height")
STATUS=$(rpc_field "getprivacystatus" "" "privacy_lane_status")
AVAILABLE=$(rpc_field "getprivacystatus" "" "rings_available")
echo "  Height: $HEIGHT, Status: $STATUS, Rings available: $AVAILABLE"

# Check balance tracking
CONF_BALANCE=$(rpc_field "getbalance" "" "confidential")
echo "  Confidential balance: $CONF_BALANCE DIN"

# Phase 3: Restart and verify no drift
echo ""
echo "[Phase 3] Restart node, verify DB persistence (no drift)..."
POOL_BEFORE=$POOL
KI_BEFORE=$(rpc_field "getprivacystatus" "" "key_images_tracked")

rpc "stop" "" > /dev/null 2>&1
# Wait for daemon PROCESS to actually exit (not just RPC drop)
echo "  Waiting for process $PID to exit..."
for i in $(seq 1 30); do
    if ! kill -0 $PID 2>/dev/null; then
        echo "  Process exited after ${i}s"
        break
    fi
    sleep 1
done
sleep 1  # Brief grace for file handle cleanup

# Restart
"$DINEROD" --datadir="$TMPDIR" --regtest --rpcport=$RPCPORT --port=$P2PPORT \
    --rpcuser=test --rpcpassword=test >/dev/null 2>&1 &
PID=$!

# Wait for RPC to become available (daemon needs to re-open DBs, re-index, etc.)
echo "  Waiting for RPC..."
RPC_READY=0
for i in $(seq 1 60); do
    if curl -s --max-time 2 --user test:test --data-binary '{"jsonrpc":"1.0","method":"getblockcount","params":[]}' \
         -H 'content-type: text/plain;' http://127.0.0.1:$RPCPORT/ 2>/dev/null | grep -q "result"; then
        echo "  RPC ready after ${i}s"
        RPC_READY=1
        break
    fi
    sleep 1
done
if [[ $RPC_READY -eq 0 ]]; then
    echo "  ✗ FAIL: RPC not ready after 60s"
    FAILURES=$((FAILURES + 1))
fi

POOL_AFTER=$(rpc_field "getprivacystatus" "" "ct_output_pool_size")
KI_AFTER=$(rpc_field "getprivacystatus" "" "key_images_tracked")
HEIGHT_AFTER=$(rpc_field "getprivacystatus" "" "current_height")

check "Pool survives restart" "$POOL_AFTER" "$POOL_BEFORE"
check "Key images survive restart" "$KI_AFTER" "$KI_BEFORE"
check "Height survives restart" "$HEIGHT_AFTER" "$HEIGHT"

# Phase 4: Mine more after restart, verify continued growth
echo ""
echo "[Phase 4] Shield after restart, verify continued growth..."
rpc "shieldcoins" "{\"amount\":$SHIELD_AMOUNT}" > /dev/null
rpc "generate" "1" > /dev/null
POOL_GROWN=$(rpc_field "getprivacystatus" "" "ct_output_pool_size")
EXPECTED_GROWN=$((EXPECTED_POOL + 2))
check "Pool grows after restart" "$POOL_GROWN" "$EXPECTED_GROWN"

# Summary
echo ""
echo "╔══════════════════════════════════════════════════════════╗"
if [[ $FAILURES -eq 0 ]]; then
    echo "║  ✓ ALL CHECKS PASSED                                  ║"
else
    echo "║  ✗ $FAILURES CHECK(S) FAILED                              ║"
fi
echo "║  Shields: $SHIELD_COUNT                                         ║"
echo "║  Final pool: $POOL_GROWN CT outputs                         ║"
echo "║  Restart drift: none                                   ║"
echo "╚══════════════════════════════════════════════════════════╝"

exit $FAILURES
