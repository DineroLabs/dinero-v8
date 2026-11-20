#!/usr/bin/env bash
# The light Bitcoin P2P client against a real Bitcoin Core (regtest): header
# sync over P2P only, a restart from the saved header file, a reorg, and a
# transaction handed to the node with no RPC involved.
#   usage: swap_btc_p2p_regtest.sh <swap_btc_p2p_tool>
set -euo pipefail
TOOL=${1:?swap_btc_p2p_tool}
command -v bitcoind >/dev/null || { echo "SKIP: bitcoind not installed"; exit 77; }
DIR=$(mktemp -d "${TMPDIR:-/tmp}/swap-btc-p2p.XXXXXX")
RPC=$(( 20000 + RANDOM % 20000 )); P2P=$(( RPC + 1 ))
BCLI=(bitcoin-cli -regtest -datadir="$DIR/btc" -rpcport=$RPC -rpcuser=test -rpcpassword=test)
cleanup() { "${BCLI[@]}" stop >/dev/null 2>&1 || true; sleep 1; [[ -n "${KEEP_LOGS:-}" ]] && cp -r "$DIR" "$KEEP_LOGS"/ 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT
FAILED=0
check() { if eval "$2"; then echo "  OK   $1"; else echo "  FAIL $1"; FAILED=1; fi; }

mkdir -p "$DIR/btc"
bitcoind -regtest -datadir="$DIR/btc" -listen=1 -bind=127.0.0.1 -port=$P2P -rpcport=$RPC -rpcuser=test -rpcpassword=test \
  -daemon -fallbackfee=0.0002 -dnsseed=0 -fixedseeds=0 -debug=net -debug=mempoolrej >/dev/null
for _ in $(seq 1 60); do "${BCLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done
[[ "$("${BCLI[@]}" getblockchaininfo | python3 -c 'import sys,json; print(json.load(sys.stdin)["chain"])')" == regtest ]] \
  || { echo "ABORT: not a regtest bitcoind"; exit 1; }
"${BCLI[@]}" createwallet w >/dev/null
ADDR=$("${BCLI[@]}" getnewaddress)
"${BCLI[@]}" generatetoaddress 2500 "$ADDR" >/dev/null   # more than one batch of 2000 headers
STORE="$DIR/headers.bin"

echo "=== header sync over P2P ==="
OUT=$("$TOOL" sync regtest 127.0.0.1 $P2P "$STORE"); echo "  $OUT"
check "synced to the node's tip (2500)" '[[ "$OUT" == "tip 2500 $("${BCLI[@]}" getbestblockhash) accepted 2500 synced 1" ]]'

echo "=== restart from the saved header file ==="
"${BCLI[@]}" generatetoaddress 7 "$ADDR" >/dev/null
OUT=$("$TOOL" sync regtest 127.0.0.1 $P2P "$STORE"); echo "  $OUT"
check "only the 7 new headers were fetched" '[[ "$OUT" == "tip 2507 $("${BCLI[@]}" getbestblockhash) accepted 7 synced 1" ]]'

echo "=== reorg ==="
OLD_TIP=$("${BCLI[@]}" getbestblockhash)
"${BCLI[@]}" invalidateblock "$("${BCLI[@]}" getblockhash 2505)" >/dev/null   # drops 2505..2507
ADDR2=$("${BCLI[@]}" getnewaddress)   # another payout: the same one could re-mine the very block just invalidated
"${BCLI[@]}" generatetoaddress 5 "$ADDR2" >/dev/null                            # new 2505..2509
OUT=$("$TOOL" sync regtest 127.0.0.1 $P2P "$STORE"); echo "  $OUT"
check "followed the node onto the longer branch (2509)" \
  '[[ "$OUT" == "tip 2509 $("${BCLI[@]}" getbestblockhash) accepted 5 synced 1" && "$("${BCLI[@]}" getbestblockhash)" != "$OLD_TIP" ]]'

echo "=== hand a transaction to the network ==="
DEST=$("${BCLI[@]}" getnewaddress)
RAW=$("${BCLI[@]}" createrawtransaction '[]' "{\"$DEST\":0.5}")
FUNDED=$("${BCLI[@]}" fundrawtransaction "$RAW" | python3 -c 'import sys,json; print(json.load(sys.stdin)["hex"])')
SIGNED=$("${BCLI[@]}" signrawtransactionwithwallet "$FUNDED" | python3 -c 'import sys,json; print(json.load(sys.stdin)["hex"])')
TXID=$("${BCLI[@]}" decoderawtransaction "$SIGNED" | python3 -c 'import sys,json; print(json.load(sys.stdin)["txid"])')
check "the node has not seen the transaction" '! "${BCLI[@]}" getmempoolentry "$TXID" >/dev/null 2>&1'
OUT=$("$TOOL" send regtest 127.0.0.1 $P2P "$STORE" "$SIGNED"); echo "  $(echo "$OUT" | tr '\n' ' ')"
check "the node fetched it from us" '[[ "$OUT" == *"sent $TXID"* ]]'
for _ in $(seq 1 20); do "${BCLI[@]}" getmempoolentry "$TXID" >/dev/null 2>&1 && break; sleep 0.5; done
check "it is in the node's mempool" '"${BCLI[@]}" getmempoolentry "$TXID" >/dev/null 2>&1'
"${BCLI[@]}" generatetoaddress 1 "$ADDR" >/dev/null
check "and it confirms" '[[ "$("${BCLI[@]}" gettransaction "$TXID" | python3 -c "import sys,json; print(json.load(sys.stdin)[\"confirmations\"])")" == 1 ]]'

echo "=== refusals ==="
OUT=$("$TOOL" sync regtest 127.0.0.1 $((P2P + 7)) "$DIR/none.bin" || true)
check "a port nobody listens on is reported, not hung on ($OUT)" '[[ "$OUT" == *"error connection refused"* || "$OUT" == *"error timed out connecting"* ]]'
OUT=$("$TOOL" sync main 127.0.0.1 $P2P "$DIR/main.bin" || true)
check "a regtest node is not accepted as mainnet ($(echo "$OUT" | tail -1))" '[[ "$OUT" == *"tip 971712 "* && "$OUT" == *"error "* ]]'

if [[ $FAILED == 0 ]]; then echo "SWAP BTC P2P REGTEST: PASS"; else echo "SWAP BTC P2P REGTEST: FAIL"; exit 1; fi
