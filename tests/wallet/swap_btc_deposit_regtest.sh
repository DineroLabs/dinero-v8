#!/usr/bin/env bash
# Deposit transactions judged by a real Bitcoin Core (regtest): the deposit
# address receives ordinary payments, and the lock and the sweep built from
# them are accepted, pay the fee rate asked for, and confirm.
#   usage: swap_btc_deposit_regtest.sh <swap_btc_deposit_tool>
set -euo pipefail
TOOL=${1:?swap_btc_deposit_tool}
command -v bitcoind >/dev/null || { echo "SKIP: bitcoind not installed"; exit 77; }
DIR=$(mktemp -d "${TMPDIR:-/tmp}/swap-btc-deposit.XXXXXX")
RPC=$(( 23000 + RANDOM % 20000 ))
BCLI=(bitcoin-cli -regtest -datadir="$DIR" -rpcport=$RPC -rpcuser=test -rpcpassword=test)
cleanup() { "${BCLI[@]}" stop >/dev/null 2>&1 || true; sleep 1; rm -rf "$DIR"; }
trap cleanup EXIT
FAILED=0
check() { if eval "$2"; then echo "  OK   $1"; else echo "  FAIL $1"; FAILED=1; fi; }
j() { python3 -c "import sys,json; d=json.load(sys.stdin); print($1)"; }

bitcoind -regtest -datadir="$DIR" -listen=0 -rpcport=$RPC -rpcuser=test -rpcpassword=test -daemon -fallbackfee=0.0002 >/dev/null
for _ in $(seq 1 60); do "${BCLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done
[[ "$("${BCLI[@]}" getblockchaininfo | j 'd["chain"]')" == regtest ]] || { echo "ABORT: not a regtest bitcoind"; exit 1; }
"${BCLI[@]}" createwallet w >/dev/null
MINER=$("${BCLI[@]}" getnewaddress)
"${BCLI[@]}" generatetoaddress 101 "$MINER" >/dev/null

KEY=$(printf '%064x' 424242)
ADDR=$("$TOOL" address "$KEY" bcrt)
check "the deposit address is one Bitcoin Core recognises as P2WPKH ($ADDR)" \
  '[[ "$("${BCLI[@]}" getaddressinfo "$ADDR" | j "str(d[\"iswitness\"])+str(d[\"witness_version\"])+str(len(d[\"witness_program\"]))")" == True040 ]]'

# Three ordinary payments to it, as from a wallet or an exchange, and one dust payment.
coin() {  # <btc amount> -> txid:vout:sats
  local txid; txid=$("${BCLI[@]}" sendtoaddress "$ADDR" "$1")
  "${BCLI[@]}" gettransaction "$txid" true true | j "next('%s:%d:%d' % (d['txid'], o['n'], round(o['value']*1e8)) for o in d['decoded']['vout'] if o['scriptPubKey'].get('address') == '$ADDR')"
}
C1=$(coin 0.00150000); C2=$(coin 0.00090000); C3=$(coin 0.00040000); C4=$(coin 0.00001500)
"${BCLI[@]}" generatetoaddress 1 "$MINER" >/dev/null
LOCK_ADDR=$("${BCLI[@]}" getnewaddress "" bech32m); LOCK_SPK=$("${BCLI[@]}" getaddressinfo "$LOCK_ADDR" | j 'd["scriptPubKey"]')
CHANGE_ADDR=$("${BCLI[@]}" getnewaddress "" legacy); CHANGE_SPK=$("${BCLI[@]}" getaddressinfo "$CHANGE_ADDR" | j 'd["scriptPubKey"]')

echo "=== the lock, from two of the coins ==="
read -r RAW TXID FEE VSIZE INPUTS <<<"$("$TOOL" lock "$KEY" "$C4,$C3,$C1,$C2" "$LOCK_SPK" 200000 "$CHANGE_SPK" 3.5 5000)"
check "it uses the two largest coins and not the dust ($INPUTS inputs)" '[[ "$INPUTS" == 2 ]]'
ACCEPT=$("${BCLI[@]}" testmempoolaccept "[\"$RAW\"]")
check "Bitcoin Core would accept it" '[[ "$(echo "$ACCEPT" | j "d[0][\"allowed\"]")" == True ]]'
check "under the id we computed before sending" '[[ "$(echo "$ACCEPT" | j "d[0][\"txid\"]")" == "$TXID" ]]'
REAL_VSIZE=$(echo "$ACCEPT" | j 'd[0]["vsize"]'); REAL_FEE=$(echo "$ACCEPT" | j 'round(d[0]["fees"]["base"]*1e8)')
check "the fee is the one we said ($FEE sat)" '[[ "$REAL_FEE" == "$FEE" ]]'
check "and pays at least 3.5 sat/vB for its real size ($REAL_FEE sat for $REAL_VSIZE vB; we assumed $VSIZE)" \
  'python3 -c "import sys; sys.exit(0 if $REAL_FEE >= 3.5 * $REAL_VSIZE and $REAL_VSIZE <= $VSIZE <= $REAL_VSIZE + 2 else 1)"'
check "it does not signal replacement" '[[ "$("${BCLI[@]}" decoderawtransaction "$RAW" | j "min(i[\"sequence\"] for i in d[\"vin\"])")" == 4294967295 ]]'
check "it is sent" '[[ "$("${BCLI[@]}" sendrawtransaction "$RAW")" == "$TXID" ]]'
"${BCLI[@]}" generatetoaddress 1 "$MINER" >/dev/null
OUT0=$("${BCLI[@]}" gettxout "$TXID" 0); OUT1=$("${BCLI[@]}" gettxout "$TXID" 1)
check "output 0 is the lock: exactly 200,000 sat to the lock script" \
  '[[ "$(echo "$OUT0" | j "str(round(d[\"value\"]*1e8))+d[\"scriptPubKey\"][\"hex\"]")" == "200000$LOCK_SPK" ]]'
check "output 1 is the change, to a legacy address" \
  '[[ "$(echo "$OUT1" | j "d[\"scriptPubKey\"][\"hex\"]")" == "$CHANGE_SPK" && "$(echo "$OUT1" | j "round(d[\"value\"]*1e8)")" == $((150000 + 90000 - 200000 - FEE)) ]]'

echo "=== the sweep: what is left goes back ==="
read -r RAW TXID FEE VSIZE INPUTS <<<"$("$TOOL" sweep "$KEY" "$C3,$C4" "$CHANGE_SPK" 2 5000)"
check "one coin worth sending back ($INPUTS input)" '[[ "$INPUTS" == 1 ]]'
check "Bitcoin Core accepts it" '[[ "$("${BCLI[@]}" testmempoolaccept "[\"$RAW\"]" | j "d[0][\"allowed\"]")" == True ]]'
check "it is sent" '[[ "$("${BCLI[@]}" sendrawtransaction "$RAW")" == "$TXID" ]]'
"${BCLI[@]}" generatetoaddress 1 "$MINER" >/dev/null
check "40,000 sat less the fee arrives" '[[ "$("${BCLI[@]}" gettxout "$TXID" 0 | j "round(d[\"value\"]*1e8)")" == $((40000 - FEE)) ]]'

echo "=== refusals ==="
check "a coin already spent makes the lock invalid for Bitcoin Core, as it should" \
  '[[ "$("${BCLI[@]}" testmempoolaccept "[\"$("$TOOL" lock "$KEY" "$C1" "$LOCK_SPK" 100000 "$CHANGE_SPK" 2 5000 | cut -d" " -f1)\"]" | j "d[0][\"allowed\"]")" == False ]]'
check "too little deposited is said, not built" '[[ "$("$TOOL" lock "$KEY" "$C3" "$LOCK_SPK" 200000 "$CHANGE_SPK" 2 5000 || true)" == "error deposit is short by "* ]]'
WRONG=$(printf '%064x' 424243)
check "a transaction signed with another key is refused by Bitcoin Core" \
  '[[ "$("${BCLI[@]}" testmempoolaccept "[\"$("$TOOL" lock "$WRONG" "$C4" "$LOCK_SPK" 600 "$CHANGE_SPK" 1 5000 | cut -d" " -f1)\"]" 2>&1 | j "d[0][\"allowed\"]" 2>/dev/null || echo False)" == False ]]'

if [[ $FAILED == 0 ]]; then echo "SWAP BTC DEPOSIT REGTEST: PASS"; else echo "SWAP BTC DEPOSIT REGTEST: FAIL"; exit 1; fi
