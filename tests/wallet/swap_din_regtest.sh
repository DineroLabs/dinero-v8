#!/usr/bin/env bash
# Dinero regtest check of the swap's DIN side: a real dinerod accepts our HTLC
# claim and refund, and one long-lived DinWatcher reports funding, depth, the
# confirmed claim (with secret), a reorg that drops and restores the funding
# block, and a refund.
#   usage: swap_din_regtest.sh <dinerod> <swap_din_tool>
set -euo pipefail
DINEROD=${1:?dinerod}; TOOL=${2:?swap_din_tool}
DIR=$(mktemp -d); RPCPORT=$((20000 + RANDOM % 20000)); P2P=$((RPCPORT + 7))
FUTURE=1800000000
cleanup() { exec 3>&- 4<&- 2>/dev/null || true; [[ -n "${WPID:-}" ]] && kill "$WPID" 2>/dev/null || true
            [[ -n "${NPID:-}" ]] && kill "$NPID" 2>/dev/null || true; sleep 1; rm -rf "$DIR"; }
trap cleanup EXIT
call() {  # <method> <params-json> -> result json (exits on error)
  local body; body=$(python3 -c 'import sys,json;print(json.dumps({"jsonrpc":"2.0","id":1,"method":sys.argv[1],"params":json.loads(sys.argv[2])}))' "$1" "$2")
  local resp; resp=$(curl -s --max-time 60 -u test:test -H 'Content-Type: application/json' -d "$body" "http://127.0.0.1:$RPCPORT/")
  python3 -c 'import sys,json
d=json.loads(sys.argv[1]); e=d.get("error")
if e: sys.stderr.write("RPC error %s: %s\n"%(sys.argv[2],e)); sys.exit(1)
r=d.get("result"); print(r if isinstance(r,str) else json.dumps(r))' "$resp" "$1"
}
"$DINEROD" -regtest -daemon=0 -server -rpcuser=test -rpcpassword=test -rpcport=$RPCPORT -port=$P2P \
  -datadir="$DIR" -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 >"$DIR/node.log" 2>&1 & NPID=$!
for _ in $(seq 1 90); do call getblockcount '[]' >/dev/null 2>&1 && break; sleep 1; done
call wallet.createhd '["swap"]' >/dev/null
call wallet.encrypt '["pw-swap-regtest"]' >/dev/null
call wallet.unlock '["pw-swap-regtest", 3600]' >/dev/null
ADDR=$(call wallet.getnewaddress '["taproot","mine"]' | python3 -c 'import sys,json
s=sys.stdin.read().strip()
try: print(json.loads(s)["address"])
except Exception: print(s)')
PAYOUT_SPK=$(python3 -c "print('5120'+'77'*32)")
mine() { call generatetoaddress "[${1:-1}, \"$ADDR\"]" >/dev/null; }
mine 110

fail=0
start_watcher() {  # <lock>
  rm -f "$DIR/in" "$DIR/out"; mkfifo "$DIR/in" "$DIR/out"
  "$TOOL" watch "$RPCPORT" test test rdin "$1" <"$DIR/in" >"$DIR/out" & WPID=$!
  exec 3>"$DIR/in" 4<"$DIR/out"
}
stop_watcher() { exec 3>&- 4<&-; wait "$WPID" 2>/dev/null || true; WPID=; }
LINE=""
check() {  # <label> <expected key=value ...>
  local label=$1; shift
  echo o >&3; read -r LINE <&4
  local ok=1; for kv in "$@"; do [[ " $LINE " == *" $kv "* ]] || ok=0; done
  printf '%-30s %s %s\n' "$label" "$([[ $ok == 1 ]] && echo OK || echo FAIL)" "$LINE"
  [[ $ok == 1 ]] || fail=1
}
fund_of() { sed -E 's/.* fund=([0-9a-f]+):([0-9]+).*/\1 \2/' <<<"$LINE"; }

# --- Claim path ---------------------------------------------------------
read -r HTLC _ <<<"$("$TOOL" address rdin $FUTURE)"
start_watcher $FUTURE
check "before funding"        ok=1 seen=0
call wallet.sendtoaddress "{\"address\":\"$HTLC\",\"amount\":10.0}" >/dev/null
mine 1
check "funding 1 conf"        ok=1 seen=1 confs=1 value=1000000000 spent=0
read -r FT FV <<<"$(fund_of)"
FUND_BLOCK=$(call getbestblockhash '[]')
CLAIM=$("$TOOL" claim $FUTURE "$FT" "$FV" 1000000000 "$PAYOUT_SPK" 100000)
call sendrawtransaction "[\"$CLAIM\"]" >/dev/null
mine 1
check "claim 1 conf"          ok=1 seen=1 confs=2 spent=1 claim=1 spendconfs=1 secret=1
call invalidateblock "[\"$FUND_BLOCK\"]" >/dev/null
check "reorg drops funding"   ok=1 seen=0 spent=0
call reconsiderblock "[\"$FUND_BLOCK\"]" >/dev/null
check "reorg restored"        ok=1 seen=1 confs=2 spent=1 claim=1 secret=1
stop_watcher

# --- Refund path (lock already in the past, so the refund is valid now) --
PAST=$(( $(date +%s) - 7200 ))
read -r HTLC2 _ <<<"$("$TOOL" address rdin $PAST)"
start_watcher $PAST
call wallet.sendtoaddress "{\"address\":\"$HTLC2\",\"amount\":5.0}" >/dev/null
mine 12
check "refund htlc funded"    ok=1 seen=1 value=500000000 spent=0
read -r RT RV <<<"$(fund_of)"
REFUND=$("$TOOL" refund $PAST "$RT" "$RV" 500000000 "$PAYOUT_SPK" 100000)
call sendrawtransaction "[\"$REFUND\"]" >/dev/null
mine 1
check "refund 1 conf"         ok=1 spent=1 claim=0 spendconfs=1 secret=0
stop_watcher

[[ $fail -eq 0 ]] && echo "DIN REGTEST: PASS" || { echo "DIN REGTEST: FAIL"; exit 1; }
