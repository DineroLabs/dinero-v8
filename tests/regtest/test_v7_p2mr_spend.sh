#!/usr/bin/env bash
# ============================================================================
# V7 P2MR End-to-End Spend — Regtest
# ============================================================================
# Phase 6 Commit 3/3 — proves a v7 post-quantum P2MR output can be
# created, mined, then spent with an ML-DSA-65 signature and the spend
# accepted into mempool + confirmed in a block.
#
# Flow:
#
#   A. Set up wallet + mine a mature coinbase
#       a.1  wallet.createhd → wallet.encrypt → wallet.unlock
#       a.2  generatetoaddress 110 blocks to a taproot address
#            (past COINBASE_MATURITY=100 so block 1's coinbase is spendable)
#
#   B. Create a P2MR output
#       b.1  wallet.getnewp2mraddress → { address, merkle_root_hex, pubkey_hex }
#       b.2  wallet.createrawtransaction — spending the block-1 coinbase,
#            output 0 = P2MR scriptPubKey ("5320<merkle_root_hex>")
#       b.3  wallet.signrawtransactionwithwallet — signs the Taproot
#            coinbase input
#       b.4  sendrawtransaction → mempool accepts (Taproot spend, P2MR
#            output)
#       b.5  generatetoaddress 1 → P2MR UTXO confirmed
#
#   C. Spend the P2MR UTXO (the interesting half)
#       c.1  wallet.createrawtransaction — input = P2MR UTXO from B,
#            output = back to a taproot address
#       c.2  debug.computesighash — exact BIP-341 sighash the validator
#            will compute
#       c.3  wallet.signp2mr(p2mr_addr, sighash_hex) — ML-DSA-65 signature
#       c.4  build canonical P2MR witness (scheme_id | pubkey | sig |
#            depth=0 | leaf_index=0)
#       c.5  debug.attachwitness — inject the witness onto input 0
#       c.6  sendrawtransaction — mempool must accept (this is the first
#            real exercise of ValidateP2MRSpend)
#       c.7  generatetoaddress 1 → P2MR spend confirmed
#       c.8  assert the spend's txid appears in the block's tx list
#
# Usage: ./test_v7_p2mr_spend.sh [dinerod_path]
# ============================================================================
set -euo pipefail

DINEROD="${1:-/Users/haydarevich/src/dinero/build/dinerod}"
RPCPORT=18492
P2PPORT=18493
TMPDIR=$(mktemp -d /tmp/dinero_v7_p2mr_spend_XXXXXX)
PID=""
FAILURES=0

WALLET_NAME="v7spend"
WALLET_PASSWORD="v7-p2mr-regtest"

# ML-DSA-65 sizes (bytes).
MLDSA_PUBKEY_BYTES=1952
MLDSA_SIG_BYTES=3309

rpc() {
    curl -s --max-time 30 -u test:test \
        -H "Content-Type: application/json" \
        -d "$2" \
        "http://127.0.0.1:$RPCPORT/" 2>/dev/null
}

# Build JSON envelope + call. Uses python3 json.dumps to avoid hand-escaping.
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

# Extract a txid-shaped string from a response. sendrawtransaction and
# similar handlers can return either a bare string or a {'result': txid}
# wrapping depending on RPC server path. We handle both.
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
echo " V7 P2MR End-to-End Spend — Regtest"
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

# Get a taproot address to mine to — this is also our change destination later.
# wallet.getnewaddress params are [address_type, label].
RESP=$(call "wallet.getnewaddress" '["taproot","mine"]')
TAPROOT_ADDR=$(result_field "$RESP" "address")
if [[ -z "$TAPROOT_ADDR" ]]; then
    # Some builds return the address as the raw result string.
    TAPROOT_ADDR=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$RESP")
fi
[[ -n "$TAPROOT_ADDR" ]] || { check_fail "could not obtain a taproot address (resp: $RESP)"; exit 1; }
check_pass "taproot mining address: ${TAPROOT_ADDR:0:18}..."

# ---------------------------------------------------------------------------
# Phase A.2 — mine 110 blocks to TAPROOT_ADDR (past coinbase maturity)
# ---------------------------------------------------------------------------
echo ""
echo "Phase A.2: mine 110 blocks to $TAPROOT_ADDR (past COINBASE_MATURITY=100)"

RESP=$(call "generatetoaddress" "[110, \"$TAPROOT_ADDR\"]")
if [[ -n "$(result_error "$RESP")" ]]; then
    check_fail "generatetoaddress -- $(result_error "$RESP")"
    exit 1
fi
H=$(height); check_eq "at height 110" "$H" "110"

# Find a mature, spendable coinbase UTXO via listunspent. More robust than
# getblock/getrawtransaction (doesn't require -txindex).
RESP=$(call "wallet.listunspent" "[1]")
UTXO_PICK=$(python3 -c "
import sys, json
utxos = json.loads(sys.argv[1]).get('result', [])
if isinstance(utxos, dict):
    utxos = utxos.get('utxos', []) or utxos.get('addresses', []) or []
# pick first mature + spendable entry
for u in utxos:
    if u.get('is_mature', True) and u.get('spendable', True):
        print(json.dumps({'txid': u['txid'], 'vout': u['vout'],
                          'amount_una': u.get('amount_una'),
                          'scriptPubKey': u.get('scriptPubKey', '')}))
        sys.exit(0)
print('')
" "$RESP")
[[ -n "$UTXO_PICK" ]] || { check_fail "no mature spendable utxo found"; echo "       resp: $RESP" | head -c 600; exit 1; }
COINBASE_TXID=$(python3 -c "import sys,json; print(json.loads(sys.argv[1])['txid'])" "$UTXO_PICK")
COINBASE_VOUT=$(python3 -c "import sys,json; print(json.loads(sys.argv[1])['vout'])" "$UTXO_PICK")
COINBASE_AMOUNT_SAT=$(python3 -c "import sys,json; print(json.loads(sys.argv[1])['amount_una'])" "$UTXO_PICK")
[[ "$COINBASE_AMOUNT_SAT" -gt 0 ]] || { check_fail "coinbase amount zero: $UTXO_PICK"; exit 1; }
check_pass "mature coinbase: ${COINBASE_TXID:0:16}...:$COINBASE_VOUT (${COINBASE_AMOUNT_SAT} sat)"

# ---------------------------------------------------------------------------
# Phase B — create a P2MR output
# ---------------------------------------------------------------------------
echo ""
echo "Phase B: build taproot → P2MR tx (Taproot input, P2MR output)"

RESP=$(call "wallet.getnewp2mraddress" '{"account":0,"change":0,"address_index":0,"leaf_index":0,"label":"spend-test"}')
if [[ -n "$(result_error "$RESP")" ]]; then
    check_fail "wallet.getnewp2mraddress -- $(result_error "$RESP")"
    exit 1
fi
P2MR_ADDR=$(result_field "$RESP" "address")
P2MR_MERKLE_HEX=$(result_field "$RESP" "merkle_root_hex")
P2MR_PUBKEY_HEX=$(result_field "$RESP" "pubkey_hex")
check_pass "P2MR address: ${P2MR_ADDR:0:14}..."
check_eq "merkle_root_hex is 64 chars"  "${#P2MR_MERKLE_HEX}" "64"
check_eq "pubkey_hex    is 3904 chars"  "${#P2MR_PUBKEY_HEX}" "3904"

# P2MR scriptPubKey = 0x53 0x20 || merkle_root
P2MR_SPK_HEX="5320${P2MR_MERKLE_HEX}"
check_eq "P2MR scriptPubKey is 68 hex chars (34 bytes)" "${#P2MR_SPK_HEX}" "68"

# Reserve fee + split into P2MR output and taproot change.
FEE_SAT=10000
P2MR_VALUE_SAT=3000000000                               # 30 DIN
CHANGE_VALUE_SAT=$((COINBASE_AMOUNT_SAT - P2MR_VALUE_SAT - FEE_SAT))
[[ "$CHANGE_VALUE_SAT" -gt 0 ]] || { check_fail "coinbase too small for split ($COINBASE_AMOUNT_SAT sat)"; exit 1; }

# Floating-point amounts for createrawtransaction (it expects DIN).
P2MR_VALUE_DIN=$(python3 -c "print(f'{$P2MR_VALUE_SAT / 1e8:.8f}')")
CHANGE_VALUE_DIN=$(python3 -c "print(f'{$CHANGE_VALUE_SAT / 1e8:.8f}')")

OUTPUTS_JSON=$(python3 -c "
import sys, json
p2mr_spk   = sys.argv[1]
p2mr_amt   = float(sys.argv[2])
change_a   = sys.argv[3]
change_amt = float(sys.argv[4])
print(json.dumps([
    {'scriptPubKey': p2mr_spk, 'amount': p2mr_amt},
    {change_a: change_amt},
]))
" "$P2MR_SPK_HEX" "$P2MR_VALUE_DIN" "$TAPROOT_ADDR" "$CHANGE_VALUE_DIN")

INPUTS_JSON="[{\"txid\":\"$COINBASE_TXID\",\"vout\":$COINBASE_VOUT}]"

RESP=$(call "wallet.createrawtransaction" "[$INPUTS_JSON, $OUTPUTS_JSON]")
if [[ -n "$(result_error "$RESP")" ]]; then
    check_fail "wallet.createrawtransaction -- $(result_error "$RESP")"
    echo "       outputs: $OUTPUTS_JSON"
    exit 1
fi
CREATE_TX_HEX=$(result_field "$RESP" "hex")
[[ -n "$CREATE_TX_HEX" ]] || { check_fail "create-tx empty hex"; exit 1; }
check_pass "created unsigned Taproot→P2MR tx (${#CREATE_TX_HEX} hex chars)"

RESP=$(call "wallet.signrawtransaction" "[\"$CREATE_TX_HEX\"]")
SIGN_ERR=$(result_error "$RESP")
if [[ -n "$SIGN_ERR" ]]; then
    check_fail "wallet.signrawtransactionwithwallet -- $SIGN_ERR"
    exit 1
fi
SIGNED_TX_HEX=$(result_field "$RESP" "hex")
COMPLETE=$(result_field "$RESP" "complete")
[[ "$COMPLETE" == "True" || "$COMPLETE" == "true" ]] || { check_fail "signrawtx returned complete=$COMPLETE"; exit 1; }
check_pass "wallet signed the Taproot input"

RESP=$(call "sendrawtransaction" "[\"$SIGNED_TX_HEX\"]")
SEND_ERR=$(result_error "$RESP")
[[ -z "$SEND_ERR" ]] || { check_fail "sendrawtransaction (create P2MR) -- $SEND_ERR"; exit 1; }
TX_B_TXID=$(extract_txid "$RESP")
if [[ -z "$TX_B_TXID" || ${#TX_B_TXID} -lt 32 ]]; then
    check_fail "could not extract P2MR-creating tx's txid from response: $RESP"
    exit 1
fi
check_pass "sendrawtransaction accepted: ${TX_B_TXID:0:16}..."

RESP=$(call "generatetoaddress" "[1, \"$TAPROOT_ADDR\"]")
H=$(height); check_eq "height after mining P2MR-creating tx" "$H" "111"

# Confirm via the mined block's tx list (no -txindex required).
BLOCK111_HASH=$(result_field "$(call "getblockhash" "[111]")" "")
BLOCK111_TXLIST=$(call "getblock" "[\"$BLOCK111_HASH\"]")
IN_BLOCK=$(python3 -c "
import sys, json
tx = json.loads(sys.argv[1]).get('result',{}).get('tx',[])
print('yes' if sys.argv[2] in tx else 'no')
" "$BLOCK111_TXLIST" "$TX_B_TXID")
check_eq "P2MR-creating tx mined in block 111" "$IN_BLOCK" "yes"

# ---------------------------------------------------------------------------
# Phase C — spend the P2MR UTXO
# ---------------------------------------------------------------------------
echo ""
echo "Phase C: spend the P2MR UTXO using wallet.signp2mr + canonical witness"

SPEND_DEST_RESP=$(call "wallet.getnewaddress" '["taproot","spend-dest"]')
SPEND_DEST=$(result_field "$SPEND_DEST_RESP" "address")
[[ -n "$SPEND_DEST" ]] || SPEND_DEST=$(python3 -c "import sys,json; r=json.loads(sys.argv[1]).get('result'); print(r if isinstance(r,str) else '')" "$SPEND_DEST_RESP")
[[ -n "$SPEND_DEST" ]] || { check_fail "could not get a spend destination"; exit 1; }

SPEND_VALUE_SAT=$((P2MR_VALUE_SAT - FEE_SAT))
SPEND_VALUE_DIN=$(python3 -c "print(f'{$SPEND_VALUE_SAT / 1e8:.8f}')")
INPUTS_C="[{\"txid\":\"$TX_B_TXID\",\"vout\":0}]"
OUTPUTS_C="[{\"$SPEND_DEST\": $SPEND_VALUE_DIN}]"

RESP=$(call "wallet.createrawtransaction" "[$INPUTS_C, $OUTPUTS_C]")
SPEND_TX_HEX=$(result_field "$RESP" "hex")
[[ -n "$SPEND_TX_HEX" ]] || { check_fail "createrawtransaction (P2MR spend) empty hex"; exit 1; }
check_pass "unsigned P2MR-spending tx built (${#SPEND_TX_HEX} hex chars)"

# debug.computesighash — the sighash the validator will compute.
PREVOUTS_JSON="[{\"scriptPubKey\":\"$P2MR_SPK_HEX\",\"amount_sat\":$P2MR_VALUE_SAT}]"
CSH_PARAMS=$(python3 -c "
import sys, json
print(json.dumps({
    'raw_tx_hex': sys.argv[1],
    'input_index': 0,
    'prevouts': json.loads(sys.argv[2]),
}))
" "$SPEND_TX_HEX" "$PREVOUTS_JSON")
RESP=$(call "debug.computesighash" "$CSH_PARAMS")
if [[ -n "$(result_error "$RESP")" ]]; then
    check_fail "debug.computesighash -- $(result_error "$RESP")"
    exit 1
fi
SIGHASH_HEX=$(result_field "$RESP" "sighash_hex")
check_eq "sighash is 64 hex chars" "${#SIGHASH_HEX}" "64"

# wallet.signp2mr — ML-DSA-65 signature over the sighash.
SIGN_PARAMS=$(python3 -c "
import sys, json
print(json.dumps({'address': sys.argv[1], 'sighash_hex': sys.argv[2]}))
" "$P2MR_ADDR" "$SIGHASH_HEX")
RESP=$(call "wallet.signp2mr" "$SIGN_PARAMS")
if [[ -n "$(result_error "$RESP")" ]]; then
    check_fail "wallet.signp2mr -- $(result_error "$RESP")"
    exit 1
fi
SCHEME_ID=$(result_field "$RESP" "scheme_id")
SIG_PUBKEY=$(result_field "$RESP" "pubkey_hex")
SIG_HEX=$(result_field "$RESP" "signature_hex")
check_eq "scheme_id=1 (ML-DSA-65)"                    "$SCHEME_ID"    "1"
check_eq "signing pubkey matches P2MR address pubkey" "$SIG_PUBKEY"   "$P2MR_PUBKEY_HEX"
check_eq "signature is 6618 hex chars (3309 bytes)"   "${#SIG_HEX}"   "6618"

# Assemble canonical P2MR witness blob (hex). Varints are Bitcoin CompactSize.
WITNESS_HEX=$(python3 - <<PY
def compact_size(n: int) -> bytes:
    if n < 0xfd: return bytes([n])
    if n <= 0xffff: return b"\xfd" + n.to_bytes(2, "little")
    if n <= 0xffffffff: return b"\xfe" + n.to_bytes(4, "little")
    return b"\xff" + n.to_bytes(8, "little")

scheme_id = 0x01
pubkey    = bytes.fromhex("$P2MR_PUBKEY_HEX")
sig       = bytes.fromhex("$SIG_HEX")
depth     = 0
leaf_idx  = 0

buf  = bytes([scheme_id])
buf += compact_size(len(pubkey))
buf += pubkey
buf += compact_size(len(sig))
buf += sig
buf += bytes([depth])
buf += compact_size(leaf_idx)
print(buf.hex())
PY
)
# Expected length: 1 + 3 + 1952 + 3 + 3309 + 1 + 1 = 5270 bytes = 10540 hex chars
check_eq "canonical P2MR witness is 10540 hex chars" "${#WITNESS_HEX}" "10540"

# debug.attachwitness — inject the witness onto input 0.
ATTACH_PARAMS=$(python3 -c "
import sys, json
print(json.dumps({'raw_tx_hex': sys.argv[1], 'input_index': 0, 'witness_hex': sys.argv[2]}))
" "$SPEND_TX_HEX" "$WITNESS_HEX")
RESP=$(call "debug.attachwitness" "$ATTACH_PARAMS")
if [[ -n "$(result_error "$RESP")" ]]; then
    check_fail "debug.attachwitness -- $(result_error "$RESP")"
    exit 1
fi
FULL_TX_HEX=$(result_field "$RESP" "hex")
[[ -n "$FULL_TX_HEX" ]] || { check_fail "attachwitness returned empty"; exit 1; }
check_pass "attached witness; with-witness tx is ${#FULL_TX_HEX} hex chars"

# sendrawtransaction — this is the decisive check: mempool validates the
# P2MR spend by running our new ValidateP2MRSpend.
RESP=$(call "sendrawtransaction" "[\"$FULL_TX_HEX\"]")
SEND_ERR=$(result_error "$RESP")
if [[ -n "$SEND_ERR" ]]; then
    check_fail "sendrawtransaction (P2MR spend) -- $SEND_ERR"
    echo "       mempool log tail:"
    grep -iE "p2mr|witness|script|validate" "$TMPDIR/node.log" | tail -10 | sed 's/^/         /'
    exit 1
fi
SPEND_TXID=$(extract_txid "$RESP")
if [[ -z "$SPEND_TXID" || ${#SPEND_TXID} -lt 32 ]]; then
    check_fail "could not extract P2MR spend txid from response: $RESP"
    exit 1
fi
check_pass "mempool accepted P2MR spend: ${SPEND_TXID:0:16}..."

# ---------------------------------------------------------------------------
# Phase C.5 — prove the template builder PULLS P2MR from mempool.
#
# generatetoaddress would succeed even if the assembler silently dropped
# the P2MR tx (the block just wouldn't include it). This check hits
# getblocktemplate directly and asserts the spend's txid shows up in
# template.transactions — the list a real miner would hand to its
# hashing layer. If this fails, miners would leave P2MR txs in mempool
# forever on a live chain.
# ---------------------------------------------------------------------------
echo ""
echo "Phase C.5: getblocktemplate includes the pending P2MR spend"

GBT_PARAMS=$(python3 -c "
import sys, json
print(json.dumps([{'address': sys.argv[1]}]))
" "$TAPROOT_ADDR")
RESP=$(call "getblocktemplate" "$GBT_PARAMS")
GBT_ERR=$(result_error "$RESP")
if [[ -n "$GBT_ERR" ]]; then
    check_fail "getblocktemplate -- $GBT_ERR"
    exit 1
fi
IN_TEMPLATE=$(python3 -c "
import sys, json
txs = json.loads(sys.argv[1]).get('result',{}).get('transactions',[])
print('yes' if any(t.get('txid') == sys.argv[2] for t in txs) else 'no')
" "$RESP" "$SPEND_TXID")
check_eq "P2MR spend txid surfaces in getblocktemplate transactions" "$IN_TEMPLATE" "yes"

# Mine and assert the spend's txid appears in the new block's tx list.
RESP=$(call "generatetoaddress" "[1, \"$TAPROOT_ADDR\"]")
H=$(height); check_eq "height after mining P2MR spend" "$H" "112"

BLOCK112_HASH=$(result_field "$(call "getblockhash" "[112]")" "")
BLOCK112_TXLIST=$(call "getblock" "[\"$BLOCK112_HASH\"]")
IN_BLOCK=$(python3 -c "
import sys, json
tx = json.loads(sys.argv[1]).get('result',{}).get('tx',[])
print('yes' if sys.argv[2] in tx else 'no')
" "$BLOCK112_TXLIST" "$SPEND_TXID")
check_eq "P2MR spend confirmed in block 112" "$IN_BLOCK" "yes"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "==============================================="
if [[ "$FAILURES" -eq 0 ]]; then
    echo " RESULT: V7 P2MR SPEND END-TO-END PASSED"
    echo "==============================================="
    exit 0
else
    echo " RESULT: $FAILURES FAILURE(S)"
    echo "==============================================="
    echo ""
    echo "Tail of node log for context:"
    tail -60 "$TMPDIR/node.log" | sed 's/^/  /'
    exit 1
fi
