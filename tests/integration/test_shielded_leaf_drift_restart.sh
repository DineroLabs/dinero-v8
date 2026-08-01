#!/usr/bin/env bash
# Repro for #322 (explicit too-low shielded fee) and #324 (leaf_index drift on restart)
set -uo pipefail
ROOT_DIR=/tmp/dinero-v8.0.4
DINEROD="${DINEROD:-$ROOT_DIR/build-release-804/dinerod}"
DATA_DIR="/tmp/dinero_repro_$$"
LOG_FILE="${DATA_DIR}.log"
PID=""
RPC_PORT=$((41000 + RANDOM % 1000)); P2P_PORT=$((RPC_PORT+1)); WALLET_PORT=$((RPC_PORT+2))

cleanup(){ [[ -n "$PID" ]] && kill "$PID" 2>/dev/null; pkill -f "dinerod.*${DATA_DIR}" 2>/dev/null; rm -rf "$DATA_DIR" "$LOG_FILE"; }
trap cleanup EXIT
cookie(){ tr -d '\n' < "${DATA_DIR}/regtest/.cookie" 2>/dev/null || tr -d '\n' < "${DATA_DIR}/.cookie" 2>/dev/null; }
rpc(){ local c; c="$(cookie)"; [ -n "$c" ] || return 1; curl -s --user "$c" -H 'Content-Type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" "http://127.0.0.1:${RPC_PORT}/" </dev/null; }
start(){ mkdir -p "$DATA_DIR"; "$DINEROD" --regtest --datadir="$DATA_DIR" --rpcport="$RPC_PORT" --port="$P2P_PORT" \
  --wallet-socket-port="$WALLET_PORT" --listen=0 --utreexo=1 --p2p.offline=1 >"$LOG_FILE" 2>&1 & PID=$!; }
wait_rpc(){ for _ in $(seq 1 90); do [[ -n "$PID" ]] && ! kill -0 "$PID" 2>/dev/null && return 1
  rpc getblockcount '[]' | jq -e '.result>=0' >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }

echo "### start regtest node 1"
start; wait_rpc || { echo "FAIL: no RPC"; tail -40 "$LOG_FILE"; exit 1; }
MINER=$(rpc wallet.getnewaddress '["taproot","m"]' | jq -r '.result.address // .result')
rpc generatetoaddress "[101,\"$MINER\"]" >/dev/null
RCPT=$(rpc wallet.getshieldedaddress '{"account":1,"j":0}' | jq -r '.result.address')
echo "recipient: ${RCPT:0:20}…"

echo "### shield note A (1 DIN), mine"
rpc wallet.shield '[1.0]' >/dev/null
rpc generatetoaddress "[1,\"$MINER\"]" >/dev/null
NOTE_A_LEAF0=$(rpc wallet.listshielded '{}' | jq -r '.result.notes[0].leaf_index // .result[0].leaf_index')
NOTE_A_COMMIT=$(rpc wallet.listshielded '{}' | jq -r '.result.notes[0].commitment_hex // .result[0].commitment_hex')
TREE0=$(rpc wallet.shieldedbalance '[]' | jq -r '.result.tree_size')
echo "note A: leaf=$NOTE_A_LEAF0 tree_size=$TREE0 commit=${NOTE_A_COMMIT:0:16}"

echo ""
echo "########## TEST #322: explicit too-low fee (20000) must be rejected cleanly, NO stuck state ##########"
R=$(rpc wallet.transfer "{\"amount_una\":70000000,\"address\":\"$RCPT\",\"fee_una\":20000}")
ERR=$(jq -r '.error.message? // .error // .result.error.message? // .result.error // empty' <<<"$R"); REQ=$(jq -r '.result.required_fee_una' <<<"$R"); VS=$(jq -r '.result.vsize' <<<"$R")
echo "result: error=$ERR required_fee_una=$REQ vsize=$VS"
SPENT_AFTER=$(rpc wallet.listshielded '{}' | jq -r '[.result.notes[]?|select(.commitment_hex==$c)][0].spent' --arg c "$NOTE_A_COMMIT")
echo "note A spent after rejected low-fee send: $SPENT_AFTER (must be false = no stuck state)"
[[ "$ERR" == "fee_too_low" ]] && echo "✅ #322: clean fee_too_low" || echo "❌ #322: expected fee_too_low, got '$ERR'"
[[ "$SPENT_AFTER" == "false" ]] && echo "✅ #322: note NOT stranded" || echo "❌ #322: note stranded (spent=$SPENT_AFTER)"

echo ""
echo "########## TEST #324: leaf_index stability across tree-growth + restart ##########"
echo "### grow the tree: shield 3 more notes (B,C,D), mine"
rpc wallet.shield '[1.0]' >/dev/null; rpc wallet.shield '[1.0]' >/dev/null; rpc wallet.shield '[1.0]' >/dev/null
rpc generatetoaddress "[1,\"$MINER\"]" >/dev/null
TREE1=$(rpc wallet.shieldedbalance '[]' | jq -r '.result.tree_size')
LEAF_PRE=$(rpc wallet.listshielded '{}' | jq -r --arg c "$NOTE_A_COMMIT" '[.result.notes[]?|select(.commitment_hex==$c)][0].leaf_index')
echo "after growth: tree_size=$TREE1 ; note A leaf (pre-restart)=$LEAF_PRE (was $NOTE_A_LEAF0)"

echo "### RESTART the daemon (triggers rescan)"
kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null; PID=""
start; wait_rpc || { echo "FAIL: no RPC after restart"; tail -40 "$LOG_FILE"; exit 1; }
LEAF_POST=$(rpc wallet.listshielded '{}' | jq -r --arg c "$NOTE_A_COMMIT" '[.result.notes[]?|select(.commitment_hex==$c)][0].leaf_index')
TREE2=$(rpc wallet.shieldedbalance '[]' | jq -r '.result.tree_size')
echo "after restart: tree_size=$TREE2 ; note A leaf (post-restart)=$LEAF_POST"
echo ""
echo "=== VERDICT #324 ==="
echo "note A leaf: mined=$NOTE_A_LEAF0  pre-restart=$LEAF_PRE  post-restart=$LEAF_POST"
if [[ "$NOTE_A_LEAF0" == "$LEAF_PRE" && "$LEAF_PRE" == "$LEAF_POST" ]]; then
  echo "✅ #324: leaf_index STABLE — no drift (cannot repro on clean wallet)"
else
  echo "❌ #324: leaf_index DRIFTED ($NOTE_A_LEAF0 → $LEAF_PRE → $LEAF_POST) — BUG REPRODUCED"
fi

echo ""
echo "########## DECISIVE: can the drifted note still be SPENT after restart? (auto-sized fee) ##########"
SR=$(rpc wallet.transfer "{\"amount_una\":50000000,\"address\":\"$RCPT\"}")
SR_ERR=$(jq -r '.error.message? // .error // .result.error.message? // .result.error // "none"' <<<"$SR"); SR_STATUS=$(jq -r '.result.status // "none"' <<<"$SR")
SR_TXID=$(jq -r '.result.txid // empty' <<<"$SR")
echo "post-restart spend: status=$SR_STATUS error=$SR_ERR txid=${SR_TXID:0:16}"
if [[ "$SR_STATUS" == "transferred" || -n "$SR_TXID" ]]; then
  MP=$(rpc getrawmempool '[]')
  IN_MP=$(jq -e --arg t "$SR_TXID" '.result|index($t)!=null' <<<"$MP" >/dev/null 2>&1 && echo yes || echo no)
  echo "in mempool: $IN_MP"
  [[ "$IN_MP" == "yes" ]] && echo "✅ SPEND WORKS post-restart — drift is DISPLAY-ONLY (lower severity)" \
                          || echo "❌ spend built but NOT in mempool — drift may break broadcast"
else
  echo "❌ SPEND FAILED post-restart (error=$SR_ERR) — #324 BREAKS SPENDING (critical: drifted leaf → invalid proof)"
fi
