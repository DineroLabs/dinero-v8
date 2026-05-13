#!/usr/bin/env bash
# ============================================================================
# V7 Shielded Pool Determinism Test
# ============================================================================
# Proves: two independent nodes processing identical shield/unshield
# operations produce identical shielded balance, note count, and tree size.
#
# Runs nodes SEQUENTIALLY (one at a time) to avoid port conflicts.
# Each node gets a fresh datadir, fresh wallet, identical operations.
#
# Usage: ./test_v7_shielded_determinism.sh [dinerod_path]
# ============================================================================
set -uo pipefail

DINEROD="${1:-/Users/haydarevich/src/dinero/build/dinerod}"
RPCPORT=18590
P2PPORT=18591
FAILURES=0

call() {
    local body
    body=$(python3 -c "
import sys, json
print(json.dumps({'jsonrpc':'2.0','id':1,'method': sys.argv[1],'params': json.loads(sys.argv[2])}))
" "$1" "$2")
    curl -s --max-time 30 -u test:test \
        -H "Content-Type: application/json" \
        -d "$body" \
        "http://127.0.0.1:$RPCPORT/" 2>/dev/null
}

rf() {
    python3 -c "
import sys, json
raw = sys.argv[1] if len(sys.argv) > 1 else ''
if not raw.strip(): print(''); sys.exit(0)
try: doc = json.loads(raw)
except: print(''); sys.exit(0)
cur = doc.get('result')
if len(sys.argv) >= 3 and sys.argv[2]:
    for k in sys.argv[2].split('.'):
        cur = cur.get(k) if isinstance(cur, dict) else None
print(cur if cur is not None else '')
" "$1" "${2:-}"
}

re() {
    python3 -c "
import sys, json
raw = sys.argv[1] if len(sys.argv) > 1 else ''
if not raw.strip(): print('empty_response'); sys.exit(0)
try: doc = json.loads(raw)
except: print('json_error'); sys.exit(0)
res = doc.get('result', {})
if isinstance(res, dict) and res.get('error'): print(res['error']); sys.exit(0)
if doc.get('error'):
    err = doc['error']; print(err.get('message','') if isinstance(err, dict) else str(err)); sys.exit(0)
print('')
" "$1"
}

height() { rf "$(call getblockcount '[]')" ""; }
check_pass() { echo "  [PASS] $1"; }
check_fail() { echo "  [FAIL] $1"; FAILURES=$((FAILURES + 1)); }
check_eq() {
    local d="$1" a="$2" e="$3"
    [[ "$a" == "$e" ]] && check_pass "$d" || check_fail "$d — got '$a', expected '$e'"
}

run_node_ops() {
    local label="$1"
    local tmpdir
    tmpdir=$(mktemp -d /tmp/dinero_det_${label}_XXXXXX)
    local pid=""

    "$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test \
        -rpcport=$RPCPORT -port=$P2PPORT -datadir="$tmpdir" \
        -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
        > "$tmpdir/node.log" 2>&1 &
    pid=$!

    # Wait for readiness
    for i in $(seq 1 60); do
        local h
        h=$(height 2>/dev/null || echo "")
        [[ "$h" =~ ^[0-9]+$ ]] && break
        sleep 1
    done

    # Setup wallet
    call "wallet.createhd" "[\"det\"]" > /dev/null
    call "wallet.encrypt" "[\"pw\"]" > /dev/null
    call "wallet.unlock" "[\"pw\",3600]" > /dev/null

    # Shield 25 DIN
    local r1
    r1=$(call "wallet.shield" '{"amount": 25.0}')

    # Shield 10 DIN
    local r2
    r2=$(call "wallet.shield" '{"amount": 10.0}')

    # Get balance state
    local bal
    bal=$(call "wallet.shieldedbalance" '{}')
    local balance_una note_count tree_size
    balance_una=$(rf "$bal" "balance_una")
    note_count=$(rf "$bal" "note_count")
    tree_size=$(rf "$bal" "tree_size")

    # Unshield leaf 0
    local r3
    r3=$(call "wallet.unshield" '{"leaf_index": 0}')
    local unshield_val
    unshield_val=$(rf "$r3" "value_una")

    # Final state
    local final_bal
    final_bal=$(call "wallet.shieldedbalance" '{}')
    local final_balance final_notes final_tree
    final_balance=$(rf "$final_bal" "balance_una")
    final_notes=$(rf "$final_bal" "note_count")
    final_tree=$(rf "$final_bal" "tree_size")

    # Double-unshield test
    local r4
    r4=$(call "wallet.unshield" '{"leaf_index": 0}')
    local double_err
    double_err=$(re "$r4")

    # Cleanup
    kill "$pid" 2>/dev/null || true
    sleep 1
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -rf "$tmpdir"

    # Return results as a single line
    echo "$balance_una|$note_count|$tree_size|$unshield_val|$final_balance|$final_notes|$final_tree|$double_err"
}

echo ""
echo "==============================================="
echo " V7 Shielded Pool Determinism Test"
echo "==============================================="

echo ""
echo "Running Node A..."
RESULT_A=$(run_node_ops "A")
echo "  Node A: $RESULT_A"

echo ""
echo "Running Node B..."
RESULT_B=$(run_node_ops "B")
echo "  Node B: $RESULT_B"

echo ""
echo "Comparing results..."

IFS='|' read -r A_BAL A_NOTES A_TREE A_UNVAL A_FBAL A_FNOTES A_FTREE A_DBERR <<< "$RESULT_A"
IFS='|' read -r B_BAL B_NOTES B_TREE B_UNVAL B_FBAL B_FNOTES B_FTREE B_DBERR <<< "$RESULT_B"

check_eq "balance_una after 2 shields" "$A_BAL" "$B_BAL"
check_eq "note_count after 2 shields" "$A_NOTES" "$B_NOTES"
check_eq "tree_size after 2 shields" "$A_TREE" "$B_TREE"
check_eq "unshield value_una" "$A_UNVAL" "$B_UNVAL"
check_eq "final balance_una" "$A_FBAL" "$B_FBAL"
check_eq "final note_count" "$A_FNOTES" "$B_FNOTES"
check_eq "final tree_size" "$A_FTREE" "$B_FTREE"
[[ -n "$A_DBERR" ]] && check_pass "Node A rejects double-unshield" || check_fail "Node A accepted double"
[[ -n "$B_DBERR" ]] && check_pass "Node B rejects double-unshield" || check_fail "Node B accepted double"

echo ""
echo "==============================================="
if [[ "$FAILURES" -eq 0 ]]; then
    echo "  ALL CHECKS PASSED — nodes are deterministic"
else
    echo "  $FAILURES CHECK(S) FAILED"
fi
echo "==============================================="
exit "$FAILURES"
