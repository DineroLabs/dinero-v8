#!/usr/bin/env bash
set -euo pipefail

# ================================================
# Phase 18.3 - ChainDB Mining Consistency Test
# -----------------------------------------------
# Validates the Phase 18 invariant:
#   Mining template → mined block → ChainDB hash →
#   RPC-retrieved block must all agree.
#
# Prevents regression of the generatetoaddress hash bug.
# ================================================

DIND=./build/dinerod
DINCLI="./build/dinero-cli -datadir=/tmp/phase18_test"

DATADIR=/tmp/phase18_test
LOGFILE=$DATADIR/d.log

rm -rf "$DATADIR"
mkdir -p "$DATADIR"

echo "[TEST] Starting DineroCoin regtest node..."
$DIND --regtest --datadir="$DATADIR" 2>&1 > "$LOGFILE" &
NODE_PID=$!

sleep 3

# Helper to mine a block and return the hash
mine_block() {
    local hash_mined
    local best

    # Use a hardcoded regtest address (no wallet required)
    local addr="rdin1qv8epkuar78ujlecxalg4cp8665e5jmezr9r9n0"

    hash_mined=$($DINCLI generatetoaddress 1 "$addr" | jq -r '.[0]')
    echo "[TEST]   Block mined via generatetoaddress: $hash_mined"

    best=$($DINCLI getbestblockhash | tr -d '"')
    echo "[TEST]   Best block hash (ChainDB):         $best"

    if [[ "$hash_mined" != "$best" ]]; then
        echo "[ERROR] Hash mismatch!"
        echo "  mined: $hash_mined"
        echo "  best:  $best"
        kill -9 $NODE_PID || true
        exit 1
    fi

    echo "[TEST] Hashes match ✔"

    # Verify getblock returns valid JSON
    block_json=$($DINCLI getblock "$hash_mined" || true)
    if ! echo "$block_json" | jq . >/dev/null 2>&1; then
        echo "[ERROR] getblock returned invalid JSON!"
        kill -9 $NODE_PID || true
        exit 1
    fi

    height=$(echo "$block_json" | jq -r '.height')
    echo "[TEST] Block retrieved at height: $height"

    if [[ "$height" != "$1" ]]; then
        echo "[ERROR] Expected block height $1 but got $height"
        kill -9 $NODE_PID || true
        exit 1
    fi

    echo "[TEST] Block height correct ✔"
    echo
}

# Mine 3 blocks and validate each
# Note: Genesis block is at height 1, so first mined block is height 2
echo "[TEST] Mining and verifying block 2..."
mine_block 2

echo "[TEST] Mining and verifying block 3..."
mine_block 3

echo "[TEST] Mining and verifying block 4..."
mine_block 4

echo "[TEST] All ChainDB mining consistency checks passed ✔✔✔"
kill -9 $NODE_PID || true
exit 0
