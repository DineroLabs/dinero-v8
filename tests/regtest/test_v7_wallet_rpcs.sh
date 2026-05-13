#!/usr/bin/env bash
# ============================================================================
# V7 Post-Quantum Wallet RPCs — Regtest End-to-End
# ============================================================================
# Spec: docs/consensus/V7_WALLET_SCHEMA.md
# Phase: 4c.3.1 Commit 3/3 — vertical-slice validation for the three safe
#        v7 wallet RPCs wired into dinerod:
#
#          wallet.getnewp2mraddress
#          wallet.listp2mraddresses
#          wallet.signp2mr
#
# The wallet.exportp2mrseed / wallet.importp2mrseed RPCs are intentionally
# NOT tested here — they remain dark until the seed-export review pass.
#
# What this script validates (no more, no less):
#
#   T1. Preamble: all three RPCs error cleanly on a locked wallet
#       (error = "wallet_locked"). Proves the AcquireUnlockedWallet
#       guard fires before any secret-handling code runs.
#
#   T2. wallet.getnewp2mraddress produces a valid P2MR address, 32-byte
#       merkle_root_hex, 1952-byte (3904-hex-char) ML-DSA-65 pubkey_hex,
#       and a BIP-44-style derivation_path under coin_type=1448.
#
#   T3. Store invariants:
#         a) second call at an IDENTICAL derivation path returns
#            HandlerStatus::UniqueConflict — proves the
#            (wallet_id, derivation_path, leaf_index) UNIQUE index
#            on v7_p2mr_addresses is being enforced.
#         b) call at a DIFFERENT leaf_index derives a distinct address
#            + merkle_root + pubkey — proves the HKDF → KeygenFromSeed
#            pipeline isn't collapsing different paths onto one key.
#
#   T4. wallet.listp2mraddresses returns all addresses created so far,
#       including the deterministic row from T3 counted exactly once.
#
#   T5. wallet.signp2mr produces a non-empty signature_hex of the right
#       length (3309 bytes = 6618 hex chars for ML-DSA-65) for a
#       synthetic 32-byte sighash. Signature is not checked for
#       cryptographic validity here — p2mr_verify_test covers that.
#
# Usage: ./test_v7_wallet_rpcs.sh [dinerod_path]
# ============================================================================
set -euo pipefail

DINEROD="${1:-/Users/haydarevich/src/dinero/build/dinerod}"
RPCPORT=18490
P2PPORT=18491
TMPDIR=$(mktemp -d /tmp/dinero_v7_wallet_rpcs_XXXXXX)
PID=""
FAILURES=0

WALLET_NAME="v7test"
WALLET_PASSWORD="v7-regtest-password"

rpc() {
    curl -s --max-time 30 -u test:test \
        -H "Content-Type: application/json" \
        -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" \
        "http://127.0.0.1:$RPCPORT/" 2>/dev/null
}

height() {
    rpc "getblockcount" "[]" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result','?'))" 2>/dev/null
}

result_field() {
    # Usage: result_field <json> <dotted.path>
    python3 -c "
import sys, json
doc = json.loads(sys.argv[1])
path = sys.argv[2].split('.')
cur = doc.get('result', {})
for k in path:
    if isinstance(cur, dict):
        cur = cur.get(k, '')
    else:
        cur = ''
        break
print(cur if cur is not None else '')
" "$1" "$2"
}

result_error() {
    python3 -c "
import sys, json
doc = json.loads(sys.argv[1])
res = doc.get('result', {})
if isinstance(res, dict) and res.get('error'):
    print(res['error'])
elif doc.get('error'):
    err = doc['error']
    print(err.get('message', '') if isinstance(err, dict) else str(err))
else:
    print('')
" "$1"
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

check_len_eq() {
    local desc="$1" actual_len="$2" expected_len="$3"
    if [[ "$actual_len" == "$expected_len" ]]; then
        check_pass "$desc (len=$actual_len)"
    else
        check_fail "$desc -- expected length $expected_len, got $actual_len"
    fi
}

check_nonempty() {
    local desc="$1" value="$2"
    if [[ -n "$value" ]]; then
        check_pass "$desc"
    else
        check_fail "$desc -- value was empty"
    fi
}

check_hex_len() {
    # Checks value is hex and has exactly N characters.
    local desc="$1" value="$2" expected_len="$3"
    if [[ ! "$value" =~ ^[0-9a-f]+$ ]]; then
        check_fail "$desc -- value is not lowercase hex: '${value:0:20}...'"
        return
    fi
    if [[ "${#value}" != "$expected_len" ]]; then
        check_fail "$desc -- expected hex length $expected_len, got ${#value}"
        return
    fi
    check_pass "$desc (${expected_len} hex chars = $((expected_len / 2)) bytes)"
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
echo " V7 PQ Wallet RPCs — Regtest End-to-End"
echo "==============================================="
echo "  dinerod:            $DINEROD"
echo "  datadir:            $TMPDIR"
echo "  wallet name:        $WALLET_NAME"
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
    if height 2>/dev/null | grep -qE '^[0-9]+$'; then
        echo "  Node ready (${i}s)"
        READY=1
        break
    fi
    sleep 1
done

if [[ "$READY" != "1" ]]; then
    check_fail "Node never became ready"
    tail -40 "$TMPDIR/node.log" | sed 's/^/       /'
    exit 1
fi

# ---------------------------------------------------------------------------
# Phase 1 — create an HD wallet and encrypt it with a passphrase
#
# We deliberately split create + encrypt (instead of passing the password
# directly to wallet.createhd) because the Argon2id path used by
# wallet.createhd's integrated encryption is not currently interoperable
# with the PBKDF2-based unlockWallet flow that loads the v7 PQ master
# key. wallet.encrypt sets the PBKDF2 wallet_salt + wallet_verify_hash
# settings that unlockWallet (and by extension the v7 master-key
# generate-on-first-unlock code in V7_WALLET_SCHEMA.md §5b) expects.
# ---------------------------------------------------------------------------
echo ""
echo "Phase 1: Create + encrypt HD wallet ($WALLET_NAME)"

CREATE_RESP=$(rpc "wallet.createhd" "[\"$WALLET_NAME\"]")
CREATE_ERR=$(result_error "$CREATE_RESP")
if [[ -n "$CREATE_ERR" ]]; then
    check_fail "wallet.createhd -- $CREATE_ERR"
    echo "       response: $CREATE_RESP" | head -c 400
    exit 1
fi
check_pass "wallet.createhd succeeded (unencrypted)"

ENCRYPT_RESP=$(rpc "wallet.encrypt" "[\"$WALLET_PASSWORD\"]")
ENCRYPT_ERR=$(result_error "$ENCRYPT_RESP")
if [[ -n "$ENCRYPT_ERR" ]]; then
    check_fail "wallet.encrypt -- $ENCRYPT_ERR"
    echo "       response: $ENCRYPT_RESP" | head -c 400
    exit 1
fi
check_pass "wallet.encrypt succeeded (wallet is now encrypted + locked)"

# ---------------------------------------------------------------------------
# Phase 2 — T1: v7 RPCs error cleanly on a locked wallet
# ---------------------------------------------------------------------------
echo ""
echo "Phase 2 — T1: v7 RPCs reject locked wallet before touching secrets"

for METHOD in wallet.getnewp2mraddress wallet.listp2mraddresses; do
    RESP=$(rpc "$METHOD" "{}")
    ERR=$(result_field "$RESP" "error")
    check_eq "$METHOD on locked wallet → error=wallet_locked" "$ERR" "wallet_locked"
done

# wallet.signp2mr needs both address and sighash_hex fields in params.
# We expect the locked-wallet check to fire before param validation kicks in.
RESP=$(rpc "wallet.signp2mr" '{"address":"din1r0","sighash_hex":"0000000000000000000000000000000000000000000000000000000000000000"}')
ERR=$(result_field "$RESP" "error")
check_eq "wallet.signp2mr on locked wallet → error=wallet_locked" "$ERR" "wallet_locked"

# ---------------------------------------------------------------------------
# Phase 3 — unlock the wallet (triggers v7 master-key generate-on-first-unlock)
# ---------------------------------------------------------------------------
echo ""
echo "Phase 3: Unlock wallet (generates v7 PQ master key on first unlock)"

UNLOCK_RESP=$(rpc "wallet.unlock" "[\"$WALLET_PASSWORD\", 600]")
UNLOCK_ERR=$(result_error "$UNLOCK_RESP")
if [[ -n "$UNLOCK_ERR" ]]; then
    check_fail "wallet.unlock -- $UNLOCK_ERR"
    exit 1
fi
check_pass "wallet.unlock succeeded"

if grep -q "V7 PQ master key generated + persisted" "$TMPDIR/node.log"; then
    check_pass "v7 master key generated on first unlock (per V7_WALLET_SCHEMA §5b)"
else
    check_fail "no 'V7 PQ master key generated + persisted' log line — master key setup may have failed"
fi

# ---------------------------------------------------------------------------
# Phase 4 — T2: wallet.getnewp2mraddress produces a well-formed P2MR address
# ---------------------------------------------------------------------------
echo ""
echo "Phase 4 — T2: wallet.getnewp2mraddress returns a well-formed row"

ADDR_RESP=$(rpc "wallet.getnewp2mraddress" '{"account":0,"change":0,"address_index":0,"leaf_index":0,"label":"regtest-a0c0i0l0"}')
ADDR_ERR=$(result_error "$ADDR_RESP")
if [[ -n "$ADDR_ERR" ]]; then
    check_fail "wallet.getnewp2mraddress -- $ADDR_ERR"
    echo "       response: $ADDR_RESP"
    exit 1
fi

ADDR=$(result_field "$ADDR_RESP" "address")
MERKLE_HEX=$(result_field "$ADDR_RESP" "merkle_root_hex")
PUBKEY_HEX=$(result_field "$ADDR_RESP" "pubkey_hex")
DERIV_PATH=$(result_field "$ADDR_RESP" "derivation_path")
LEAF_IDX=$(result_field "$ADDR_RESP" "leaf_index")

check_nonempty "address non-empty"                              "$ADDR"
# Regtest uses the "dinrt" HRP by default in the v5 codec; v7 handler defaults
# HRP to "din" when unspecified. Accept both prefixes rather than hard-coding.
if [[ "$ADDR" =~ ^din ]]; then
    check_pass "address has 'din' prefix ('${ADDR:0:8}...')"
else
    check_fail "address prefix unexpected ('${ADDR:0:8}...')"
fi

check_hex_len    "merkle_root_hex is 32 bytes"                  "$MERKLE_HEX" 64
check_hex_len    "pubkey_hex is 1952 bytes (ML-DSA-65)"         "$PUBKEY_HEX" 3904
check_eq         "derivation_path uses purpose=88 and coin_type=1448" "$DERIV_PATH" "m/88'/1448'/0'/0/0"
check_eq         "leaf_index echoed back"                       "$LEAF_IDX"   "0"

# ---------------------------------------------------------------------------
# Phase 5 — T3a: repeat at IDENTICAL path hits the store's UNIQUE constraint
# ---------------------------------------------------------------------------
echo ""
echo "Phase 5 — T3a: repeat call at identical path → unique_conflict"

DUP_RESP=$(rpc "wallet.getnewp2mraddress" '{"account":0,"change":0,"address_index":0,"leaf_index":0,"label":"regtest-a0c0i0l0"}')
DUP_ERR=$(result_field "$DUP_RESP" "error")
check_eq "duplicate derivation path → error=unique_conflict" "$DUP_ERR" "unique_conflict"

# ---------------------------------------------------------------------------
# Phase 5b — T3b: different leaf_index produces a DIFFERENT valid row
# ---------------------------------------------------------------------------
echo ""
echo "Phase 5b — T3b: different leaf_index derives a distinct address"

ALT_RESP=$(rpc "wallet.getnewp2mraddress" '{"account":0,"change":0,"address_index":0,"leaf_index":1,"label":"regtest-a0c0i0l1"}')
ALT_ERR=$(result_error "$ALT_RESP")
if [[ -n "$ALT_ERR" ]]; then
    check_fail "wallet.getnewp2mraddress (leaf_index=1) -- $ALT_ERR"
else
    ALT_ADDR=$(result_field "$ALT_RESP" "address")
    ALT_MERKLE=$(result_field "$ALT_RESP" "merkle_root_hex")
    ALT_PUBKEY=$(result_field "$ALT_RESP" "pubkey_hex")
    ALT_LEAF=$(result_field "$ALT_RESP" "leaf_index")

    if [[ -n "$ALT_ADDR" && "$ALT_ADDR" != "$ADDR" ]]; then
        check_pass "leaf_index=1 address differs from leaf_index=0 ('${ALT_ADDR:0:12}...' vs '${ADDR:0:12}...')"
    else
        check_fail "leaf_index=1 address equals leaf_index=0 — HKDF collision or ignored leaf_index"
    fi
    [[ "$ALT_MERKLE" != "$MERKLE_HEX" ]] && check_pass "leaf_index=1 merkle_root differs" || check_fail "leaf_index=1 merkle_root equals leaf_index=0"
    [[ "$ALT_PUBKEY" != "$PUBKEY_HEX" ]] && check_pass "leaf_index=1 pubkey differs"      || check_fail "leaf_index=1 pubkey equals leaf_index=0"
    check_eq "leaf_index=1 echoed back" "$ALT_LEAF" "1"
fi

# ---------------------------------------------------------------------------
# Phase 6 — T4: wallet.listp2mraddresses returns both created rows
# ---------------------------------------------------------------------------
echo ""
echo "Phase 6 — T4: wallet.listp2mraddresses returns both created rows"

LIST_RESP=$(rpc "wallet.listp2mraddresses" "{}")
LIST_ERR=$(result_error "$LIST_RESP")
if [[ -n "$LIST_ERR" ]]; then
    check_fail "wallet.listp2mraddresses -- $LIST_ERR"
else
    COUNT=$(python3 -c "
import sys, json
doc = json.loads(sys.argv[1])
addrs = doc.get('result', {}).get('addresses', [])
print(sum(1 for a in addrs if a.get('address') == sys.argv[2]))
" "$LIST_RESP" "$ADDR")
    check_eq "leaf_index=0 address appears exactly once in list" "$COUNT" "1"

    TOTAL=$(python3 -c "
import sys, json
doc = json.loads(sys.argv[1])
print(len(doc.get('result', {}).get('addresses', [])))
" "$LIST_RESP")
    check_eq "list length = 2 (leaf_index 0 + 1)" "$TOTAL" "2"
fi

# ---------------------------------------------------------------------------
# Phase 7 — T5: wallet.signp2mr produces a full ML-DSA-65 signature
# ---------------------------------------------------------------------------
echo ""
echo "Phase 7 — T5: wallet.signp2mr returns a full ML-DSA-65 signature"

# Synthetic 32-byte sighash. Not a real transaction — p2mr_verify_test covers
# cryptographic validity; here we validate the RPC plumbing + output shape.
SIGHASH_HEX="0101010101010101010101010101010101010101010101010101010101010101"
SIGN_RESP=$(rpc "wallet.signp2mr" "{\"address\":\"$ADDR\",\"sighash_hex\":\"$SIGHASH_HEX\"}")
SIGN_ERR=$(result_error "$SIGN_RESP")
if [[ -n "$SIGN_ERR" ]]; then
    check_fail "wallet.signp2mr -- $SIGN_ERR"
    echo "       response: $SIGN_RESP" | head -c 400
else
    SCHEME_ID=$(result_field "$SIGN_RESP" "scheme_id")
    SIGN_PUBKEY=$(result_field "$SIGN_RESP" "pubkey_hex")
    SIG_HEX=$(result_field "$SIGN_RESP" "signature_hex")

    check_eq         "scheme_id = 1 (ML-DSA-65)"                      "$SCHEME_ID"   "1"
    check_eq         "signing pubkey matches address pubkey"          "$SIGN_PUBKEY" "$PUBKEY_HEX"
    check_hex_len    "signature_hex is 3309 bytes (ML-DSA-65)"        "$SIG_HEX"     6618
fi

# ---------------------------------------------------------------------------
# Phase 8 — negative: wallet.signp2mr rejects malformed sighash
# ---------------------------------------------------------------------------
echo ""
echo "Phase 8 — negative input handling"

BAD_RESP=$(rpc "wallet.signp2mr" "{\"address\":\"$ADDR\",\"sighash_hex\":\"deadbeef\"}")
BAD_ERR=$(result_field "$BAD_RESP" "error")
check_eq "short sighash → error=invalid_params" "$BAD_ERR" "invalid_params"

MISSING_RESP=$(rpc "wallet.signp2mr" "{}")
MISSING_ERR=$(result_field "$MISSING_RESP" "error")
check_eq "missing address → error=missing_address" "$MISSING_ERR" "missing_address"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "==============================================="
if [[ "$FAILURES" -eq 0 ]]; then
    echo " RESULT: ALL CHECKS PASSED"
    echo "==============================================="
    exit 0
else
    echo " RESULT: $FAILURES FAILURE(S)"
    echo "==============================================="
    echo ""
    echo "Tail of node log for context:"
    tail -40 "$TMPDIR/node.log" | sed 's/^/  /'
    exit 1
fi
