#!/usr/bin/env bash
# ============================================================================
# V7 P2MR wallet-side signing — Regtest
# ============================================================================
# Phase 10 Commit 2 — proves a v7 P2MR UTXO can be spent through the regular
# wallet signing RPC (wallet.signrawtransaction), with the wallet producing
# the canonical ML-DSA-65 witness blob internally. No debug.* helpers.
#
# What this specifically exercises:
#   TransactionSigner::Sign (via WalletKeyProvider) dispatches P2MR inputs
#   through the hybrid provider's SignP2MR method — computing the BIP-341
#   sighash over the full input set, decrypting the stored seed with the
#   wallet master key, re-deriving the ML-DSA keypair, signing, and
#   serializing a canonical P2MRWitness blob ready for sendrawtransaction.
#
# Coin selection for P2MR UTXOs is a separate wallet-tracking concern
# (wallet.listunspent does not yet surface P2MR outputs). This test
# therefore builds the raw spend transaction explicitly via
# wallet.createrawtransaction, then asks wallet.signrawtransaction to
# sign it — proving the signing path works end-to-end regardless of
# coin-selection plumbing.
#
# Usage: ./test_v7_p2mr_sendtoaddress.sh [dinerod_path]
# ============================================================================
set -euo pipefail

DINEROD="${1:-/Users/haydarevich/src/dinero/build/dinerod}"
RPCPORT=18494
P2PPORT=18495
TMPDIR=$(mktemp -d /tmp/dinero_v7_p2mr_sign_XXXXXX)
PID=""
FAILURES=0

WALLET_NAME="v7sign"
WALLET_PASSWORD="v7-p2mr-sign-regtest"

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
    if [[ "${KEEP_TMPDIR:-0}" != "1" ]]; then
        rm -rf "$TMPDIR"
    else
        echo "  (tmpdir preserved at $TMPDIR)"
    fi
}
trap cleanup EXIT

echo ""
echo "==============================================="
echo " V7 P2MR wallet-side signing — Regtest"
echo "==============================================="
echo "  dinerod:      $DINEROD"
echo "  datadir:      $TMPDIR"
echo ""

# ---------------------------------------------------------------------------
# Phase 0 — start the node
# ---------------------------------------------------------------------------
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
    if [[ "$H" =~ ^[0-9]+$ ]]; then
        echo "  Node ready (${i}s, height=$H)"
        READY=1
        break
    fi
    sleep 1
done
[[ "$READY" == "1" ]] || { check_fail "Node never became ready"; tail -40 "$TMPDIR/node.log" | sed 's/^/       /'; exit 1; }

# ---------------------------------------------------------------------------
# Phase A — wallet setup
# ---------------------------------------------------------------------------
echo ""
echo "Phase A: wallet setup (create → encrypt → unlock)"

RESP=$(call "wallet.createhd" "[\"$WALLET_NAME\"]")
[[ -z "$(result_error "$RESP")" ]] || { check_fail "wallet.createhd -- $(result_error "$RESP")"; exit 1; }
check_pass "wallet.createhd"

RESP=$(call "wallet.encrypt" "[\"$WALLET_PASSWORD\"]")
[[ -z "$(result_error "$RESP")" ]] || { check_fail "wallet.encrypt -- $(result_error "$RESP")"; exit 1; }
check_pass "wallet.encrypt"

RESP=$(call "wallet.unlock" "[\"$WALLET_PASSWORD\", 3600]")
[[ -z "$(result_error "$RESP")" ]] || { check_fail "wallet.unlock -- $(result_error "$RESP")"; exit 1; }
check_pass "wallet.unlock"

RESP=$(call "wallet.getnewaddress" '["taproot","mine"]')
TAPROOT_ADDR=$(result_field "$RESP" "address")
[[ -n "$TAPROOT_ADDR" ]] || TAPROOT_ADDR=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$RESP")
[[ -n "$TAPROOT_ADDR" ]] || { check_fail "could not obtain a taproot address"; exit 1; }
check_pass "taproot mining address: ${TAPROOT_ADDR:0:18}..."

RESP=$(call "generatetoaddress" "[110, \"$TAPROOT_ADDR\"]")
[[ -z "$(result_error "$RESP")" ]] || { check_fail "generatetoaddress -- $(result_error "$RESP")"; exit 1; }
H=$(height); check_eq "at height 110" "$H" "110"

RESP=$(call "wallet.listunspent" "[1]")
UTXO_PICK=$(python3 -c "
import sys, json
utxos = json.loads(sys.argv[1]).get('result', [])
if isinstance(utxos, dict):
    utxos = utxos.get('utxos', []) or utxos.get('addresses', []) or []
for u in utxos:
    if u.get('is_mature', True) and u.get('spendable', True):
        print(json.dumps({'txid': u['txid'], 'vout': u['vout'],
                          'amount_una': u.get('amount_una'),
                          'scriptPubKey': u.get('scriptPubKey', '')}))
        sys.exit(0)
print('')
" "$RESP")
[[ -n "$UTXO_PICK" ]] || { check_fail "no mature spendable utxo found"; exit 1; }
COINBASE_TXID=$(python3 -c "import sys,json; print(json.loads(sys.argv[1])['txid'])" "$UTXO_PICK")
COINBASE_VOUT=$(python3 -c "import sys,json; print(json.loads(sys.argv[1])['vout'])" "$UTXO_PICK")
COINBASE_AMOUNT_SAT=$(python3 -c "import sys,json; print(json.loads(sys.argv[1])['amount_una'])" "$UTXO_PICK")
check_pass "mature coinbase: ${COINBASE_TXID:0:16}...:$COINBASE_VOUT (${COINBASE_AMOUNT_SAT} una)"

# ---------------------------------------------------------------------------
# Phase B — create + fund a P2MR address
# ---------------------------------------------------------------------------
echo ""
echo "Phase B: create + fund a P2MR address"

RESP=$(call "wallet.getnewp2mraddress" '{"account":0,"change":0,"address_index":0,"leaf_index":0,"label":"sign-test"}')
[[ -z "$(result_error "$RESP")" ]] || { check_fail "wallet.getnewp2mraddress -- $(result_error "$RESP")"; exit 1; }
P2MR_ADDR=$(result_field "$RESP" "address")
P2MR_MERKLE_HEX=$(result_field "$RESP" "merkle_root_hex")
[[ -n "$P2MR_ADDR" ]] || { check_fail "wallet.getnewp2mraddress returned no address"; exit 1; }
check_pass "P2MR address: ${P2MR_ADDR:0:14}..."
check_eq "merkle_root_hex is 64 chars" "${#P2MR_MERKLE_HEX}" "64"

P2MR_SPK_HEX="5320${P2MR_MERKLE_HEX}"

FEE_SAT=10000
P2MR_VALUE_SAT=3000000000
CHANGE_VALUE_SAT=$((COINBASE_AMOUNT_SAT - P2MR_VALUE_SAT - FEE_SAT))
[[ "$CHANGE_VALUE_SAT" -gt 0 ]] || { check_fail "coinbase too small"; exit 1; }

P2MR_VALUE_DIN=$(python3 -c "print(f'{$P2MR_VALUE_SAT / 1e8:.8f}')")
CHANGE_VALUE_DIN=$(python3 -c "print(f'{$CHANGE_VALUE_SAT / 1e8:.8f}')")

OUTPUTS_JSON=$(python3 -c "
import sys, json
print(json.dumps([
    {'scriptPubKey': sys.argv[1], 'amount': float(sys.argv[2])},
    {sys.argv[3]: float(sys.argv[4])},
]))
" "$P2MR_SPK_HEX" "$P2MR_VALUE_DIN" "$TAPROOT_ADDR" "$CHANGE_VALUE_DIN")

INPUTS_JSON="[{\"txid\":\"$COINBASE_TXID\",\"vout\":$COINBASE_VOUT}]"

RESP=$(call "wallet.createrawtransaction" "[$INPUTS_JSON, $OUTPUTS_JSON]")
[[ -z "$(result_error "$RESP")" ]] || { check_fail "createrawtransaction -- $(result_error "$RESP")"; exit 1; }
FUND_TX_HEX=$(result_field "$RESP" "hex")

RESP=$(call "wallet.signrawtransaction" "[\"$FUND_TX_HEX\"]")
[[ -z "$(result_error "$RESP")" ]] || { check_fail "signrawtransaction -- $(result_error "$RESP")"; exit 1; }
FUND_SIGNED_HEX=$(result_field "$RESP" "hex")

RESP=$(call "sendrawtransaction" "[\"$FUND_SIGNED_HEX\"]")
[[ -z "$(result_error "$RESP")" ]] || { check_fail "sendrawtransaction (fund P2MR) -- $(result_error "$RESP")"; exit 1; }
FUND_TX_ID=$(extract_txid "$RESP")
[[ -n "$FUND_TX_ID" ]] || { check_fail "funding txid missing"; exit 1; }
check_pass "P2MR funded: ${FUND_TX_ID:0:16}..."

RESP=$(call "generatetoaddress" "[1, \"$TAPROOT_ADDR\"]")
H=$(height); check_eq "height after P2MR funding" "$H" "111"

# ---------------------------------------------------------------------------
# Phase C — spend the P2MR UTXO via wallet.signrawtransaction
# ---------------------------------------------------------------------------
echo ""
echo "Phase C: spend the P2MR UTXO via wallet.signrawtransaction"

# Build the unsigned spend tx by hand (coin selection doesn't surface P2MR
# UTXOs today — separate wallet-tracking concern). Input = the funding tx's
# P2MR output, output = a fresh taproot recipient.
RESP=$(call "wallet.getnewaddress" '["taproot","recipient"]')
RECIPIENT=$(result_field "$RESP" "address")
[[ -n "$RECIPIENT" ]] || RECIPIENT=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$RESP")
[[ -n "$RECIPIENT" ]] || { check_fail "no recipient address"; exit 1; }

SPEND_VALUE_SAT=$((P2MR_VALUE_SAT - FEE_SAT))
SPEND_VALUE_DIN=$(python3 -c "print(f'{$SPEND_VALUE_SAT / 1e8:.8f}')")
INPUTS_C="[{\"txid\":\"$FUND_TX_ID\",\"vout\":0}]"
OUTPUTS_C="[{\"$RECIPIENT\": $SPEND_VALUE_DIN}]"

RESP=$(call "wallet.createrawtransaction" "[$INPUTS_C, $OUTPUTS_C]")
[[ -z "$(result_error "$RESP")" ]] || { check_fail "createrawtransaction (P2MR spend) -- $(result_error "$RESP")"; exit 1; }
SPEND_TX_HEX=$(result_field "$RESP" "hex")
[[ -n "$SPEND_TX_HEX" ]] || { check_fail "spend-tx empty"; exit 1; }
check_pass "unsigned P2MR-spending tx built (${#SPEND_TX_HEX} hex chars)"

# The key assertion: wallet.signrawtransaction recognizes the P2MR input
# shape (0x53 0x20 || merkle_root), resolves the stored seed via
# V7P2MRStore::GetByMerkleRoot, signs via ML-DSA-65, and stamps a canonical
# witness. We supply the prevout explicitly (amount + scriptPubKey) because
# the funding tx's P2MR output isn't in wallet.listunspent (see note above).
P2MR_AMOUNT_DIN=$(python3 -c "print(f'{$P2MR_VALUE_SAT / 1e8:.8f}')")
PREVTXS=$(python3 -c "
import sys, json
print(json.dumps([{
    'txid': sys.argv[1],
    'vout': 0,
    'scriptPubKey': sys.argv[2],
    'amount': float(sys.argv[3]),
}]))
" "$FUND_TX_ID" "$P2MR_SPK_HEX" "$P2MR_AMOUNT_DIN")

SIGN_PARAMS=$(python3 -c "
import sys, json
print(json.dumps([sys.argv[1], json.loads(sys.argv[2])]))
" "$SPEND_TX_HEX" "$PREVTXS")

RESP=$(call "wallet.signrawtransaction" "$SIGN_PARAMS")
SIGN_ERR=$(result_error "$RESP")
[[ -z "$SIGN_ERR" ]] || { check_fail "wallet.signrawtransaction (P2MR spend) -- $SIGN_ERR"; exit 1; }
SIGNED_HEX=$(result_field "$RESP" "hex")
COMPLETE=$(result_field "$RESP" "complete")
[[ -n "$SIGNED_HEX" ]] || { check_fail "signed hex empty"; exit 1; }
check_eq "wallet.signrawtransaction reports complete=true" "$COMPLETE" "True"

# Sanity: signed hex should be materially longer than unsigned — the
# P2MR witness blob alone adds ~5.3 KB (pubkey 1952 + sig 3309 + framing).
UNSIGNED_LEN=${#SPEND_TX_HEX}
SIGNED_LEN=${#SIGNED_HEX}
DELTA=$((SIGNED_LEN - UNSIGNED_LEN))
if [[ "$DELTA" -lt 10000 ]]; then
    check_fail "signed-tx hex grew by only $DELTA hex chars (expected >10000 for ML-DSA-65 witness)"
else
    check_pass "signed-tx hex grew by $DELTA hex chars (consistent with ML-DSA-65 witness)"
fi

# Submit to mempool → must accept → mine a block and confirm inclusion.
RESP=$(call "sendrawtransaction" "[\"$SIGNED_HEX\"]")
SEND_ERR=$(result_error "$RESP")
[[ -z "$SEND_ERR" ]] || { check_fail "sendrawtransaction (P2MR spend) -- $SEND_ERR"; exit 1; }
SPEND_TXID=$(extract_txid "$RESP")
[[ -n "$SPEND_TXID" && ${#SPEND_TXID} -ge 32 ]] || { check_fail "spend txid missing: $RESP"; exit 1; }
check_pass "mempool accepted P2MR spend: ${SPEND_TXID:0:16}..."

RESP=$(call "generatetoaddress" "[1, \"$TAPROOT_ADDR\"]")
H=$(height); check_eq "height after mining P2MR spend" "$H" "112"

BH=$(result_field "$(call "getblockhash" "[112]")" "")
BLOCK_TXS=$(call "getblock" "[\"$BH\"]")
IN_BLOCK=$(python3 -c "
import sys, json
tx = json.loads(sys.argv[1]).get('result',{}).get('tx',[])
print('yes' if sys.argv[2] in tx else 'no')
" "$BLOCK_TXS" "$SPEND_TXID")
check_eq "P2MR spend mined into block 112" "$IN_BLOCK" "yes"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "==============================================="
if [[ "$FAILURES" -eq 0 ]]; then
    echo "  ALL CHECKS PASSED"
    echo "==============================================="
    exit 0
else
    echo "  $FAILURES CHECK(S) FAILED"
    echo "==============================================="
    exit 1
fi
