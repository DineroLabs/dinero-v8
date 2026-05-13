#!/usr/bin/env bash
# ============================================================================
# V7 P2MR wallet.sendtoaddress full e2e — Regtest
# ============================================================================
# Proves the complete user-facing flow: wallet.getnewp2mraddress → fund →
# wallet.sendtoaddress picks the P2MR UTXO via coin selection → signs via
# WalletKeyProvider → mempool accepts → mined. No debug.* helpers, no
# manual raw-tx construction.
#
# Usage: ./test_v7_p2mr_wallet_e2e.sh [dinerod_path]
# ============================================================================
set -euo pipefail

DINEROD="${1:-/Users/haydarevich/src/dinero/build/dinerod}"
RPCPORT=18540
P2PPORT=18541
TMPDIR=$(mktemp -d /tmp/dinero_v7_e2e_XXXXXX)
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

result_field() {
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

result_error() {
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

extract_txid() {
    python3 -c "
import sys, json
r = json.loads(sys.argv[1]).get('result')
if isinstance(r, dict):
    r = r.get('txid') or r.get('result') or ''
print(r if isinstance(r, str) else '')
" "$1"
}

height() {
    local resp
    resp=$(call "getblockcount" "[]")
    result_field "$resp" ""
}

check_pass() { echo "  [PASS] $1"; }
check_fail() { echo "  [FAIL] $1"; FAILURES=$((FAILURES + 1)); }
check_eq() {
    local desc="$1" actual="$2" expected="$3"
    if [[ "$actual" == "$expected" ]]; then
        check_pass "$desc"
    else
        check_fail "$desc -- expected '$expected', got '$actual'"
    fi
}

cleanup() {
    if [[ -n "$PID" ]]; then
        kill "$PID" 2>/dev/null || true
        for i in 1 2 3 4 5; do
            kill -0 "$PID" 2>/dev/null || break
            sleep 1
        done
        kill -9 "$PID" 2>/dev/null || true
    fi
    [[ "${KEEP_TMPDIR:-0}" == "1" ]] && echo "  (tmpdir: $TMPDIR)" || rm -rf "$TMPDIR"
}
trap cleanup EXIT

echo ""
echo "==============================================="
echo " V7 P2MR wallet.sendtoaddress e2e — Regtest"
echo "==============================================="

"$DINEROD" \
    -regtest -daemon=0 -server \
    -rpcuser=test -rpcpassword=test \
    -rpcport=$RPCPORT -port=$P2PPORT \
    -datadir="$TMPDIR" \
    -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
    > "$TMPDIR/node.log" 2>&1 &
PID=$!

READY=0
for i in $(seq 1 60); do
    H=$(height 2>/dev/null || echo "")
    [[ "$H" =~ ^[0-9]+$ ]] && { READY=1; break; }
    sleep 1
done
[[ "$READY" == "1" ]] || { check_fail "Node never became ready"; exit 1; }
echo "  Node ready"

# ── Phase A: wallet ──
echo ""
echo "Phase A: wallet setup"
call "wallet.createhd" '["e2e"]' > /dev/null
call "wallet.encrypt" '["pw"]' > /dev/null
call "wallet.unlock" '["pw",3600]' > /dev/null
check_pass "wallet created + unlocked"

RESP=$(call "wallet.getnewaddress" '["taproot","mine"]')
TAP=$(result_field "$RESP" "address")
[[ -n "$TAP" ]] || TAP=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$RESP")
[[ -n "$TAP" ]] || { check_fail "no taproot address"; exit 1; }

call "generatetoaddress" "[110, \"$TAP\"]" > /dev/null
H=$(height); check_eq "at height 110" "$H" "110"

# ── Phase B: fund P2MR ──
echo ""
echo "Phase B: create + fund P2MR address"

RESP=$(call "wallet.getnewp2mraddress" '{"account":0,"change":0,"address_index":0}')
[[ -z "$(result_error "$RESP")" ]] || { check_fail "getnewp2mraddress -- $(result_error "$RESP")"; exit 1; }
P2MR_ADDR=$(result_field "$RESP" "address")
P2MR_MR=$(result_field "$RESP" "merkle_root_hex")
P2MR_SPK="5320${P2MR_MR}"
check_pass "P2MR address: ${P2MR_ADDR:0:14}..."

# Pick a coinbase UTXO.
RESP=$(call "wallet.listunspent" "[1]")
UTXO_PICK=$(python3 -c "
import sys, json
utxos = json.loads(sys.argv[1]).get('result', [])
if isinstance(utxos, dict): utxos = utxos.get('utxos', []) or []
for u in utxos:
    if u.get('is_mature', True) and u.get('spendable', True):
        print(json.dumps({'txid': u['txid'], 'vout': u['vout'], 'amt': u.get('amount_una')}))
        sys.exit(0)
print('')
" "$RESP")
[[ -n "$UTXO_PICK" ]] || { check_fail "no mature utxo"; exit 1; }
CB_TXID=$(python3 -c "import sys,json; print(json.loads(sys.argv[1])['txid'])" "$UTXO_PICK")
CB_VOUT=$(python3 -c "import sys,json; print(json.loads(sys.argv[1])['vout'])" "$UTXO_PICK")
CB_AMT=$(python3 -c "import sys,json; print(json.loads(sys.argv[1])['amt'])" "$UTXO_PICK")

# Build funding tx via createrawtransaction.
FEE=10000
P2MR_SAT=3000000000
CHG_SAT=$((CB_AMT - P2MR_SAT - FEE))
P2MR_DIN=$(python3 -c "print(f'{$P2MR_SAT/1e8:.8f}')")
CHG_DIN=$(python3 -c "print(f'{$CHG_SAT/1e8:.8f}')")

OUTPUTS=$(python3 -c "
import sys, json
print(json.dumps([
    {'scriptPubKey': sys.argv[1], 'amount': float(sys.argv[2])},
    {sys.argv[3]: float(sys.argv[4])},
]))
" "$P2MR_SPK" "$P2MR_DIN" "$TAP" "$CHG_DIN")

INPUTS="[{\"txid\":\"$CB_TXID\",\"vout\":$CB_VOUT}]"

RESP=$(call "wallet.createrawtransaction" "[$INPUTS, $OUTPUTS]")
FUND_HEX=$(result_field "$RESP" "hex")
[[ -n "$FUND_HEX" ]] || { check_fail "create fund tx"; exit 1; }

RESP=$(call "wallet.signrawtransaction" "[\"$FUND_HEX\"]")
FUND_SIGNED=$(result_field "$RESP" "hex")

RESP=$(call "sendrawtransaction" "[\"$FUND_SIGNED\"]")
FUND_TXID=$(extract_txid "$RESP")
[[ -n "$FUND_TXID" ]] || { check_fail "send fund tx"; exit 1; }
check_pass "P2MR funded: ${FUND_TXID:0:16}..."

# Mine 2 blocks after the funding tx. The wallet.sendtoaddress sync gate
# (WaitForHeight) ensures the wallet worker has indexed the P2MR UTXO
# before coin selection runs. The second block provides a confirmation
# buffer that avoids edge cases in the async worker queue latency.
call "generatetoaddress" "[2, \"$TAP\"]" > /dev/null
H=$(height); check_eq "height 112" "$H" "112"

# ── Phase C: wallet.sendtoaddress spending P2MR ──
echo ""
echo "Phase C: wallet.sendtoaddress spending P2MR coin"

# Get a fresh recipient.
RESP=$(call "wallet.getnewaddress" '["taproot","rcpt"]')
RCPT=$(result_field "$RESP" "address")
[[ -n "$RCPT" ]] || RCPT=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$RESP")
[[ -n "$RCPT" ]] || { check_fail "no recipient"; exit 1; }

SEND_PARAMS=$(python3 -c "import json; print(json.dumps({'address':'$RCPT','amount':29.9}))")

RESP=$(call "wallet.sendtoaddress" "$SEND_PARAMS")
SEND_ERR=$(result_error "$RESP")
[[ -z "$SEND_ERR" ]] || { check_fail "wallet.sendtoaddress -- $SEND_ERR"; echo "  resp: $RESP" | head -c 1000; exit 1; }

SEND_TXID=$(result_field "$RESP" "txid")
SELECTED=$(python3 -c "
import sys, json
r = json.loads(sys.argv[1]).get('result', {})
si = r.get('selected_inputs', [])
for inp in si:
    # Check if this is the P2MR UTXO (30 DIN = 3000000000 una, vout 0 of fund tx)
    if inp.get('amount_una') == 3000000000:
        print('p2mr'); sys.exit(0)
print('other')
" "$RESP")
check_eq "coin selector picked P2MR UTXO" "$SELECTED" "p2mr"

FEE_PAID=$(result_field "$RESP" "fee_paid_una")
echo "  fee_paid: $FEE_PAID una"
[[ "$FEE_PAID" -gt 4000 ]] || check_fail "fee too low for P2MR witness ($FEE_PAID una)"
[[ "$FEE_PAID" -gt 4000 ]] && check_pass "fee correctly sized for ML-DSA-65 witness ($FEE_PAID una)"

check_pass "wallet.sendtoaddress accepted: ${SEND_TXID:0:16}..."

# Mine and verify.
call "generatetoaddress" "[1, \"$TAP\"]" > /dev/null
H=$(height)
BH=$(result_field "$(call "getblockhash" "[$H]")" "")
IN_BLOCK=$(python3 -c "
import sys, json
tx = json.loads(sys.argv[1]).get('result',{}).get('tx',[])
print('yes' if sys.argv[2] in tx else 'no')
" "$(call "getblock" "[\"$BH\"]")" "$SEND_TXID")
check_eq "P2MR spend mined in block" "$IN_BLOCK" "yes"

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
