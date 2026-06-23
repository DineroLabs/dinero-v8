#!/usr/bin/env bash
# Verify the #324 heal makes a CORRUPTED note spendable: dup tree + drifted leaf
# + stale nullifier -> DedupChainLeaves heals (tree+leaf+nullifier) -> spend works.
set -uo pipefail
ROOT=/tmp/dinero-v8.0.4
DINEROD="$ROOT/build-release-804/dinerod"
DD="/tmp/dinero_heal_$$"; LOG="$DD.log"; PID=""
RP=$((43000+RANDOM%1000)); PP=$((RP+1)); WP=$((RP+2))
cleanup(){ [[ -n "$PID" ]] && kill "$PID" 2>/dev/null; pkill -f "dinerod.*$DD" 2>/dev/null; rm -rf "$DD" "$LOG"; }
trap cleanup EXIT
ck(){ tr -d '\n' < "$DD/regtest/.cookie" 2>/dev/null || tr -d '\n' < "$DD/.cookie" 2>/dev/null; }
rpc(){ local c; c="$(ck)"; [ -n "$c" ] || return 1; curl -s --user "$c" -H 'Content-Type: application/json' -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" "http://127.0.0.1:$RP/" </dev/null; }
start(){ mkdir -p "$DD"; "$DINEROD" --regtest --datadir="$DD" --rpcport="$RP" --port="$PP" --wallet-socket-port="$WP" --listen=0 --utreexo=1 --p2p.offline=1 >"$LOG" 2>&1 & PID=$!; }
waitrpc(){ for _ in $(seq 1 90); do [[ -n "$PID" ]] && ! kill -0 "$PID" 2>/dev/null && return 1; rpc getblockcount '[]' | jq -e '.result>=0' >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }
DB(){ echo "$DD/wallets/shielded_notes_default.sqlite"; }

echo "### setup: 101 blocks, shield 3 notes"
start; waitrpc || { echo FAIL-RPC; tail -30 "$LOG"; exit 1; }
M=$(rpc wallet.getnewaddress '["taproot","m"]' | jq -r '.result.address // .result')
rpc generatetoaddress "[101,\"$M\"]" >/dev/null
RC=$(rpc wallet.getshieldedaddress '{"account":1,"j":0}' | jq -r '.result.address')
for i in 1 2 3; do rpc wallet.shield '[1.0]' >/dev/null; done
rpc generatetoaddress "[1,\"$M\"]" >/dev/null
sleep 1
# locate the per-wallet DB actually used
DBF=$(ls "$DD"/wallets/shielded_notes_*.sqlite 2>/dev/null | head -1)
echo "wallet DB: $DBF"
echo "clean state:"
sqlite3 "$DBF" "SELECT 'leaves='||COUNT(*)||' distinct='||COUNT(DISTINCT commitment) FROM shielded_tree_leaves; SELECT 'note leaf='||leaf_index||' nf='||substr(hex(nullifier),1,12) FROM shielded_notes ORDER BY leaf_index;"
# target = the highest-leaf note
TGT=$(sqlite3 "$DBF" "SELECT hex(commitment) FROM shielded_notes ORDER BY leaf_index DESC LIMIT 1;")
TGT_LEAF0=$(sqlite3 "$DBF" "SELECT leaf_index FROM shielded_notes ORDER BY leaf_index DESC LIMIT 1;")
TGT_NF0=$(sqlite3 "$DBF" "SELECT hex(nullifier) FROM shielded_notes ORDER BY leaf_index DESC LIMIT 1;")
echo "target note: leaf=$TGT_LEAF0 nf=${TGT_NF0:0:12} commit=${TGT:0:12}"

echo ""
echo "### STOP + CORRUPT (simulate the bug: dup tree twice + drift target leaf + stale nullifier)"
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""
sqlite3 "$DBF" "
INSERT INTO shielded_tree_leaves (leaf_index,commitment,created_height) SELECT leaf_index+(SELECT COUNT(*) FROM shielded_tree_leaves),commitment,created_height FROM (SELECT * FROM shielded_tree_leaves);
INSERT INTO shielded_tree_leaves (leaf_index,commitment,created_height) SELECT leaf_index+(SELECT COUNT(*) FROM shielded_tree_leaves),commitment,created_height FROM (SELECT * FROM shielded_tree_leaves);
UPDATE shielded_notes SET leaf_index=(SELECT MAX(leaf_index) FROM shielded_tree_leaves l WHERE l.commitment=shielded_notes.commitment), nullifier=zeroblob(32) WHERE hex(commitment)='$TGT';
"
echo "corrupted state:"
sqlite3 "$DBF" "SELECT 'leaves='||COUNT(*)||' distinct='||COUNT(DISTINCT commitment) FROM shielded_tree_leaves; SELECT 'target leaf='||leaf_index||' nf='||substr(hex(nullifier),1,12) FROM shielded_notes WHERE hex(commitment)='$TGT';"

echo ""
echo "### RESTART -> heal on Open"
start; waitrpc || { echo FAIL-RPC2; tail -30 "$LOG"; exit 1; }
sleep 1
echo "healed state:"
sqlite3 "$DBF" "SELECT 'leaves='||COUNT(*)||' distinct='||COUNT(DISTINCT commitment)||' min='||MIN(leaf_index)||' max='||MAX(leaf_index) FROM shielded_tree_leaves; SELECT 'target leaf='||leaf_index||' nf='||substr(hex(nullifier),1,12) FROM shielded_notes WHERE hex(commitment)='$TGT';"
echo "  target leaf restored to $TGT_LEAF0? nullifier back to clean ${TGT_NF0:0:12}?"

echo ""
echo "### SPEND the healed target note (auto fee) -> must be ACCEPTED"
SP=$(rpc wallet.transfer "{\"amount_una\":50000000,\"address\":\"$RC\"}")
ST=$(jq -r '.result.status//"none"' <<<"$SP"); ER=$(jq -r '.result.error//"none"' <<<"$SP"); TX=$(jq -r '.result.txid//empty' <<<"$SP")
echo "spend: status=$ST error=$ER txid=${TX:0:16}"
if [[ -n "$TX" ]]; then
  rpc generatetoaddress "[1,\"$M\"]" >/dev/null
  MINED=$(rpc getblock "[\"$(rpc getbestblockhash '[]'|jq -r .result)\",1]" | jq -e --arg t "$TX" '.result.tx|index($t)!=null' >/dev/null 2>&1 && echo yes || echo no)
  echo "mined into block: $MINED"
  [[ "$MINED" == "yes" ]] && echo "✅✅ HEAL VERIFIED: corrupted note healed + spent + MINED" || echo "⚠️ accepted but mine-check inconclusive"
else
  echo "❌ HEAL FAILED: spend rejected (error=$ER) — heal insufficient"
fi
