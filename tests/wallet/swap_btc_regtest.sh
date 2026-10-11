#!/usr/bin/env bash
# Bitcoin Core regtest check for the swap's BTC side: a real bitcoind must
# accept our HTLC claim, reject the refund before its timestamp lock, and
# accept the refund once median time past reaches the lock.
#   usage: swap_btc_regtest.sh <path-to-swap_btc_regtest_tool>
set -euo pipefail
TOOL=${1:?path to swap_btc_regtest_tool}
LOCK=1800000000
DIR=$(mktemp -d)
RPCPORT=$((20000 + RANDOM % 20000))
CLI=(bitcoin-cli -regtest -datadir="$DIR" -rpcport=$RPCPORT)
cleanup() { "${CLI[@]}" stop >/dev/null 2>&1 || true; sleep 1; rm -rf "$DIR"; }
trap cleanup EXIT

# No P2P: -listen=0 (Bitcoin Core also binds an onion port at P2P port + 1).
bitcoind -regtest -datadir="$DIR" -listen=0 -rpcport=$RPCPORT -daemon -fallbackfee=0.0002 \
  -txindex=1 >/dev/null
for _ in $(seq 1 60); do "${CLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done
"${CLI[@]}" createwallet test >/dev/null
MINER=$("${CLI[@]}" getnewaddress)
"${CLI[@]}" generatetoaddress 101 "$MINER" >/dev/null

read -r WSCRIPT SPK <<<"$("$TOOL" script)"
HTLC_ADDR=$("${CLI[@]}" decodescript "$WSCRIPT" | python3 -c 'import sys,json;print(json.load(sys.stdin)["segwit"]["address"])')
PAYOUT_SPK=$("${CLI[@]}" getaddressinfo "$("${CLI[@]}" getnewaddress)" | python3 -c 'import sys,json;print(json.load(sys.stdin)["scriptPubKey"])')

fund() {  # -> "<txid> <vout>"
  local txid; txid=$("${CLI[@]}" sendtoaddress "$HTLC_ADDR" 0.01)
  "${CLI[@]}" generatetoaddress 1 "$MINER" >/dev/null
  local vout; vout=$("${CLI[@]}" getrawtransaction "$txid" true | python3 -c "
import sys,json; t=json.load(sys.stdin)
print([o['n'] for o in t['vout'] if o['scriptPubKey']['hex']=='$SPK'][0])")
  echo "$txid $vout"
}
accepts() {  # tx hex -> prints allowed + reason
  "${CLI[@]}" testmempoolaccept "[\"$1\"]" | python3 -c '
import sys,json; r=json.load(sys.stdin)[0]; print(r["allowed"], r.get("reject-reason",""))'
}

fail=0
read -r T1 V1 <<<"$(fund)"
CLAIM=$("$TOOL" claim "$T1" "$V1" 1000000 "$PAYOUT_SPK" 1000)
r=$(accepts "$CLAIM"); echo "claim:              $r"; [[ $r == True* ]] || fail=1

read -r T2 V2 <<<"$(fund)"
REFUND=$("$TOOL" refund "$T2" "$V2" 1000000 "$PAYOUT_SPK" 1000)
r=$(accepts "$REFUND"); echo "refund before lock: $r"; [[ $r == False*non-final* ]] || fail=1

# Move median time past beyond the lock: 11+ blocks with mock time after it.
"${CLI[@]}" setmocktime $((LOCK + 3600))
for _ in $(seq 1 12); do "${CLI[@]}" generatetoaddress 1 "$MINER" >/dev/null; done
r=$(accepts "$REFUND"); echo "refund after lock:  $r"; [[ $r == True* ]] || fail=1

# Mine the claim for real and confirm it spends the HTLC.
"${CLI[@]}" sendrawtransaction "$CLAIM" >/dev/null
"${CLI[@]}" generatetoaddress 1 "$MINER" >/dev/null
CONF=$("${CLI[@]}" getrawtransaction "$("${CLI[@]}" decoderawtransaction "$CLAIM" | python3 -c 'import sys,json;print(json.load(sys.stdin)["txid"])')" true | python3 -c 'import sys,json;print(json.load(sys.stdin).get("confirmations",0))')
echo "claim confirmations: $CONF"; [[ $CONF -ge 1 ]] || fail=1

[[ $fail -eq 0 ]] && echo "BTC REGTEST: PASS" || { echo "BTC REGTEST: FAIL"; exit 1; }
