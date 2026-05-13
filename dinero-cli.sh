#!/usr/bin/env bash
set -euo pipefail

# =========================
# Dinero CLI (grandma-friendly)
# =========================
# Usage examples:
#   ./dinero-cli.sh chain-info
#   ./dinero-cli.sh height
#   ./dinero-cli.sh start-mining 4      # start mining with 4 threads
#   ./dinero-cli.sh stop-mining
#   ./dinero-cli.sh mining-info
#   ./dinero-cli.sh best-hash
#   ./dinero-cli.sh clean-lock          # safely remove stale pid/cookie
#   ./dinero-cli.sh start-daemon        # start dinerod in background
#   ./dinero-cli.sh stop-daemon         # stop dinerod (RPC or PID)
#   ./dinero-cli.sh restart             # stop -> clean-lock -> start-daemon
#
# Config overrides (optional):
#   DINERO_DATADIR=/tmp/test-dir2 ./dinero-cli.sh chain-info
#   DINERO_RPC=http://127.0.0.1:20998 ./dinero-cli.sh height
#   DINERO_NETWORK=mainnet|testnet|regtest
#   DAEMON_BIN=./build-test/bin/dinerod ./dinero-cli.sh start-daemon

# ---- Config defaults
RPC_URL="${DINERO_RPC:-http://127.0.0.1:20998}"
DATADIR="${DINERO_DATADIR:-${HOME}/.dinero}"
NETWORK="${DINERO_NETWORK:-mainnet}"
DAEMON_BIN="${DAEMON_BIN:-dinerod}"

# Prefer your current temp datadir automatically if it exists
if [[ -d "/tmp/test-dir2" && -f "/tmp/test-dir2/${NETWORK}/.cookie" ]]; then
  DATADIR="/tmp/test-dir2"
fi

LOCK_FILE="${DATADIR}/${NETWORK}/dinerod.pid"
COOKIE_FILE="${DATADIR}/${NETWORK}/.cookie"

# Optional pretty print if jq is present
JQ="$(command -v jq || true)"
pp() {
  if [[ -n "$JQ" ]]; then
    "$JQ" -C .
  else
    cat
  fi
}

die() { echo "ERROR: $*" >&2; exit 1; }

read_auth() {
  [[ -f "$COOKIE_FILE" ]] || die "Cookie file not found: $COOKIE_FILE
- Is dinerod running?
- Correct DATADIR/NETWORK? Current: DATADIR='$DATADIR' NETWORK='$NETWORK'
Tip: DINERO_DATADIR=/tmp/test-dir2 ./dinero-cli.sh chain-info
"
  cat "$COOKIE_FILE"
}

rpc() {
  local method="$1"; shift || true
  local params_json="[]"
  if [[ "$#" -gt 0 ]]; then
    # Build simple JSON array from remaining args (strings only)
    local a=()
    for x in "$@"; do a+=("\"${x}\""); done
    params_json="[$(IFS=,; echo "${a[*]}")]"
  fi
  local AUTH; AUTH="$(read_auth)"
  curl -s --user "$AUTH" \
    -H 'content-type: application/json' \
    --data "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"${method}\",\"params\":${params_json}}" \
    "$RPC_URL"
}

rpc_raw() {
  local payload="$1"
  local AUTH; AUTH="$(read_auth)"
  curl -s --user "$AUTH" -H 'content-type: application/json' --data "$payload" "$RPC_URL"
}

get_height_val() {
  local out
  out="$(rpc getblockcount)"
  if [[ -n "$JQ" ]]; then
    echo "$out" | "$JQ" -r '.result'
  else
    echo "$out" | sed -n 's/.*"result":\s*\([0-9]\+\).*/\1/p'
  fi
}

nproc_guess() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  else
    sysctl -n hw.ncpu 2>/dev/null || echo 1
  fi
}

height_cmd() { rpc getblockcount | pp; }

best_hash_cmd() {
  local h; h="$(get_height_val)"
  [[ -n "$h" ]] || die "Could not read block height"
  rpc getblockhash "$h" | pp
}

clean_lock_cmd() {
  echo "🔧 Cleaning stale PID lock and cookie files…"
  if [[ -f "$LOCK_FILE" ]]; then
    rm -f "$LOCK_FILE"
    echo "✅ Removed lock file: $LOCK_FILE"
  else
    echo "ℹ️ No lock file found at $LOCK_FILE"
  fi

  if [[ -f "$COOKIE_FILE" ]]; then
    rm -f "$COOKIE_FILE"
    echo "✅ Removed cookie file: $COOKIE_FILE (will be regenerated)"
  else
    echo "ℹ️ No cookie file found at $COOKIE_FILE"
  fi

  echo "🎉 Lock cleanup complete. You can now (re)start dinerod safely."
}

stop_daemon_cmd() {
  echo "🛑 Stopping daemon…"
  # Try RPC stop if cookie exists and RPC responds
  if [[ -f "$COOKIE_FILE" ]]; then
    # Attempt RPC stop; if method isn't supported, it will error
    if rpc_raw '{"jsonrpc":"2.0","id":1,"method":"stop","params":[]}' | grep -qi '"result"'; then
      echo "✅ Stopped via RPC."
      sleep 1
      return 0
    fi
  fi

  # Fallback: PID file
  if [[ -f "$LOCK_FILE" ]]; then
    PID="$(cat "$LOCK_FILE" 2>/dev/null || true)"
    if [[ -n "${PID:-}" && "$PID" =~ ^[0-9]+$ ]]; then
      if kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null || true
        echo "⏳ Sent TERM to PID $PID…"
        for i in {1..10}; do
          sleep 0.3
          kill -0 "$PID" 2>/dev/null || { echo "✅ Process $PID exited."; break; }
          [[ $i -eq 10 ]] && { echo "⚠️  Forcing kill -9 $PID"; kill -9 "$PID" 2>/dev/null || true; }
        done
      else
        echo "ℹ️ PID $PID not running."
      fi
    else
      echo "ℹ️ PID file exists but invalid."
    fi
  else
    echo "ℹ️ No PID file; daemon may already be stopped."
  fi
}

start_daemon_cmd() {
  echo "🚀 Starting dinerod in background…"
  [[ -x "$DAEMON_BIN" || "$(command -v "$DAEMON_BIN" || true)" ]] || die "Cannot find dinerod binary. Set DAEMON_BIN=/path/to/dinerod"
  "$DAEMON_BIN" -datadir="$DATADIR" -daemon >/dev/null 2>&1 || {
    echo "⚠️ Start attempt returned non-zero. Checking status…"
  }
  # Wait a moment for cookie to appear
  for i in {1..20}; do
    [[ -f "$COOKIE_FILE" ]] && break
    sleep 0.2
  done
  if [[ -f "$COOKIE_FILE" ]]; then
    echo "✅ dinerod started. Cookie: $COOKIE_FILE"
  else
    echo "⚠️ dinerod may not have started; cookie not found."
  fi
}

restart_cmd() {
  echo "🔁 Restarting daemon…"
  stop_daemon_cmd || true
  clean_lock_cmd
  start_daemon_cmd
  echo "🔎 Checking chain-info…"
  if [[ -f "$COOKIE_FILE" ]]; then
    rpc getblockchaininfo | pp
  else
    echo "ℹ️ Skipping chain-info (no cookie yet)."
  fi
}

test_suite_cmd() {
  echo "🧪 Running Dinero RPC test suite…"

  # 1) Header check
  if curl -sD - -o /dev/null --user "$(cat "$COOKIE_FILE")" \
      -H 'content-type: application/json' \
      --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' "$RPC_URL" \
     | tr -d '\r' | grep -iq '^x-dinero-rpc-engine: v2'; then
    echo "✅ Header: x-dinero-rpc-engine: v2"
  else
    echo "❌ Header missing or wrong"
  fi

  # Height & best hash
  H="$(rpc getblockcount | jq -r '.result')"
  echo "Height: ${H:-unknown}"
  BH="$(rpc_raw '{"jsonrpc":"2.0","id":1,"method":"getblockhash","params":['"$H"']}' | jq -r '.result')"
  echo "Best hash: ${BH:-unknown}"

  # 2) nextblockhash omitted at tip
  if rpc_raw '{"jsonrpc":"2.0","id":1,"method":"getblock","params":["'"$BH"'",true]}' | jq -e '.result | has("nextblockhash") | not' >/dev/null; then
    echo "✅ nextblockhash omitted at tip"
  else
    echo "❌ nextblockhash present at tip"
  fi

  # 3) No 'error': null
  if rpc getblockcount | jq -e 'has("error") | not' >/dev/null; then
    echo "✅ JSON clean (no \"error\": null)"
  else
    echo "❌ Found top-level \"error\" field"
  fi
}

headers_cmd() {
  echo "🔎 Fetching headers…"
  curl -sD - -o /dev/null --user "$(cat "$COOKIE_FILE")" \
    -H 'content-type: application/json' \
    --data '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' "$RPC_URL" \
  | tr -d '\r'
}

cmd="${1:-}"; shift || true

case "$cmd" in
  # ---------- Friendly aliases ----------
  start|mining-start|start-mining)
    threads="${1:-}"
    if [[ -z "$threads" ]]; then
      cores="$(nproc_guess)"
      threads=$(( cores > 1 ? cores - 1 : 1 ))
    fi
    echo "Starting mining with $threads thread(s)…"
    rpc setgenerate true "$threads" | pp
    ;;

  stop|mining-stop|stop-mining)
    echo "Stopping mining…"
    rpc setgenerate false 0 | pp
    ;;

  mining-info|mining|status) rpc getmininginfo | pp ;;

  chain-info|info)           rpc getblockchaininfo | pp ;;

  tips|chaintips)            rpc getchaintips | pp ;;

  net|netstats|network)      rpc getnetworkstats | pp ;;

  height|blockcount)         height_cmd ;;

  best-hash|besthash)        best_hash_cmd ;;

  clean-lock|unlock|fix-lock) clean_lock_cmd ;;

  start-daemon)              start_daemon_cmd ;;

  stop-daemon)               stop_daemon_cmd ;;

  restart)                   restart_cmd ;;

  test-suite)               test_suite_cmd ;;

  headers)                   headers_cmd ;;

  # ---------- Direct RPC passthrough with string params ----------
  getblockhash|getblock|getblockcount|getblockchaininfo|getmininginfo|getnetworkstats|getchaintips|importaddress|importxpub|signmessage|verifymessage|setgenerate|stop)
    rpc "$cmd" "$@" | pp
    ;;

  rpc)
    payload="${1:-}"
    [[ -n "$payload" ]] || die "Usage: ./dinero-cli.sh rpc '<full JSON payload>'"
    rpc_raw "$payload" | pp
    ;;

  --help|-h|help|"")
    cat <<EOF
Dinero CLI (grandma-friendly)

ENV overrides:
  DINERO_DATADIR=/path/to/datadir        (default: ${DATADIR})
  DINERO_NETWORK=mainnet|testnet|regtest (default: ${NETWORK})
  DINERO_RPC=http://127.0.0.1:20998      (default: ${RPC_URL})
  DAEMON_BIN=/path/to/dinerod            (default: ${DAEMON_BIN})

Commands:
  start-mining [threads]  Start mining (default: CPU-1 threads)
  stop-mining             Stop mining
  mining-info             Show mining status
  chain-info              Show chain info
  height                  Show current block height
  best-hash               Show best block hash
  tips                    Show chain tips
  net                     Show network stats
  clean-lock              Remove stale pid & cookie (safe)
  start-daemon            Start dinerod in background
  stop-daemon             Stop dinerod (RPC if available, else PID)
  restart                 Stop -> clean-lock -> start-daemon
  test-suite              Run RPC validation tests
  headers                 Show RPC response headers

Raw RPC passthrough (string params):
  getblockcount
  getblockhash <height>
  getblock <hash> [true|false]
  setgenerate <true|false> <threads>
  stop
  ... and more methods supported by your daemon

Raw JSON:
  rpc '<json-payload>'    e.g. rpc '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}'

Examples:
  ./dinero-cli.sh chain-info
  ./dinero-cli.sh start-mining 4
  DINERO_DATADIR=/tmp/test-dir2 ./dinero-cli.sh mining-info
  DAEMON_BIN=./build-test/bin/dinerod DINERO_DATADIR=/tmp/test-dir2 ./dinero-cli.sh start-daemon
  ./dinero-cli.sh restart
  ./dinero-cli.sh test-suite
  ./dinero-cli.sh headers
EOF
    ;;

  *)
    echo "Unknown command: '$cmd'. Try --help." >&2
    exit 2
    ;;
esac
