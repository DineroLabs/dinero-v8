#!/usr/bin/env bash
# ============================================================================
# V7 P2MR unified wallet test — getnewaddress, send-to-P2MR, mixed inputs
# ============================================================================
# Covers:
#   A. wallet.getnewaddress ["p2mr"] — UX unification (auto-increment)
#   B. wallet.sendtoaddress to a P2MR destination (receive side)
#   C. Mixed-input transaction: 1 Taproot + 1 P2MR input in same tx
#
# Usage: ./test_v7_p2mr_unified.sh [dinerod_path]
# ============================================================================
set -euo pipefail

DINEROD="${1:-/Users/haydarevich/src/dinero/build/dinerod}"
RPCPORT=18550
P2PPORT=18551
TMPDIR=$(mktemp -d /tmp/dinero_v7_unified_XXXXXX)
PID=""
FAILURES=0

rpc() {
    curl -s --max-time 30 -u test:test \
        -H "Content-Type: application/json" \
        -d "$2" \
        "http://127.0.0.1:$RPCPORT/" 2>/dev/null
}

call() {
    local method="$1" params_json="$2"
    local body
    body=$(python3 -c "
import sys, json
print(json.dumps({'jsonrpc':'2.0','id':1,'method': sys.argv[1],'params': json.loads(sys.argv[2])}))
" "$method" "$params_json")
    rpc "$method" "$body"
}

rf() {
    python3 -c "
import sys, json
doc = json.loads(sys.argv[1])
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
doc = json.loads(sys.argv[1])
res = doc.get('result', {})
if isinstance(res, dict) and res.get('error'):
    print(res['error']); sys.exit(0)
if doc.get('error'):
    err = doc['error']
    print(err.get('message','') if isinstance(err, dict) else str(err))
    sys.exit(0)
print('')
" "$1"
}

xt() {
    python3 -c "
import sys, json
r = json.loads(sys.argv[1]).get('result')
if isinstance(r, dict): r = r.get('txid') or r.get('result') or ''
print(r if isinstance(r, str) else '')
" "$1"
}

height() { rf "$(call "getblockcount" "[]")" ""; }
check_pass() { echo "  [PASS] $1"; }
check_fail() { echo "  [FAIL] $1"; FAILURES=$((FAILURES + 1)); }
check_eq() {
    local d="$1" a="$2" e="$3"
    [[ "$a" == "$e" ]] && check_pass "$d" || check_fail "$d -- expected '$e', got '$a'"
}

cleanup() {
    [[ -n "$PID" ]] && { kill "$PID" 2>/dev/null || true; sleep 2; kill -9 "$PID" 2>/dev/null || true; }
    [[ "${KEEP_TMPDIR:-0}" == "1" ]] && echo "  (tmpdir: $TMPDIR)" || rm -rf "$TMPDIR"
}
trap cleanup EXIT

echo ""
echo "==============================================="
echo " V7 P2MR unified wallet test"
echo "==============================================="

"$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test \
    -rpcport=$RPCPORT -port=$P2PPORT -datadir="$TMPDIR" \
    -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
    > "$TMPDIR/node.log" 2>&1 &
PID=$!
for i in $(seq 1 60); do
    H=$(height 2>/dev/null || echo "")
    [[ "$H" =~ ^[0-9]+$ ]] && break
    sleep 1
done

call "wallet.createhd" '["uni"]' > /dev/null
call "wallet.encrypt" '["pw"]' > /dev/null
call "wallet.unlock" '["pw",3600]' > /dev/null

TAP=$(rf "$(call "wallet.getnewaddress" '["taproot","mine"]')" "address")
[[ -n "$TAP" ]] || TAP=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$(call "wallet.getnewaddress" '["taproot","mine"]')")
call "generatetoaddress" "[110, \"$TAP\"]" > /dev/null
check_pass "setup: height $(height), taproot=$TAP"

# ── Test A: wallet.getnewaddress ["p2mr"] ──
echo ""
echo "Test A: wallet.getnewaddress [\"p2mr\"]"

RESP=$(call "wallet.getnewaddress" '["p2mr"]')
ERR=$(re "$RESP")
[[ -z "$ERR" ]] || { check_fail "getnewaddress p2mr -- $ERR"; exit 1; }
P2MR_ADDR1=$(rf "$RESP" "address")
[[ -n "$P2MR_ADDR1" ]] || { check_fail "empty P2MR address"; exit 1; }
check_pass "first P2MR: ${P2MR_ADDR1:0:18}..."

# Auto-increment: second call should produce a DIFFERENT address.
RESP=$(call "wallet.getnewaddress" '["p2mr","second"]')
P2MR_ADDR2=$(rf "$RESP" "address")
[[ -n "$P2MR_ADDR2" ]] || { check_fail "second P2MR address empty"; exit 1; }
if [[ "$P2MR_ADDR1" != "$P2MR_ADDR2" ]]; then
    check_pass "auto-increment: second P2MR is different"
else
    check_fail "auto-increment: second P2MR is same as first"
fi

# ── Test B: send TO a P2MR destination ──
echo ""
echo "Test B: wallet.sendtoaddress to P2MR destination"

SEND_PARAMS=$(python3 -c "import json; print(json.dumps({'address':'$P2MR_ADDR1','amount':10.0}))")
RESP=$(call "wallet.sendtoaddress" "$SEND_PARAMS")
ERR=$(re "$RESP")
[[ -z "$ERR" ]] || { check_fail "sendtoaddress TO P2MR -- $ERR"; exit 1; }
FUND_TXID=$(rf "$RESP" "txid")
check_pass "sent 10 DIN to P2MR: ${FUND_TXID:0:16}..."

call "generatetoaddress" "[2, \"$TAP\"]" > /dev/null
check_pass "mined 2 blocks (height $(height))"

# ── Test C: spend FROM P2MR (coin selection, same as e2e test) ──
echo ""
echo "Test C: wallet.sendtoaddress spending FROM P2MR"

RCPT=$(rf "$(call "wallet.getnewaddress" '["taproot","rcpt"]')" "address")
[[ -n "$RCPT" ]] || RCPT=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$(call "wallet.getnewaddress" '["taproot","rcpt"]')")

SEND_PARAMS=$(python3 -c "import json; print(json.dumps({'address':'$RCPT','amount':9.9}))")
RESP=$(call "wallet.sendtoaddress" "$SEND_PARAMS")
ERR=$(re "$RESP")
[[ -z "$ERR" ]] || { check_fail "sendtoaddress FROM P2MR -- $ERR"; exit 1; }
SPEND_TXID=$(rf "$RESP" "txid")
FEE=$(rf "$RESP" "fee_paid_una")
check_pass "P2MR spend: ${SPEND_TXID:0:16}... (fee=$FEE una)"

call "generatetoaddress" "[1, \"$TAP\"]" > /dev/null
BH=$(rf "$(call "getblockhash" "[$(height)]")" "")
IN_BLK=$(python3 -c "
import sys,json
tx=json.loads(sys.argv[1]).get('result',{}).get('tx',[])
print('yes' if sys.argv[2] in tx else 'no')
" "$(call "getblock" "[\"$BH\"]")" "$SPEND_TXID")
check_eq "P2MR spend mined" "$IN_BLK" "yes"

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
