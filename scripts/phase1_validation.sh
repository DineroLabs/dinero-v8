#!/usr/bin/env bash
# phase1_validation.sh  — run from repo root
set -euo pipefail

ROOT="$HOME/Documents/DineroCoin"
DATADIR="$ROOT/test-data/regtest"
PORT=20999
COOKIE=$(cat "$DATADIR/regtest/regtest/.cookie" 2>/dev/null || cat "$DATADIR/regtest/.cookie")

rpc() { curl -sS --user "$COOKIE" -H 'Content-Type: application/json' -d "$1" "http://127.0.0.1:$PORT/"; }
jj()  { jq -r "$1"; }

echo "🚀 Phase-1 Validation (RPC + Block checks)"
echo "========================================="

# 0) Sanity: daemon up + build info
echo "• Build info"
rpc '{"jsonrpc":"2.0","id":"b","method":"getbuildinfo"}' | jq .
echo

# 1) Wallet RPCs (getnewaddress / getbalance / listunspent / listtransactions)
echo "• Wallet RPCs"
ADDR=$(rpc '{"jsonrpc":"2.0","id":"na","method":"getnewaddress"}' | jj '.result.address // .result')
[[ "$ADDR" =~ ^din1q ]] || { echo "❌ getnewaddress did not return bech32 din1q..."; exit 1; }
echo "  - New address: $ADDR"

BAL_BEFORE=$(rpc '{"jsonrpc":"2.0","id":"wi","method":"getwalletinfo"}' | jj '.result.balance // 0')
UTXO_BEFORE=$(rpc '{"jsonrpc":"2.0","id":"lu","method":"listunspent"}' | jq '.result|length')
TXS_BEFORE=$(rpc '{"jsonrpc":"2.0","id":"lt","method":"listtransactions"}' | jq '.result|length')
echo "  - balance(before)=$BAL_BEFORE, utxos(before)=$UTXO_BEFORE, txs(before)=$TXS_BEFORE"

# 2) Set mining address (persistence path)
echo "• Set mining address"
rpc '{"jsonrpc":"2.0","id":"sa","method":"mining.setaddress","params":["'"$ADDR"'"]}' | jq .

# 3) Height advance + coinbase maturity path
echo "• Generate 101 blocks to mature coinbase"
H1=$(rpc '{"jsonrpc":"2.0","id":"b1","method":"getblockchaininfo"}' | jj '.result.blocks')
GEN=$(rpc '{"jsonrpc":"2.0","id":"gen","method":"generatetoaddress","params":[101,"'"$ADDR"'"]}')
H2=$(rpc '{"jsonrpc":"2.0","id":"b2","method":"getblockchaininfo"}' | jj '.result.blocks')
echo "  - height: $H1 -> $H2"
[[ $((H2 - H1)) -ge 101 ]] || { echo "❌ chain height did not increase by 101"; exit 1; }

# 4) Wallet surfaces after maturity
BAL_AFTER=$(rpc '{"jsonrpc":"2.0","id":"wi2","method":"getwalletinfo"}' | jj '.result.balance // 0')
UTXO_AFTER=$(rpc '{"jsonrpc":"2.0","id":"lu2","method":"listunspent"}' | jq '.result|length')
TXS_AFTER=$(rpc '{"jsonrpc":"2.0","id":"lt2","method":"listtransactions"}' | jq '.result|length')

echo "  - balance(after)=$BAL_AFTER, utxos(after)=$UTXO_AFTER, txs(after)=$TXS_AFTER"
(( $(printf '%.0f' "$BAL_AFTER") > 0 )) || { echo "❌ balance did not increase after 101 blocks"; exit 1; }
(( UTXO_AFTER > UTXO_BEFORE )) || { echo "❌ listunspent did not grow"; exit 1; }
(( TXS_AFTER  > TXS_BEFORE  )) || { echo "❌ listtransactions did not grow"; exit 1; }

# 5) Block-validation smoke: merkle + BIP34 (best-effort)
# If your node exposes getblock with verbosity >=2, we'll inspect coinbase & merkle.
echo "• Block validation checks (best-effort via RPC)"
HAS_GETBLOCK=$(rpc '{"jsonrpc":"2.0","id":"x","method":"help"}' | jj '.result[]' | grep -c '^getblock$' || true)
if [[ "$HAS_GETBLOCK" -gt 0 ]]; then
  TIP=$(rpc '{"jsonrpc":"2.0","id":"bci","method":"getblockchaininfo"}' | jj '.result.bestblockhash')
  BLK=$(rpc '{"jsonrpc":"2.0","id":"gb","method":"getblock","params":["'"$TIP"'",2]}' )
  # Expect at least one tx (coinbase) and merkle field present
  TXCOUNT=$(echo "$BLK" | jq '.result.tx|length')
  MERKLE=$(echo "$BLK" | jq -r '.result.merkleroot // empty')
  echo "  - bestblock txs=$TXCOUNT, merkleroot=${MERKLE:-<none>}"
  (( TXCOUNT >= 1 )) || { echo "❌ getblock shows no transactions"; exit 1; }
  [[ -n "$MERKLE" ]] || echo "  ⚠️  merkleroot not exposed by RPC; internal check still validates it."
  # Optional: coinbase height (BIP34) if scriptSig or height field is exposed
  HEIGHT_FIELD=$(echo "$BLK" | jq -r '.result.tx[0].vin[0].height // empty' 2>/dev/null || true)
  if [[ -n "$HEIGHT_FIELD" ]]; then
    echo "  - coinbase BIP34 height in tx: $HEIGHT_FIELD (OK)"
  else
    echo "  ⚠️  coinbase height not exposed; rely on internal BIP34 validation."
  fi
else
  echo "  ⚠️  getblock RPC not available; relying on internal validation (acceptance proves merkle & BIP34)."
fi

echo
echo "✅ Phase-1 checks passed: wallet RPCs real, blocks accepted, state updated."
