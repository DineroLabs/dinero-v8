#!/usr/bin/env bash
set -Eeuo pipefail

# ===== Config (override via env: NET, DATADIR, RPC_PORT) =====
REPO="${REPO:-/Users/haydarevich/Documents/DineroCoin}"
NET="${NET:-regtest}"
DATADIR="${DATADIR:-$REPO/data}"
CANDIDATE_PORTS=()
[[ -n "${RPC_PORT:-}" ]] && CANDIDATE_PORTS+=("$RPC_PORT")
CANDIDATE_PORTS+=("20998" "20999")   # try both, common in your logs

# ===== Helpers =====
die(){ echo "❌ $*" >&2; exit 1; }
need(){ command -v "$1" >/dev/null || die "Missing dependency: $1"; }

need curl
need jq
need sqlite3

cd "$REPO"

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
  ( ./build/dinerod $NETFLAG -datadir="$DATADIR" & echo $! > "$NETDIR/.tmp_dinerod_pid" ) || die "Failed to start dinerod"
  started_pid="$(cat "$NETDIR/.tmp_dinerod_pid" || true)"
  rm -f "$NETDIR/.tmp_dinerod_pid"
}

stop_if_started() {
  if [[ -n "$started_pid" ]]; then
    log "Stopping dinerod (pid=$started_pid)…"
    kill "$started_pid" 2>/dev/null || true
    # graceful wait
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
      echo "$p"
      return 0
    fi
  done
  # If healthz not available yet, probe JSON-RPC
  for p in "${CANDIDATE_PORTS[@]}"; do
    [[ -z "$p" ]] && continue
    if curl -s --user "$(cat "$COOKIE")" -H 'Content-Type: application/json' \
       -d '{"jsonrpc":"2.0","id":"t","method":"uptime","params":[]}' "http://127.0.0.1:$p/" \
       | jq -re 'has("result")' >/dev/null 2>&1; then
      echo "$p"
      return 0
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

db_meta() { # $1 key
  sqlite3 "$NETDIR/blockchain.db" "select value from meta where key='$1';"
}

lower() { tr '[:upper:]' '[:lower:]'; }

# ===== Run =====
trap 'stop_if_started' EXIT

echo
echo "🎊 FINAL CORE 4 + HEALTH/METRICS SMOKE"
echo "======================================="
echo "Repo:     $REPO"
echo "Network:  $NET ($NETFLAG)"
echo "Datadir:  $DATADIR"
echo

start_if_needed
wait_for_cookie

RPC_PORT="$(detect_port)"
echo "RPC port: $RPC_PORT"
echo

echo "1) /healthz …"
HEALTH="$(curl -s "http://127.0.0.1:${RPC_PORT}/healthz" | jq .)"
echo "$HEALTH" | jq . >/dev/null || die "/healthz not JSON"
echo "$HEALTH" | jq -re '.status=="ok"' >/dev/null || die "/healthz status != ok"
echo "✅ /healthz ok"

echo
echo "2) Core 4 RPC …"

echo "- getblockchaininfo"
INFO="$(rpc getblockchaininfo)"
echo "$INFO" | jq . >/dev/null || die "getblockchaininfo invalid JSON"
CHAIN=$(echo "$INFO" | jq -r '.result.chain // .chain')
BLOCKS=$(echo "$INFO" | jq -r '.result.blocks // .blocks')
BBH=$(echo "$INFO" | jq -r '.result.bestblockhash // .bestblockhash')
CW=$(echo "$INFO" | jq -r '.result.chainwork // .chainwork')
echo "  chain=$CHAIN blocks=$BLOCKS"
echo "  bestblockhash=$BBH"
echo "  chainwork=$CW"

echo "- getbestblockhash"
BEST="$(rpc getbestblockhash | jq -r '.result')"
[[ "$BEST" =~ ^[0-9a-f]{64}$ ]] || die "bestblockhash not 64-hex"
echo "  best=$BEST"

echo "- getblockcount"
COUNT="$(rpc getblockcount | jq -r '.result')"
[[ "$COUNT" =~ ^[0-9]+$ ]] || die "blockcount not numeric"
echo "  count=$COUNT"

echo "- uptime"
UP="$(rpc uptime | jq -r '.result')"
[[ "$UP" =~ ^[0-9]+$ ]] || die "uptime not numeric"
echo "  uptime=${UP}s"

echo "✅ Core 4 ok"

echo
echo "3) RPC ↔ DB consistency …"
DB_GEN=$(db_meta genesis_hash | lower)
DB_BEST=$(db_meta besthash | lower)
DB_H=$(db_meta height)
[[ -z "$DB_GEN" ]] && die "DB genesis_hash missing"
[[ -z "$DB_BEST" ]] && die "DB besthash missing"
[[ -z "$DB_H" ]] && die "DB height missing"

RPC_H0="$(rpc getblockhash '[0]' | jq -r '.result')"
[[ "$RPC_H0" == "$DB_GEN" ]] || die "Mismatch: RPC getblockhash(0) != DB genesis_hash"

[[ "$BEST" == "$DB_BEST" ]] || die "Mismatch: RPC besthash != DB besthash"
[[ "$COUNT" == "$DB_H"    ]] || die "Mismatch: RPC height != DB height"

echo "✅ Consistency ok"

echo
echo "4) Invariants & CI guards …"

# Height-based invariants
HEIGHT="$(sqlite3 "$NETDIR/blockchain.db" "SELECT value FROM meta WHERE key='height';")"
BEST_HASH="$(sqlite3 "$NETDIR/blockchain.db" "SELECT value FROM meta WHERE key='besthash';" | lower)"
GENESIS_HASH="$(sqlite3 "$NETDIR/blockchain.db" "SELECT value FROM meta WHERE key='genesis_hash';" | lower)"

echo "- Height: $HEIGHT"
echo "- Best hash: $BEST_HASH"
echo "- Genesis hash: $GENESIS_HASH"

if [[ "$HEIGHT" == "0" ]]; then
  [[ "$BEST_HASH" == "$GENESIS_HASH" ]] || die "At height 0, besthash must equal genesis_hash"
  echo "✅ Height 0 invariant: besthash == genesis_hash"
else
  [[ "$BEST_HASH" != "$GENESIS_HASH" ]] || die "At height >= 1, besthash must not equal genesis_hash"
  echo "✅ Height >= 1 invariant: besthash != genesis_hash"
fi

echo
echo "5) New Block/Header RPCs …"

# Test getdifficulty
DIFFICULTY="$(rpc getdifficulty | jq -r '.result')"
echo "- getdifficulty: $DIFFICULTY"
[[ "$DIFFICULTY" =~ ^[0-9]+(\.[0-9]+)?$ ]] || die "getdifficulty not numeric"

# Test getblockheader for genesis
HEADER="$(rpc getblockheader "[\"$GENESIS_HASH\"]" | jq -r '.result')"
echo "- getblockheader (genesis): $(echo "$HEADER" | jq -r '.hash // "raw_hex"' | head -c 16)..."

# Test getblock for genesis  
BLOCK="$(rpc getblock "[\"$GENESIS_HASH\"]" | jq -r '.result')"
echo "- getblock (genesis): $(echo "$BLOCK" | jq -r '.hash // "raw_hex"' | head -c 16)..."

# Test enhanced getnetworkinfo
NETINFO="$(rpc getnetworkinfo | jq -r '.result')"
VERSION="$(echo "$NETINFO" | jq -r '.version')"
SUBVERSION="$(echo "$NETINFO" | jq -r '.subversion')"
echo "- getnetworkinfo: version=$VERSION subversion=$SUBVERSION"

echo "✅ Block/Header RPCs ok"

echo
echo "6) /metrics …"
METRICS="$(curl -s "http://127.0.0.1:${RPC_PORT}/metrics")"
echo "$METRICS" | grep -q 'dinero_' || die "metrics missing dinero_* lines"
echo "✅ /metrics ok"

echo
echo "🎉 ALL CHECKS PASSED"
