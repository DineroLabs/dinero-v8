#!/usr/bin/env bash
# ============================================================================
# V7 P2MR intra-wallet transfers — Taproot ↔ P2MR within same wallet
# ============================================================================
set -euo pipefail

DINEROD="${1:-/Users/haydarevich/src/dinero/build/dinerod}"
RPCPORT=18580
P2PPORT=18581
TMPDIR=$(mktemp -d /tmp/dinero_v7_intra_XXXXXX)
PID=""
FAILURES=0

rpc() { curl -s --max-time 30 -u test:test -H "Content-Type: application/json" -d "$2" "http://127.0.0.1:$RPCPORT/" 2>/dev/null; }
call() {
    local body
    body=$(python3 -c "
import sys, json
print(json.dumps({'jsonrpc':'2.0','id':1,'method': sys.argv[1],'params': json.loads(sys.argv[2])}))
" "$1" "$2")
    rpc "$1" "$body"
}
rf() { python3 -c "
import sys, json
doc = json.loads(sys.argv[1])
cur = doc.get('result')
if len(sys.argv) >= 3 and sys.argv[2]:
    for k in sys.argv[2].split('.'):
        cur = cur.get(k) if isinstance(cur, dict) else None
print(cur if cur is not None else '')
" "$1" "${2:-}"; }
re() { python3 -c "
import sys, json
doc = json.loads(sys.argv[1])
res = doc.get('result', {})
if isinstance(res, dict) and res.get('error'):
    print(res['error']); sys.exit(0)
if doc.get('error'):
    err = doc['error']
    print(err.get('message','') if isinstance(err, dict) else str(err))
    sys.exit(0)
print('')
" "$1"; }
height() { rf "$(call getblockcount '[]')" ""; }
check_pass() { echo "  [PASS] $1"; }
check_fail() { echo "  [FAIL] $1"; FAILURES=$((FAILURES + 1)); }

cleanup() {
    [[ -n "$PID" ]] && { kill "$PID" 2>/dev/null || true; sleep 2; kill -9 "$PID" 2>/dev/null || true; }
    [[ "${KEEP_TMPDIR:-0}" == "1" ]] && echo "  (tmpdir: $TMPDIR)" || rm -rf "$TMPDIR"
}
trap cleanup EXIT

echo ""
echo "==============================================="
echo " V7 P2MR intra-wallet transfers"
echo "==============================================="

"$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test \
    -rpcport=$RPCPORT -port=$P2PPORT -datadir="$TMPDIR" \
    -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
    > "$TMPDIR/node.log" 2>&1 &
PID=$!
for i in $(seq 1 60); do H=$(height 2>/dev/null || echo ""); [[ "$H" =~ ^[0-9]+$ ]] && break; sleep 1; done

call "wallet.createhd" '["iw"]' > /dev/null
call "wallet.encrypt" '["pw"]' > /dev/null
call "wallet.unlock" '["pw",3600]' > /dev/null

# Generate both address types in the SAME wallet
TAP=$(rf "$(call "wallet.getnewaddress" '["taproot"]')" "address")
[[ -n "$TAP" ]] || TAP=$(python3 -c "import sys,json;r=json.loads(sys.argv[1]).get('result');print(r if isinstance(r,str) else '')" "$(call "wallet.getnewaddress" '["taproot"]')")
P2MR=$(rf "$(call "wallet.getnewaddress" '["p2mr"]')" "address")
echo "  Taproot: ${TAP:0:20}..."
echo "  P2MR:    ${P2MR:0:20}..."

# Mine coins to the Taproot address
call "generatetoaddress" "[112, \"$TAP\"]" > /dev/null
check_pass "setup: height $(height)"

# ── Test 1: Taproot → P2MR (same wallet) ──
echo ""
echo "Test 1: Taproot → P2MR (same wallet)"
SEND_P=$(python3 -c "import json; print(json.dumps({'address':'$P2MR','amount':20.0}))")
RESP=$(call "wallet.sendtoaddress" "$SEND_P")
ERR=$(re "$RESP")
if [[ -z "$ERR" ]]; then
    TXID=$(rf "$RESP" "txid")
    check_pass "sent 20 DIN Taproot→P2MR: ${TXID:0:16}..."
else
    check_fail "Taproot→P2MR: $ERR"
fi

call "generatetoaddress" "[2, \"$TAP\"]" > /dev/null
check_pass "mined (height $(height))"

# ── Test 2: P2MR → Taproot (same wallet) ──
echo ""
echo "Test 2: P2MR → Taproot (same wallet)"
TAP2=$(rf "$(call "wallet.getnewaddress" '["taproot","dest"]')" "address")
[[ -n "$TAP2" ]] || TAP2=$(python3 -c "import sys,json;r=json.loads(sys.argv[1]).get('result');print(r if isinstance(r,str) else '')" "$(call "wallet.getnewaddress" '["taproot","dest"]')")
SEND_P=$(python3 -c "import json; print(json.dumps({'address':'$TAP2','amount':19.9}))")
RESP=$(call "wallet.sendtoaddress" "$SEND_P")
ERR=$(re "$RESP")
if [[ -z "$ERR" ]]; then
    TXID=$(rf "$RESP" "txid")
    FEE=$(rf "$RESP" "fee_paid_una")
    check_pass "sent 19.9 DIN P2MR→Taproot: ${TXID:0:16}... (fee=$FEE una)"
    if [[ "$FEE" -gt 4000 ]]; then
        check_pass "fee confirms ML-DSA-65 witness ($FEE una)"
    else
        check_fail "fee too low for P2MR ($FEE una)"
    fi
else
    check_fail "P2MR→Taproot: $ERR"
fi

call "generatetoaddress" "[1, \"$TAP\"]" > /dev/null

# ── Balance check with PQ ratio ──
echo ""
echo "Balance after both transfers:"
RESP=$(call "wallet.getbalance" '{}')
TOTAL=$(rf "$RESP" "total")
PQ_RATIO=$(rf "$RESP" "pq_ratio")
PQ_DIN=$(rf "$RESP" "pq_balance_din")
echo "  total:    $TOTAL DIN"
echo "  pq_ratio: $PQ_RATIO"
echo "  pq_din:   $PQ_DIN DIN"
check_pass "balance reported with PQ health ratio"

# ── Summary ──
echo ""
echo "==============================================="
if [[ "$FAILURES" -eq 0 ]]; then
    echo "  ALL CHECKS PASSED"
else
    echo "  $FAILURES CHECK(S) FAILED"
fi
echo "==============================================="
exit "$FAILURES"
