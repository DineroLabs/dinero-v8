#!/usr/bin/env bash
# Bitcoin Core regtest check of BtcWatcher: funding, depth, mempool reveal of
# the secret, confirmed claim, a reorg that drops and restores the funding
# block, and a refund spend — all observed by one long-lived watcher.
#   usage: swap_btc_watch_regtest.sh <swap_btc_watch_tool> <swap_btc_regtest_tool>
set -euo pipefail
WATCH=${1:?watch tool}; TOOL=${2:?tx tool}
LOCK=1800000000
DIR=$(mktemp -d); RPCPORT=$((20000 + RANDOM % 20000)); U=swap; P=swappass
CLI=(bitcoin-cli -regtest -datadir="$DIR" -rpcport=$RPCPORT -rpcuser=$U -rpcpassword=$P)
cleanup() { exec 3>&- 4<&- 2>/dev/null || true; [[ -n "${WPID:-}" ]] && kill "$WPID" 2>/dev/null || true; "${CLI[@]}" stop >/dev/null 2>&1 || true; sleep 1; rm -rf "$DIR"; }
trap cleanup EXIT
bitcoind -regtest -datadir="$DIR" -listen=0 -rpcport=$RPCPORT -rpcuser=$U -rpcpassword=$P -daemon \
  -fallbackfee=0.0002 -txindex=1 >/dev/null
for _ in $(seq 1 60); do "${CLI[@]}" getblockchaininfo >/dev/null 2>&1 && break; sleep 0.5; done
"${CLI[@]}" createwallet test >/dev/null
MINER=$("${CLI[@]}" getnewaddress)
mine() { "${CLI[@]}" generatetoaddress "${1:-1}" "$MINER" >/dev/null; }
mine 101
read -r WSCRIPT SPK <<<"$("$TOOL" script)"
HTLC_ADDR=$("${CLI[@]}" decodescript "$WSCRIPT" | python3 -c 'import sys,json;print(json.load(sys.stdin)["segwit"]["address"])')
PAYOUT=$("${CLI[@]}" getaddressinfo "$("${CLI[@]}" getnewaddress)" | python3 -c 'import sys,json;print(json.load(sys.stdin)["scriptPubKey"])')
vout_of() { "${CLI[@]}" getrawtransaction "$1" true | python3 -c "import sys,json;print([o['n'] for o in json.load(sys.stdin)['vout'] if o['scriptPubKey']['hex']=='$SPK'][0])"; }

fail=0
check() {  # <label> <expected key=value ...>  (reads one observation)
  local label=$1; shift
  echo o >&3; local line; read -r line <&4
  local ok=1; for kv in "$@"; do [[ " $line " == *" $kv "* ]] || ok=0; done
  printf '%-34s %s %s\n' "$label" "$([[ $ok == 1 ]] && echo OK || echo FAIL)" "$line"
  [[ $ok == 1 ]] || fail=1
}

START=$(( $("${CLI[@]}" getblockcount) + 1 ))
# One long-lived watcher fed through named pipes (macOS /bin/bash 3.2 has no coproc).
start_watcher() {  # <scan_from_height>
  rm -f "$DIR/in" "$DIR/out"; mkfifo "$DIR/in" "$DIR/out"
  "$WATCH" "$RPCPORT" "$U" "$P" "$1" interactive <"$DIR/in" >"$DIR/out" & WPID=$!
  exec 3>"$DIR/in" 4<"$DIR/out"
}
stop_watcher() { exec 3>&- 4<&-; wait "$WPID" 2>/dev/null || true; WPID=; }
start_watcher "$START"
check "before funding"            ok=1 seen=0
T1=$("${CLI[@]}" sendtoaddress "$HTLC_ADDR" 0.01)
check "funding in mempool only"   ok=1 seen=0
mine 1; FUND_HASH=$("${CLI[@]}" getbestblockhash)
check "funding 1 conf"            ok=1 seen=1 confs=1 value=1000000 spent=0
CLAIM=$("$TOOL" claim "$T1" "$(vout_of "$T1")" 1000000 "$PAYOUT" 1000)
"${CLI[@]}" sendrawtransaction "$CLAIM" >/dev/null
check "claim in mempool (reveal)" ok=1 spent=1 claim=1 spendconfs=0 secret=1
mine 1
check "claim 1 conf"              ok=1 confs=2 spent=1 claim=1 spendconfs=1 secret=1
"${CLI[@]}" invalidateblock "$FUND_HASH"
check "reorg drops funding"       ok=1 seen=0 spent=0
"${CLI[@]}" reconsiderblock "$FUND_HASH"
check "reorg restored"            ok=1 seen=1 confs=2 spent=1 claim=1 spendconfs=1 secret=1
stop_watcher

# Refund path with a second HTLC funding, watched from after the first.
START2=$(( $("${CLI[@]}" getblockcount) + 1 ))
T2=$("${CLI[@]}" sendtoaddress "$HTLC_ADDR" 0.01); mine 1
REFUND=$("$TOOL" refund "$T2" "$(vout_of "$T2")" 1000000 "$PAYOUT" 1000)
"${CLI[@]}" setmocktime $((LOCK + 3600)); mine 12
"${CLI[@]}" sendrawtransaction "$REFUND" >/dev/null
start_watcher "$START2"
check "refund in mempool"         ok=1 seen=1 spent=1 claim=0 spendconfs=0 secret=0
mine 1
check "refund 1 conf"             ok=1 spent=1 claim=0 spendconfs=1 secret=0

[[ $fail -eq 0 ]] && echo "BTC WATCHER REGTEST: PASS" || { echo "BTC WATCHER REGTEST: FAIL"; exit 1; }
