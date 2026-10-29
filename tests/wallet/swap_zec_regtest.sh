#!/usr/bin/env bash
# Zebra regtest check of the ZEC swap leg (milestone 4 of
# docs/design/din-zec-atomic-swaps.md): the per-swap deposit (short, split and
# overpaid, cancel), the P2SH HTLC claim and refund as Zebra's mempool and
# blocks judge them, the watcher (funding, mempool reveal, reorg, refund), the
# never-expiring tower rung, and the network-upgrade guard.
#   usage: swap_zec_regtest.sh <swap_zec_regtest_tool> <zebrad>
set -euo pipefail
. "$(cd "$(dirname "$0")" && pwd)/swap_regtest_lib.sh"
TOOL=${1:?tool}; ZEBRAD=${2:?zebrad}
REGTEST_GENESIS=029f11d80ef9765602235e1bc9727e3eb6ba20839319f761fee920d63401e327
UPGRADE_AT=700  # NU6.2 on this regtest chain, after every other scenario
DIR=$(mktemp -d); BASE=$(swap_free_base 1); PORT=$BASE
cleanup() {
  exec 3>&- 4<&- 2>/dev/null || true
  [[ -n "${WPID:-}" ]] && kill "$WPID" 2>/dev/null || true
  [[ -n "${ZPID:-}" ]] && { kill "$ZPID" 2>/dev/null || true; wait "$ZPID" 2>/dev/null || true; }
  rm -rf "$DIR"
}
trap cleanup EXIT

MINER=$("$TOOL" addr 7); DEPOSIT=$("$TOOL" addr 8); BOB=$("$TOOL" addr 9)
cat >"$DIR/zebrad.toml" <<EOF
[network]
network = "Regtest"
listen_addr = "127.0.0.1:$((BASE + 1))"
cache_dir = false
[network.testnet_parameters.activation_heights]
NU5 = 1
NU6 = 1
"NU6.1" = 1
"NU6.2" = $UPGRADE_AT
[mining]
miner_address = "$MINER"
[rpc]
listen_addr = "127.0.0.1:$PORT"
enable_cookie_auth = false
[state]
ephemeral = true
cache_dir = "$DIR/state"
[tracing]
use_color = false
EOF
"$ZEBRAD" -c "$DIR/zebrad.toml" start >"$DIR/zebrad.log" 2>&1 & ZPID=$!

rpc() {  # <method> <json params>  -> result (compact JSON)
  curl -s --max-time 120 -H 'content-type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"$1\",\"params\":$2}" "http://127.0.0.1:$PORT" |
    python3 -c 'import sys,json; d=json.load(sys.stdin); r=d.get("result"); print(r if isinstance(r,str) else json.dumps(r))'
}
for _ in $(seq 1 60); do rpc getblockcount '[]' >/dev/null 2>&1 && break; sleep 0.5; done
# Zebra names regtest "test", like testnet: the genesis block tells them apart.
[[ "$(rpc getblockhash '[0]')" == "$REGTEST_GENESIS" ]] ||
  { echo "ABORT: the node on port $PORT is not a fresh Zebra regtest"; exit 1; }
mine() { rpc generate "[${1:-1}]" >/dev/null; }
tip() { rpc getblockcount '[]'; }
mtp() { "$TOOL" chain "$PORT" | sed 's/.*mtp=\([0-9]*\).*/\1/'; }
mine_past() {  # <unix>: regtest block times advance about 1 s per 5 blocks
  while (( $(mtp) < $1 )); do mine 25; done
}

fail=0
expect() {  # <label> <got> <expected substring ...>
  local label=$1 got=$2 ok=1; shift 2
  for want in "$@"; do [[ " $got " == *"$want"* ]] || ok=0; done
  printf '%-44s %s %s\n' "$label" "$([[ $ok == 1 ]] && echo OK || echo FAIL)" "$got"
  [[ $ok == 1 ]] || fail=1
}
check() {  # <label> <key=value ...>  (one observation from the running watcher)
  local label=$1; shift
  echo o >&3; local line; read -r line <&4
  expect "$label" "$line" "${@/#/ }"
}
start_watcher() {  # <scan_from> <lock>
  rm -f "$DIR/in" "$DIR/out"; mkfifo "$DIR/in" "$DIR/out"
  "$TOOL" watch "$PORT" "$1" "$2" "$AMOUNT" interactive <"$DIR/in" >"$DIR/out" & WPID=$!
  exec 3>"$DIR/in" 4<"$DIR/out"
}
stop_watcher() { exec 3>&- 4<&-; wait "$WPID" 2>/dev/null || true; WPID=; }

mine 110  # coinbase matures after 100 blocks
AMOUNT=1000000; NEED=1020000  # the swap amount + 2 x the conventional fee

# --- Deposit ----------------------------------------------------------------
"$TOOL" pay "$PORT" 7 "$DEPOSIT" 400000 >/dev/null; mine 10
expect "deposit: short payment confirmed"      "$("$TOOL" deposit "$PORT" 10)" "confirmed=400000" "coins=1"
expect "deposit: short -> funding refused"     "$("$TOOL" fund "$PORT" $AMOUNT 1300000000 "$BOB" 2>&1 || true)" "short by 620000 zat"
"$TOOL" pay "$PORT" 7 "$DEPOSIT" 700000 >/dev/null; mine 9
expect "deposit: 2nd payment at 9 confs"       "$("$TOOL" deposit "$PORT" 10)" "confirmed=400000" "coins=1"
mine 1
expect "deposit: split + overpaid confirmed"   "$("$TOOL" deposit "$PORT" 10)" "confirmed=1100000" "coins=2"
expect "deposit: cancel sweeps it back"        "$("$TOOL" sweep "$PORT" "$BOB")" "accepted"
mine 1
expect "deposit: empty after cancel"           "$("$TOOL" deposit "$PORT" 1)" "confirmed=0" "coins=0"

# --- Claim path, watched ----------------------------------------------------
"$TOOL" pay "$PORT" 7 "$DEPOSIT" $NEED >/dev/null; mine 10
expect "deposit: exact payment confirmed"      "$("$TOOL" deposit "$PORT" 10)" "confirmed=$NEED"
LOCK=$(( $(mtp) + 15 ))
START=$(( $(tip) + 1 ))
start_watcher "$START" "$LOCK"
check "watch: before funding"                  ok=1 seen=0
"$TOOL" pay "$PORT" 7 "$("$TOOL" htlcaddr "$LOCK")" 50000 >/dev/null; mine 1  # anyone can pay the script
check "watch: decoy payment to the HTLC ignored" ok=1 seen=0
read -r T1 V1 <<<"$("$TOOL" fund "$PORT" $AMOUNT "$LOCK" "$BOB")"
check "watch: funding in mempool only"         ok=1 seen=0
mine 1; FUND_HASH=$(rpc getbestblockhash '[]')
check "watch: funding 1 conf"                  ok=1 seen=1 confs=1 value=$AMOUNT spent=0 "fund=$T1"
expect "zebra: claim with a wrong secret"      "$("$TOOL" send "$PORT" "$("$TOOL" spend "$PORT" badclaim "$T1" "$V1" $AMOUNT "$LOCK" $(( $(tip) + 40 )))")" "rejected"
expect "zebra: refund before the lock"         "$("$TOOL" send "$PORT" "$("$TOOL" spend "$PORT" refund "$T1" "$V1" $AMOUNT "$LOCK" $(( $(tip) + 40 )))")" "rejected"
expect "zebra: claim"                          "$("$TOOL" send "$PORT" "$("$TOOL" spend "$PORT" claim "$T1" "$V1" $AMOUNT "$LOCK" $(( $(tip) + 40 )))")" "accepted"
check "watch: claim in mempool (reveal)"       ok=1 spent=1 claim=1 spendconfs=0 secret=1
mine 1
check "watch: claim 1 conf"                    ok=1 confs=2 spent=1 claim=1 spendconfs=1 secret=1
rpc invalidateblock "[\"$FUND_HASH\"]" >/dev/null
check "watch: reorg drops the funding"         ok=1 seen=0 spent=0
rpc reconsiderblock "[\"$FUND_HASH\"]" >/dev/null
check "watch: reorg restored"                  ok=1 seen=1 confs=2 spent=1 claim=1 spendconfs=1 secret=1
stop_watcher

# --- Refund path: the tower rung never expires; a wallet refund does ---------
"$TOOL" pay "$PORT" 7 "$DEPOSIT" $NEED >/dev/null; mine 10
LOCK2=$(( $(mtp) + 15 ))
START2=$(( $(tip) + 1 ))
read -r T2 V2 <<<"$("$TOOL" fund "$PORT" $AMOUNT "$LOCK2" "$BOB")"; mine 1
RUNG=$("$TOOL" spend "$PORT" refund "$T2" "$V2" $AMOUNT "$LOCK2" 0)                    # nExpiryHeight 0
EXPIRING=$("$TOOL" spend "$PORT" refund "$T2" "$V2" $AMOUNT "$LOCK2" $(( $(tip) + 40 )))
EARLY=$("$TOOL" spend "$PORT" early_refund "$T2" "$V2" $AMOUNT "$LOCK2" 0)
mine_past "$LOCK2"; mine 45  # past the lock, and 40+ blocks after signing
expect "zebra: refund below the CLTV lock"     "$("$TOOL" send "$PORT" "$EARLY")" "rejected"
expect "zebra: refund past its expiry height"  "$("$TOOL" send "$PORT" "$EXPIRING")" "rejected"
expect "zebra: tower rung, long after signing" "$("$TOOL" send "$PORT" "$RUNG")" "accepted"
start_watcher "$START2" "$LOCK2"
check "watch: refund in mempool"               ok=1 seen=1 spent=1 claim=0 spendconfs=0 secret=0
mine 1
check "watch: refund 1 conf"                   ok=1 spent=1 claim=0 spendconfs=1 secret=0
stop_watcher

# --- Network upgrade --------------------------------------------------------
expect "guard: 1 h window, upgrade far away"   "$("$TOOL" guard "$PORT" 3600)" "upgrade=none"
expect "guard: window reaching NU6.2"          "$("$TOOL" guard "$PORT" $(( (UPGRADE_AT - $(tip)) * 75 )))" "upgrade=NU6.2@$UPGRADE_AT"
"$TOOL" pay "$PORT" 7 "$DEPOSIT" $NEED >/dev/null; mine 10
LOCK3=$(( $(mtp) + 15 ))
read -r T3 V3 <<<"$("$TOOL" fund "$PORT" $AMOUNT "$LOCK3" "$BOB")"; mine 1
OLD_RUNG=$("$TOOL" spend "$PORT" refund "$T3" "$V3" $AMOUNT "$LOCK3" 0)  # signed under NU6.1
mine $(( UPGRADE_AT - $(tip) + 1 )); mine_past "$LOCK3"
expect "zebra: branch id is NU6.2 now"         "$("$TOOL" chain "$PORT")" "branch=5437f330"
expect "zebra: rung signed before the upgrade" "$("$TOOL" send "$PORT" "$OLD_RUNG")" "rejected"
expect "zebra: refund re-signed after it"      "$("$TOOL" send "$PORT" "$("$TOOL" spend "$PORT" refund "$T3" "$V3" $AMOUNT "$LOCK3" 0)")" "accepted"

[[ $fail -eq 0 ]] && echo "ZEC REGTEST: PASS" || { echo "ZEC REGTEST: FAIL"; exit 1; }
