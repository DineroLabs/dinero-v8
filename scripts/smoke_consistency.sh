#!/usr/bin/env bash
set -Eeuo pipefail

# ===== Config =====
REPO="${REPO:-/Users/haydarevich/Documents/DineroCoin}"
NET="${NET:-regtest}"
DATADIR="${DATADIR:-$REPO/data}"
CANDIDATE_PORTS=()
[[ -n "${RPC_PORT:-}" ]] && CANDIDATE_PORTS+=("$RPC_PORT")
CANDIDATE_PORTS+=("20998" "20999")

# ===== Helpers =====
die(){ echo "❌ $*" >&2; exit 1; }
need(){ command -v "$1" >/dev/null || die "Missing dependency: $1"; }
need curl; need jq

NETFLAG=""
case "$NET" in
  regtest) NETFLAG="-regtest" ;;
  testnet) NETFLAG="-testnet" ;;
  mainnet|"") NETFLAG="" ;;
  *) NETFLAG="-$NET" ;;
esac

NETDIR="$DATADIR/$NET"
COOKIE="$NETDIR/.cookie"
started_pid=""

log(){ printf "%s %s\n" "$(date +'%H:%M:%S')" "$*"; }

start_if_needed() {
  if pgrep -f "dinerod.*${NETFLAG}" >/dev/null; then
    log "Daemon already running for $NET"
    return
  fi
  log "Starting dinerod ($NETFLAG, datadir=$DATADIR) …"
  mkdir -p "$NETDIR"
  ( "${REPO}/build/dinerod" $NETFLAG -datadir="$DATADIR" & echo $! > "$NETDIR/.tmp_dinerod_pid" ) || die "Failed to start dinerod"
  started_pid="$(cat "$NETDIR/.tmp_dinerod_pid" || true)"
  rm -f "$NETDIR/.tmp_dinerod_pid"
}

stop_if_started() {
  if [[ -n "$started_pid" ]]; then
    log "Stopping dinerod (pid=$started_pid)…"
    kill "$started_pid" 2>/dev/null || true
    for _ in {1..30}; do
      kill -0 "$started_pid" 2>/dev/null || break
      sleep 0.2
    done
  fi
}

wait_for_cookie() {
  log "Waiting for cookie: $COOKIE"
  for _ in {1..100}; do
    [[ -f "$COOKIE" ]] && return 0
    sleep 0.1
  done
  die "Cookie not created at $COOKIE"
}

detect_port() {
  for p in "${CANDIDATE_PORTS[@]}"; do
    [[ -z "$p" ]] && continue
    if curl -s "http://127.0.0.1:$p/healthz" | jq -re '.status=="ok"' >/dev/null 2>&1; then
      echo "$p"; return 0
    fi
  done
  for p in "${CANDIDATE_PORTS[@]}"; do
    [[ -z "$p" ]] && continue
    if curl -s --user "$(cat "$COOKIE")" -H 'Content-Type: application/json' \
       -d '{"jsonrpc":"2.0","id":"t","method":"uptime","params":[]}' "http://127.0.0.1:$p/" \
       | jq -re 'has("result")' >/dev/null 2>&1; then
      echo "$p"; return 0
    fi
  done
  die "Could not detect RPC port (tried: ${CANDIDATE_PORTS[*]})"
}

rpc() { # $1 method, $2 params JSON array (optional)
  local method="$1"
  local params="${2:-[]}"
  curl -s --user "$(cat "$COOKIE")" -H 'Content-Type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":\"t\",\"method\":\"$method\",\"params\":$params}" \
    "http://127.0.0.1:${RPC_PORT}/"
}

# ===== Run =====
trap 'stop_if_started' EXIT

echo
echo "🔎 CONSISTENCY SMOKE: chainwork + nTx invariants"
echo "================================================="
echo "Repo:    $REPO"
echo "Network: $NET ($NETFLAG)"
echo "Datadir: $DATADIR"
echo

start_if_needed
wait_for_cookie
RPC_PORT="$(detect_port)"
echo "RPC port: $RPC_PORT"
echo

# 1) Chainwork invariant
INFO="$(rpc getblockchaininfo)"
HEIGHT="$(echo "$INFO" | jq -r '.result.blocks // .blocks')"
CW_INFO="$(echo "$INFO" | jq -r '.result.chainwork // .chainwork')"
[[ "$HEIGHT" =~ ^[0-9]+$ ]] || die "Height not numeric"
[[ "$CW_INFO" =~ ^[0-9a-f]{64}$ ]] || die "chainwork from getblockchaininfo not 64-hex"

if [[ "$HEIGHT" == "0" ]]; then
  H0="$(rpc getblockhash '[0]' | jq -r '.result')"
  [[ "$H0" =~ ^[0-9a-f]{64}$ ]] || die "genesis hash not 64-hex"
  HDR="$(rpc getblockheader "[\"$H0\", true]")"
  CW_HDR="$(echo "$HDR" | jq -r '.result.chainwork // empty')"
  [[ "$CW_HDR" =~ ^[0-9a-f]{64}$ ]] || die "chainwork missing in genesis header"
  echo "• Chainwork (info)   : $CW_INFO"
  echo "• Chainwork (header0): $CW_HDR"
  [[ "$CW_INFO" == "$CW_HDR" ]] || die "Chainwork mismatch at height 0 (info != header(genesis))"
else
  BEST="$(rpc getbestblockhash | jq -r '.result')"
  [[ "$BEST" =~ ^[0-9a-f]{64}$ ]] || die "best hash not 64-hex"
  HDR="$(rpc getblockheader "[\"$BEST\", true]")"
  CW_HDR="$(echo "$HDR" | jq -r '.result.chainwork // empty')"
  [[ "$CW_HDR" =~ ^[0-9a-f]{64}$ ]] || die "chainwork missing in tip header"
  echo "• Chainwork (info) : $CW_INFO"
  echo "• Chainwork (header tip): $CW_HDR"
  [[ "$CW_INFO" == "$CW_HDR" ]] || die "Chainwork mismatch at tip (info != header(best))"
fi
echo "✅ Chainwork invariant ok"

# 2) nTx vs tx length invariant (verbosity=1)
BLK="$(rpc getblock "[\"$(rpc getblockhash '[0]' | jq -r .result)\", 1]")"
NTX="$(echo "$BLK" | jq -r '.result.nTx // empty')"
TXLEN="$(echo "$BLK" | jq -r '.result.tx | length')"
[[ "$NTX" =~ ^[0-9]+$ ]] || die "nTx not numeric in getblock(...,1)"
echo "• nTx reported : $NTX"
echo "• tx[] length  : $TXLEN"
[[ "$NTX" -eq "$TXLEN" ]] || die "nTx != length(tx) in getblock(...,1)"
echo "✅ getblock nTx/tx length invariant ok"

echo
echo "🎉 ALL CONSISTENCY CHECKS PASSED"
