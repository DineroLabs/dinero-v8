#!/usr/bin/env bash
# ============================================================================
# Mainnet Pool Bootstrap
# ============================================================================
# Shields DIN in batches to build the CT output pool to 1024.
# Runs on a server with spendable balance. Does NOT call generate —
# waits for natural mining between batches.
#
# Usage: ./bootstrap_mainnet_pool.sh [rpc_port] [cookie_path] [batch_size] [target_pool]
# ============================================================================
set -euo pipefail

RPCPORT="${1:-20998}"
COOKIE_PATH="${2:-/root/Dinero-Coin/data-main/.cookie}"
BATCH_SIZE="${3:-10}"        # Shields per batch (conservative — mempool limit is 50)
TARGET_POOL="${4:-1024}"
SHIELD_AMOUNT="1.0"          # DIN per shield (small, just building pool)
WAIT_BETWEEN_BATCHES=90      # Seconds to wait for block confirmation

COOKIE_VAL=$(sed 's/^[^:]*://' "$COOKIE_PATH")

rpc() {
    curl -s --max-time 30 \
        -H "Authorization: Basic $(echo -n "__cookie__:${COOKIE_VAL}" | base64)" \
        -H 'content-type: text/plain;' \
        --data-binary "{\"jsonrpc\":\"1.0\",\"method\":\"$1\",\"params\":[$2]}" \
        "http://127.0.0.1:$RPCPORT/"
}

get_pool() {
    rpc "getprivacystatus" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',{}).get('ct_output_pool_size',0))" 2>/dev/null
}

get_height() {
    rpc "getblockcount" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',0))" 2>/dev/null
}

get_balance() {
    rpc "getbalance" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',{}).get('spendable',0))" 2>/dev/null
}

get_batch_sources() {
    local requested="${1:-1}"
    local payload_file
    payload_file="$(mktemp "${TMPDIR:-/tmp}/dinero-bootstrap-utxos.XXXXXX")"
    rpc "listunspent" "1" > "$payload_file"
    python3 - "$requested" "$payload_file" <<'PY'
import json, sys

requested = int(sys.argv[1])
payload_path = sys.argv[2]
with open(payload_path, "r", encoding="utf-8") as handle:
    payload = json.load(handle)
utxos = payload.get("result", [])

picked = []
for utxo in utxos:
    if not utxo.get("spendable", False):
        continue
    if not utxo.get("safe", True):
        continue
    if not utxo.get("is_mature", True):
        continue
    if float(utxo.get("amount", 0.0)) < 1.01:
        continue
    picked.append(f"{utxo['txid']}:{utxo['vout']}")
    if len(picked) >= requested:
        break

for item in picked:
    print(item)
PY
    local status=$?
    rm -f "$payload_file"
    return $status
}

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Mainnet Pool Bootstrap                                    ║"
echo "║  Target: $TARGET_POOL CT outputs                               ║"
echo "║  Batch: $BATCH_SIZE shields, then wait ${WAIT_BETWEEN_BATCHES}s for mining        ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""

POOL=$(get_pool)
HEIGHT=$(get_height)
BALANCE=$(get_balance)
echo "Initial state: height=$HEIGHT pool=$POOL balance=$BALANCE DIN"

if (( $(echo "$BALANCE < 600" | bc -l) )); then
    echo "ERROR: Insufficient balance ($BALANCE DIN). Need at least 600 DIN."
    exit 1
fi

TOTAL_SHIELDS=0
BATCH_NUM=0

while [[ $(get_pool) -lt $TARGET_POOL ]]; do
    BATCH_NUM=$((BATCH_NUM + 1))
    POOL_NOW=$(get_pool)
    HEIGHT_NOW=$(get_height)
    REMAINING=$((TARGET_POOL - POOL_NOW))
    SHIELDS_NEEDED=$(( (REMAINING + 1) / 2 ))  # Each shield creates ~2 CT outputs

    # Don't overshoot
    THIS_BATCH=$BATCH_SIZE
    if [[ $SHIELDS_NEEDED -lt $THIS_BATCH ]]; then
        THIS_BATCH=$SHIELDS_NEEDED
    fi

    echo ""
    echo "[Batch $BATCH_NUM] height=$HEIGHT_NOW pool=$POOL_NOW remaining=$REMAINING shields=$THIS_BATCH"

    # Pick distinct transparent UTXOs up front so each shield spends a different
    # source coinbase instead of conflicting in mempool.
    SOURCES=()
    while IFS= read -r SOURCE; do
        [[ -n "$SOURCE" ]] && SOURCES+=("$SOURCE")
    done < <(get_batch_sources "$THIS_BATCH")
    if [[ ${#SOURCES[@]} -eq 0 ]]; then
        echo "  No spendable transparent UTXOs available for shielding."
        break
    fi
    if [[ ${#SOURCES[@]} -lt $THIS_BATCH ]]; then
        echo "  Only ${#SOURCES[@]} distinct spendable UTXOs available; trimming batch."
        THIS_BATCH=${#SOURCES[@]}
    fi

    # Send batch of shields
    BATCH_OK=0
    BATCH_FAIL=0
    for SOURCE in "${SOURCES[@]:0:$THIS_BATCH}"; do
        TXID_SRC="${SOURCE%%:*}"
        VOUT_SRC="${SOURCE##*:}"
        RESULT=$(rpc "shieldcoins" "{\"amount\":$SHIELD_AMOUNT,\"source_txid\":\"$TXID_SRC\",\"source_vout\":$VOUT_SRC}")
        TXID=$(echo "$RESULT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',{}).get('txid','FAIL'))" 2>/dev/null)
        STATUS=$(echo "$RESULT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',{}).get('status','FAIL'))" 2>/dev/null)
        if [[ "$TXID" != "FAIL" && -n "$TXID" && "$STATUS" == "broadcast" ]]; then
            BATCH_OK=$((BATCH_OK + 1))
        else
            BATCH_FAIL=$((BATCH_FAIL + 1))
            ERR=$(echo "$RESULT" | python3 -c "import sys,json; root=json.load(sys.stdin); r=root.get('result',{}); print(r.get('error') or r.get('warning') or root.get('error',{}).get('message','unknown'))" 2>/dev/null)
            echo "  ⚠ Shield failed: $ERR"
            # If we get "insufficient funds" or mempool full, stop this batch
            if echo "$ERR" | grep -qi "insufficient\|mempool\|limit"; then
                echo "  Stopping batch early: $ERR"
                break
            fi
        fi
    done
    TOTAL_SHIELDS=$((TOTAL_SHIELDS + BATCH_OK))
    echo "  Sent: $BATCH_OK ok, $BATCH_FAIL failed (total shields: $TOTAL_SHIELDS)"

    # Wait for block(s) to confirm the batch
    echo "  Waiting ${WAIT_BETWEEN_BATCHES}s for mining..."
    POOL_BEFORE=$(get_pool)
    sleep $WAIT_BETWEEN_BATCHES

    POOL_AFTER=$(get_pool)
    GROWTH=$((POOL_AFTER - POOL_BEFORE))
    echo "  Pool: $POOL_BEFORE → $POOL_AFTER (+$GROWTH)"

    # If no growth after waiting, blocks aren't being mined — wait longer
    if [[ $GROWTH -eq 0 && $BATCH_OK -gt 0 ]]; then
        echo "  No growth — waiting extra 120s for mining..."
        sleep 120
        POOL_AFTER=$(get_pool)
        GROWTH=$((POOL_AFTER - POOL_BEFORE))
        echo "  Pool: $POOL_BEFORE → $POOL_AFTER (+$GROWTH)"
    fi
done

POOL_FINAL=$(get_pool)
HEIGHT_FINAL=$(get_height)
STATUS_FINAL=$(rpc "getprivacystatus" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',{}).get('privacy_lane_status','?'))" 2>/dev/null)

echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║  Pool Bootstrap Complete                                   ║"
echo "║  Height: $HEIGHT_FINAL                                          ║"
echo "║  Pool: $POOL_FINAL CT outputs                                  ║"
echo "║  Status: $STATUS_FINAL                                       ║"
echo "║  Total shields: $TOTAL_SHIELDS                                     ║"
echo "╚══════════════════════════════════════════════════════════════╝"

if [[ "$STATUS_FINAL" == "active" ]]; then
    echo ""
    echo "🔒 PRIVATE LANE IS NOW ACTIVE"
    echo "   Ring signatures are mandatory for all CT input spends."
    echo "   sendprivate will produce v3 ring-16 CLSAG transactions."
fi
