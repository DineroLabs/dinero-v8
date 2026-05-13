#!/usr/bin/env bash
# ============================================================================
# V7 P2MR stress test — Regtest
# ============================================================================
# Creates 20 P2MR addresses, funds them all, then rapid-fires
# wallet.sendtoaddress to spend each one back to taproot, mining blocks
# between batches. Tests DB contention, sync gate under load, fee
# estimation across many P2MR UTXOs, and wallet worker throughput.
#
# Usage: ./test_v7_p2mr_stress.sh [dinerod_path]
# ============================================================================
set -euo pipefail

DINEROD="${1:-/Users/haydarevich/src/dinero/build/dinerod}"
RPCPORT=18570
P2PPORT=18571
TMPDIR=$(mktemp -d /tmp/dinero_v7_stress_XXXXXX)
PID=""
FAILURES=0
NUM_P2MR=20

rpc() {
    curl -s --max-time 60 -u test:test \
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

cleanup() {
    [[ -n "$PID" ]] && { kill "$PID" 2>/dev/null || true; sleep 2; kill -9 "$PID" 2>/dev/null || true; }
    [[ "${KEEP_TMPDIR:-0}" == "1" ]] && echo "  (tmpdir: $TMPDIR)" || rm -rf "$TMPDIR"
}
trap cleanup EXIT

echo ""
echo "==============================================="
echo " V7 P2MR stress test ($NUM_P2MR addresses)"
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

# ── Setup ──
call "wallet.createhd" '["stress"]' > /dev/null
call "wallet.encrypt" '["pw"]' > /dev/null
call "wallet.unlock" '["pw",3600]' > /dev/null
TAP=$(rf "$(call "wallet.getnewaddress" '["taproot","mine"]')" "address")
[[ -n "$TAP" ]] || TAP=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$(call "wallet.getnewaddress" '["taproot","mine"]')")
call "generatetoaddress" "[120, \"$TAP\"]" > /dev/null
echo "  Setup complete: height $(height)"

# ── Phase 1: generate N P2MR addresses ──
echo ""
echo "Phase 1: generate $NUM_P2MR P2MR addresses"
declare -a P2MR_ADDRS
declare -a P2MR_SPKS
for idx in $(seq 0 $((NUM_P2MR - 1))); do
    RESP=$(call "wallet.getnewaddress" "[\"p2mr\",\"stress-$idx\"]")
    ADDR=$(rf "$RESP" "address")
    MR=$(rf "$RESP" "merkle_root_hex")
    if [[ -z "$ADDR" || -z "$MR" ]]; then
        check_fail "getnewaddress p2mr idx=$idx"
        exit 1
    fi
    P2MR_ADDRS[$idx]="$ADDR"
    P2MR_SPKS[$idx]="5320${MR}"
done
check_pass "generated $NUM_P2MR P2MR addresses"

# ── Phase 2: fund each P2MR address with 5 DIN ──
echo ""
echo "Phase 2: fund $NUM_P2MR P2MR addresses (5 DIN each)"
FUND_AMT=500000000  # 5 DIN
FUND_DIN=$(python3 -c "print(f'{$FUND_AMT/1e8:.8f}')")
FUNDED=0

for idx in $(seq 0 $((NUM_P2MR - 1))); do
    # Get a mature coinbase
    RESP=$(call "wallet.listunspent" "[1]")
    CB=$(python3 -c "
import sys, json
utxos = json.loads(sys.argv[1]).get('result', [])
if isinstance(utxos, dict): utxos = utxos.get('utxos', []) or []
for u in utxos:
    if u.get('is_mature', True) and u.get('spendable', True) and u.get('amount_una', 0) >= 600000000:
        print(json.dumps({'txid': u['txid'], 'vout': u['vout'], 'amt': u.get('amount_una')}))
        sys.exit(0)
print('')
" "$RESP")
    [[ -n "$CB" ]] || { check_fail "no coinbase for P2MR idx=$idx"; break; }

    CB_TXID=$(echo "$CB" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['txid'])")
    CB_VOUT=$(echo "$CB" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['vout'])")
    CB_AMT=$(echo "$CB" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['amt'])")

    FEE=10000
    CHG=$((CB_AMT - FUND_AMT - FEE))
    CHG_DIN=$(python3 -c "print(f'{$CHG/1e8:.8f}')")

    OUTPUTS=$(python3 -c "
import sys, json
print(json.dumps([
    {'scriptPubKey': sys.argv[1], 'amount': float(sys.argv[2])},
    {sys.argv[3]: float(sys.argv[4])},
]))
" "${P2MR_SPKS[$idx]}" "$FUND_DIN" "$TAP" "$CHG_DIN")
    INPUTS="[{\"txid\":\"$CB_TXID\",\"vout\":$CB_VOUT}]"

    RESP=$(call "wallet.createrawtransaction" "[$INPUTS, $OUTPUTS]")
    FH=$(rf "$RESP" "hex")
    RESP=$(call "wallet.signrawtransaction" "[\"$FH\"]")
    SH=$(rf "$RESP" "hex")
    RESP=$(call "sendrawtransaction" "[\"$SH\"]")
    [[ -z "$(re "$RESP")" ]] || { check_fail "fund P2MR idx=$idx -- $(re "$RESP")"; break; }

    FUNDED=$((FUNDED + 1))

    # Mine every 5 funding txs to keep mempool size reasonable.
    if (( (idx + 1) % 5 == 0 )); then
        call "generatetoaddress" "[1, \"$TAP\"]" > /dev/null
    fi
done

# Mine remaining + extra for wallet worker.
call "generatetoaddress" "[3, \"$TAP\"]" > /dev/null
check_pass "funded $FUNDED/$NUM_P2MR P2MR addresses (height $(height))"

# ── Phase 3: spend each P2MR UTXO via wallet.sendtoaddress ──
echo ""
echo "Phase 3: spend $FUNDED P2MR UTXOs via wallet.sendtoaddress"
SPENT=0
TOTAL_FEE=0

for idx in $(seq 0 $((FUNDED - 1))); do
    RCPT=$(rf "$(call "wallet.getnewaddress" '["taproot","drain"]')" "address")
    [[ -n "$RCPT" ]] || RCPT=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$(call "wallet.getnewaddress" '["taproot","drain"]')")

    SEND_PARAMS=$(python3 -c "import json; print(json.dumps({'address':'$RCPT','amount':4.9}))")
    RESP=$(call "wallet.sendtoaddress" "$SEND_PARAMS")
    ERR=$(re "$RESP")
    if [[ -n "$ERR" ]]; then
        check_fail "spend P2MR idx=$idx -- $ERR"
        continue
    fi

    FEE=$(rf "$RESP" "fee_paid_una")
    TOTAL_FEE=$((TOTAL_FEE + FEE))
    SPENT=$((SPENT + 1))

    # Mine every 5 spends.
    if (( (idx + 1) % 5 == 0 )); then
        call "generatetoaddress" "[1, \"$TAP\"]" > /dev/null
    fi
done

call "generatetoaddress" "[1, \"$TAP\"]" > /dev/null

if [[ "$SPENT" -eq "$FUNDED" ]]; then
    check_pass "spent all $SPENT/$FUNDED P2MR UTXOs"
else
    check_fail "only spent $SPENT/$FUNDED P2MR UTXOs"
fi

AVG_FEE=$((TOTAL_FEE / (SPENT > 0 ? SPENT : 1)))
echo "  total fee: $TOTAL_FEE una, avg: $AVG_FEE una/tx"

if [[ "$AVG_FEE" -gt 4000 ]]; then
    check_pass "avg fee $AVG_FEE una (consistent with ML-DSA-65 witness)"
else
    check_fail "avg fee $AVG_FEE una too low for P2MR"
fi

echo "  final height: $(height)"

# ── Summary ──
echo ""
echo "==============================================="
if [[ "$FAILURES" -eq 0 ]]; then
    echo "  ALL CHECKS PASSED ($SPENT P2MR spends, $NUM_P2MR addresses)"
else
    echo "  $FAILURES CHECK(S) FAILED"
fi
echo "==============================================="
exit "$FAILURES"
