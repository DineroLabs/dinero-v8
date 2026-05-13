#!/usr/bin/env bash
# ============================================================================
# V7 P2MR mixed-input transaction — Regtest
# ============================================================================
# Proves a single transaction can spend BOTH a Taproot (P2TR) input and a
# P2MR (ML-DSA-65, witness v3) input, with both sighashes computed via
# the consensus SignatureHashTaproot primitive and validated by the same
# block validator.
#
# This is the invariant that matters: both input types commit to the full
# set of prevout amounts + scriptPubKeys (BIP-341). If the wallet-layer
# sighash for either type diverged from consensus, the mixed tx would
# fail validation.
#
# Flow:
#   A. Setup wallet, mine 110 blocks
#   B. Create a P2MR address, fund it with 15 DIN from a coinbase
#   C. Build a raw tx spending BOTH a Taproot coinbase (15 DIN) + the
#      P2MR UTXO (15 DIN) → 1 output (29.99 DIN to a taproot recipient)
#   D. wallet.signrawtransaction signs both inputs in one pass
#   E. sendrawtransaction → mempool accepts → mine → confirmed
#
# Usage: ./test_v7_p2mr_mixed_inputs.sh [dinerod_path]
# ============================================================================
set -euo pipefail

DINEROD="${1:-/Users/haydarevich/src/dinero/build/dinerod}"
RPCPORT=18560
P2PPORT=18561
TMPDIR=$(mktemp -d /tmp/dinero_v7_mixed_XXXXXX)
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
echo " V7 P2MR mixed-input transaction — Regtest"
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
call "wallet.createhd" '["mix"]' > /dev/null
call "wallet.encrypt" '["pw"]' > /dev/null
call "wallet.unlock" '["pw",3600]' > /dev/null

TAP=$(rf "$(call "wallet.getnewaddress" '["taproot","mine"]')" "address")
[[ -n "$TAP" ]] || TAP=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$(call "wallet.getnewaddress" '["taproot","mine"]')")
call "generatetoaddress" "[110, \"$TAP\"]" > /dev/null
echo "  Setup: height $(height)"

# ── Step B: fund P2MR with 15 DIN ──
echo ""
echo "Step B: fund a P2MR address with 15 DIN"

P2MR_RESP=$(call "wallet.getnewaddress" '["p2mr","mixed-test"]')
P2MR_ADDR=$(rf "$P2MR_RESP" "address")
P2MR_MR=$(rf "$P2MR_RESP" "merkle_root_hex")
P2MR_SPK="5320${P2MR_MR}"
[[ -n "$P2MR_ADDR" ]] || { check_fail "no P2MR address"; exit 1; }
check_pass "P2MR address: ${P2MR_ADDR:0:18}..."

# Get a coinbase to fund the P2MR output.
RESP=$(call "wallet.listunspent" "[1]")
CB1=$(python3 -c "
import sys, json
utxos = json.loads(sys.argv[1]).get('result', [])
if isinstance(utxos, dict): utxos = utxos.get('utxos', []) or []
for u in utxos:
    if u.get('is_mature', True) and u.get('spendable', True):
        print(json.dumps({'txid': u['txid'], 'vout': u['vout'],
                          'amt': u.get('amount_una'),
                          'spk': u.get('scriptPubKey','')}))
        sys.exit(0)
" "$RESP")
CB1_TXID=$(echo "$CB1" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['txid'])")
CB1_VOUT=$(echo "$CB1" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['vout'])")
CB1_AMT=$(echo "$CB1" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['amt'])")
CB1_SPK=$(echo "$CB1" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['spk'])")

FEE=10000
P2MR_SAT=1500000000  # 15 DIN
CHG_SAT=$((CB1_AMT - P2MR_SAT - FEE))
P2MR_DIN=$(python3 -c "print(f'{$P2MR_SAT/1e8:.8f}')")
CHG_DIN=$(python3 -c "print(f'{$CHG_SAT/1e8:.8f}')")

OUTPUTS=$(python3 -c "
import sys, json
print(json.dumps([
    {'scriptPubKey': sys.argv[1], 'amount': float(sys.argv[2])},
    {sys.argv[3]: float(sys.argv[4])},
]))
" "$P2MR_SPK" "$P2MR_DIN" "$TAP" "$CHG_DIN")
INPUTS="[{\"txid\":\"$CB1_TXID\",\"vout\":$CB1_VOUT}]"

RESP=$(call "wallet.createrawtransaction" "[$INPUTS, $OUTPUTS]")
FH=$(rf "$RESP" "hex")
RESP=$(call "wallet.signrawtransaction" "[\"$FH\"]")
SH=$(rf "$RESP" "hex")
RESP=$(call "sendrawtransaction" "[\"$SH\"]")
FUND_TXID=$(xt "$RESP")
[[ -n "$FUND_TXID" ]] || { check_fail "fund tx failed"; exit 1; }
check_pass "funded P2MR: ${FUND_TXID:0:16}... (15 DIN)"

# Mine 2 blocks for wallet worker sync.
call "generatetoaddress" "[2, \"$TAP\"]" > /dev/null
check_pass "height $(height)"

# ── Step C: build mixed-input raw tx ──
echo ""
echo "Step C: build + sign mixed-input tx (1 Taproot + 1 P2MR)"

# Find a SECOND mature taproot coinbase to use as the Taproot input.
RESP=$(call "wallet.listunspent" "[1]")
INPUTS_JSON=$(python3 -c "
import sys, json
utxos = json.loads(sys.argv[1]).get('result', [])
if isinstance(utxos, dict): utxos = utxos.get('utxos', []) or []

taproot_input = None
p2mr_input = None

for u in utxos:
    spk = u.get('scriptPubKey', '') or u.get('script_pubkey', '')
    if not u.get('spendable', False): continue

    # P2MR input: the funded 15-DIN UTXO
    if spk.startswith('5320') and len(spk) == 68 and not p2mr_input:
        p2mr_input = u
        continue

    # Taproot input: any mature coinbase
    if spk.startswith('5120') and u.get('is_mature', False) and not taproot_input:
        taproot_input = u

if taproot_input and p2mr_input:
    inputs = [
        {'txid': taproot_input['txid'], 'vout': taproot_input['vout']},
        {'txid': p2mr_input['txid'], 'vout': p2mr_input['vout']},
    ]
    total = taproot_input.get('amount_una', 0) + p2mr_input.get('amount_una', 0)
    prevouts = [
        {'txid': taproot_input['txid'], 'vout': taproot_input['vout'],
         'scriptPubKey': taproot_input.get('scriptPubKey', taproot_input.get('script_pubkey','')),
         'amount': taproot_input.get('amount_una', 0) / 1e8},
        {'txid': p2mr_input['txid'], 'vout': p2mr_input['vout'],
         'scriptPubKey': p2mr_input.get('scriptPubKey', p2mr_input.get('script_pubkey','')),
         'amount': p2mr_input.get('amount_una', 0) / 1e8},
    ]
    print(json.dumps({
        'inputs': inputs,
        'total_una': total,
        'prevouts': prevouts,
        'tap_amt': taproot_input.get('amount_una', 0),
        'p2mr_amt': p2mr_input.get('amount_una', 0),
    }))
else:
    print(json.dumps({'error': 'Could not find both Taproot and P2MR UTXOs',
                      'tap': taproot_input is not None, 'p2mr': p2mr_input is not None}))
" "$RESP")

HAS_ERROR=$(echo "$INPUTS_JSON" | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d.get('error',''))")
[[ -z "$HAS_ERROR" ]] || { check_fail "input selection: $HAS_ERROR"; exit 1; }

TOTAL_UNA=$(echo "$INPUTS_JSON" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['total_una'])")
TAP_AMT=$(echo "$INPUTS_JSON" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['tap_amt'])")
P2MR_AMT=$(echo "$INPUTS_JSON" | python3 -c "import sys,json; print(json.loads(sys.stdin.read())['p2mr_amt'])")
check_pass "selected: Taproot (${TAP_AMT} una) + P2MR (${P2MR_AMT} una)"

# Output: total - generous fee (P2MR witness is ~5.3 KB)
MIXED_FEE=20000  # generous
OUT_UNA=$((TOTAL_UNA - MIXED_FEE))
OUT_DIN=$(python3 -c "print(f'{$OUT_UNA/1e8:.8f}')")

# Get recipient
RCPT=$(rf "$(call "wallet.getnewaddress" '["taproot","mixed-rcpt"]')" "address")
[[ -n "$RCPT" ]] || RCPT=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$(call "wallet.getnewaddress" '["taproot","mixed-rcpt"]')")

INPUTS_FOR_CREATE=$(echo "$INPUTS_JSON" | python3 -c "import sys,json; print(json.dumps(json.loads(sys.stdin.read())['inputs']))")
OUTPUTS_FOR_CREATE=$(python3 -c "import json; print(json.dumps([{'$RCPT': $OUT_DIN}]))")

RESP=$(call "wallet.createrawtransaction" "[$INPUTS_FOR_CREATE, $OUTPUTS_FOR_CREATE]")
[[ -z "$(re "$RESP")" ]] || { check_fail "createrawtransaction -- $(re "$RESP")"; exit 1; }
RAW_HEX=$(rf "$RESP" "hex")
[[ -n "$RAW_HEX" ]] || { check_fail "empty raw hex"; exit 1; }
check_pass "unsigned mixed tx built (${#RAW_HEX} hex chars)"

# ── Step D: sign with prevouts (so wallet.signrawtransaction sees both inputs) ──
PREVOUTS_FOR_SIGN=$(echo "$INPUTS_JSON" | python3 -c "import sys,json; print(json.dumps(json.loads(sys.stdin.read())['prevouts']))")

SIGN_PARAMS=$(python3 -c "
import sys, json
print(json.dumps([sys.argv[1], json.loads(sys.argv[2])]))
" "$RAW_HEX" "$PREVOUTS_FOR_SIGN")

RESP=$(call "wallet.signrawtransaction" "$SIGN_PARAMS")
SIGN_ERR=$(re "$RESP")
[[ -z "$SIGN_ERR" ]] || { check_fail "signrawtransaction mixed -- $SIGN_ERR"; exit 1; }
SIGNED_HEX=$(rf "$RESP" "hex")
COMPLETE=$(rf "$RESP" "complete")
check_eq "signrawtransaction complete" "$COMPLETE" "True"

# Sanity: signed hex should be much longer (Taproot witness ~64B + P2MR witness ~5.3 KB).
UNSIGNED_LEN=${#RAW_HEX}
SIGNED_LEN=${#SIGNED_HEX}
DELTA=$((SIGNED_LEN - UNSIGNED_LEN))
if [[ "$DELTA" -gt 10000 ]]; then
    check_pass "witness delta: $DELTA hex chars (Taproot + ML-DSA-65)"
else
    check_fail "witness delta only $DELTA hex chars (expected >10000)"
fi

# ── Step E: submit + mine ──
RESP=$(call "sendrawtransaction" "[\"$SIGNED_HEX\"]")
SEND_ERR=$(re "$RESP")
[[ -z "$SEND_ERR" ]] || { check_fail "sendrawtransaction mixed -- $SEND_ERR"; exit 1; }
MIXED_TXID=$(xt "$RESP")
[[ -n "$MIXED_TXID" ]] || { check_fail "no txid"; exit 1; }
check_pass "mempool accepted mixed tx: ${MIXED_TXID:0:16}..."

call "generatetoaddress" "[1, \"$TAP\"]" > /dev/null
H=$(height)
BH=$(rf "$(call "getblockhash" "[$H]")" "")
IN_BLK=$(python3 -c "
import sys, json
tx = json.loads(sys.argv[1]).get('result',{}).get('tx',[])
print('yes' if sys.argv[2] in tx else 'no')
" "$(call "getblock" "[\"$BH\"]")" "$MIXED_TXID")
check_eq "mixed-input tx mined in block $H" "$IN_BLK" "yes"

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
