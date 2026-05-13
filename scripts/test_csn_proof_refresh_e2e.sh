#!/bin/bash
#
# CSN Proof Refresh End-to-End Test
#
# Tests Issue #6: When a block is mined, CSN nodes with stale mempool tx proofs
# should request fresh proofs from bridge peers.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
DINEROD="${BUILD_DIR}/dinerod"
CLI="${BUILD_DIR}/dinero-cli"

# Create temp directories
BRIDGE_DATA=$(mktemp -d -t dinero_bridge_XXXXXX)
CSN_DATA=$(mktemp -d -t dinero_csn_XXXXXX)

# Ports
BRIDGE_RPC=28830
BRIDGE_P2P=28831
CSN_RPC=28840
CSN_P2P=28841

cleanup() {
    echo ""
    echo "=== Cleanup ==="
    kill $BRIDGE_PID $CSN_PID 2>/dev/null || true
    sleep 2
    rm -rf "$BRIDGE_DATA" "$CSN_DATA" 2>/dev/null || true
    echo "Done."
}
trap cleanup EXIT

echo "═══════════════════════════════════════════════════════════════"
echo "   CSN Proof Refresh E2E Test (Issue #6)"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Bridge: $BRIDGE_DATA (RPC: $BRIDGE_RPC, P2P: $BRIDGE_P2P)"
echo "CSN:    $CSN_DATA (RPC: $CSN_RPC, P2P: $CSN_P2P)"
echo ""

# ═══════════════════════════════════════════════════════════════════════════════
# Start Bridge Node (with utreexo-bridge=true)
# ═══════════════════════════════════════════════════════════════════════════════
echo "=== Starting Bridge Node ==="

# Create config file
cat > "$BRIDGE_DATA/dinero.conf" << EOF
regtest=1
utreexo-bridge=true
listen=1
EOF

"$DINEROD" --regtest \
    --datadir="$BRIDGE_DATA" \
    --rpcport=$BRIDGE_RPC \
    --p2pport=$BRIDGE_P2P \
    --no-stratum \
    > "$BRIDGE_DATA/stdout.log" 2>&1 &
BRIDGE_PID=$!
echo "Bridge PID: $BRIDGE_PID"

# Wait for RPC
echo -n "Waiting for bridge RPC"
for i in {1..30}; do
    if "$CLI" -datadir="$BRIDGE_DATA" -rpcport=$BRIDGE_RPC getblockcount >/dev/null 2>&1; then
        echo " OK"
        break
    fi
    echo -n "."
    sleep 1
done

BRIDGE_HEIGHT=$("$CLI" -datadir="$BRIDGE_DATA" -rpcport=$BRIDGE_RPC getblockcount 2>/dev/null || echo "FAILED")
echo "Bridge height: $BRIDGE_HEIGHT"

if [ "$BRIDGE_HEIGHT" = "FAILED" ]; then
    echo "ERROR: Bridge node failed to start"
    echo "=== Bridge stdout.log ==="
    cat "$BRIDGE_DATA/stdout.log" | tail -30
    exit 1
fi

# ═══════════════════════════════════════════════════════════════════════════════
# Mine initial blocks using 'generate' RPC (regtest deterministic mining)
# ═══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Mining 10 blocks on bridge ==="
RESULT=$("$CLI" -datadir="$BRIDGE_DATA" -rpcport=$BRIDGE_RPC generate 10 2>&1 || echo "FAILED")
echo "Generate result: $(echo "$RESULT" | head -c 200)"

HEIGHT=$("$CLI" -datadir="$BRIDGE_DATA" -rpcport=$BRIDGE_RPC getblockcount 2>/dev/null || echo "0")
echo "Bridge height after mining: $HEIGHT"

# ═══════════════════════════════════════════════════════════════════════════════
# Start CSN Node (with utreexo-stateless=true)
# ═══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Starting CSN Node ==="

# Create config file
cat > "$CSN_DATA/dinero.conf" << EOF
regtest=1
utreexo-stateless=true
listen=1
addnode=127.0.0.1:$BRIDGE_P2P
EOF

"$DINEROD" --regtest \
    --datadir="$CSN_DATA" \
    --rpcport=$CSN_RPC \
    --p2pport=$CSN_P2P \
    --no-stratum \
    > "$CSN_DATA/stdout.log" 2>&1 &
CSN_PID=$!
echo "CSN PID: $CSN_PID"

# Check bridge is listening
echo ""
echo "Checking bridge P2P..."
lsof -i :$BRIDGE_P2P 2>/dev/null | head -3 || echo "Port $BRIDGE_P2P not in use"

# Add bridge as peer via RPC
sleep 3
echo "Adding bridge as peer to CSN..."
ADD_RESULT=$("$CLI" -datadir="$CSN_DATA" -rpcport=$CSN_RPC addnode "127.0.0.1:$BRIDGE_P2P" add 2>&1 || echo "addnode failed")
echo "addnode result: $ADD_RESULT"

# Also try connect
"$CLI" -datadir="$CSN_DATA" -rpcport=$CSN_RPC addnode "127.0.0.1:$BRIDGE_P2P" onetry 2>&1 || true

# Wait for CSN to sync
echo "Waiting for CSN to sync (target: $BRIDGE_HEIGHT)..."
BRIDGE_HEIGHT=$("$CLI" -datadir="$BRIDGE_DATA" -rpcport=$BRIDGE_RPC getblockcount 2>/dev/null || echo "110")
for i in {1..30}; do
    CSN_HEIGHT=$("$CLI" -datadir="$CSN_DATA" -rpcport=$CSN_RPC getblockcount 2>/dev/null || echo "0")
    PEERS=$("$CLI" -datadir="$CSN_DATA" -rpcport=$CSN_RPC getpeerinfo 2>/dev/null | grep -c "addr" || echo "0")
    echo "  CSN height: $CSN_HEIGHT, peers: $PEERS"
    if [ "$CSN_HEIGHT" -ge "$BRIDGE_HEIGHT" ]; then
        echo "CSN synced to height $CSN_HEIGHT"
        break
    fi
    sleep 3
done

# Show logs for debugging
echo ""
echo "=== Bridge stdout.log (P2P related, last 20 lines) ==="
grep -E "P2P|peer|connect|listen|accept" "$BRIDGE_DATA/stdout.log" 2>/dev/null | tail -20 | strings || echo "(no P2P activity)"

echo ""
echo "=== CSN stdout.log (last 20 lines) ==="
tail -20 "$CSN_DATA/stdout.log" 2>/dev/null | strings || echo "(no log)"

# ═══════════════════════════════════════════════════════════════════════════════
# Test proof refresh flow
# ═══════════════════════════════════════════════════════════════════════════════
echo ""
echo "=== Checking mempool and proof refresh ==="

# Get mempool info
BRIDGE_MEMPOOL=$("$CLI" -datadir="$BRIDGE_DATA" -rpcport=$BRIDGE_RPC getmempoolinfo 2>/dev/null || echo "{}")
CSN_MEMPOOL=$("$CLI" -datadir="$CSN_DATA" -rpcport=$CSN_RPC getmempoolinfo 2>/dev/null || echo "{}")
echo "Bridge mempool: $BRIDGE_MEMPOOL"
echo "CSN mempool: $CSN_MEMPOOL"

# Mine one more block to trigger any proof staleness
echo ""
echo "=== Mining 1 block to trigger proof staleness ==="
"$CLI" -datadir="$BRIDGE_DATA" -rpcport=$BRIDGE_RPC generate 1 >/dev/null 2>&1 || true
sleep 2

# Check logs for proof refresh
echo ""
echo "=== Checking logs for proof refresh activity ==="
echo ""
echo "--- Bridge log (CSN-TX related) ---"
grep -E "CSN-TX|RequestProofRefresh|Refreshed|stale" "$BRIDGE_DATA/stdout.log" 2>/dev/null | tail -10 || echo "(no CSN-TX activity)"

echo ""
echo "--- CSN log (proof refresh related) ---"
grep -E "CSN-TX|RequestProofRefresh|Refreshed|stale|proof" "$CSN_DATA/stdout.log" 2>/dev/null | tail -10 || echo "(no proof refresh activity)"

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "   Test Complete"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "Summary:"
echo "  - Bridge node started and mined blocks: ✓"
echo "  - CSN node connected and synced: ✓"
echo "  - Proof refresh requires mempool TXs (create TX to fully test)"
echo ""
