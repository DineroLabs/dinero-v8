#!/usr/bin/env bash
set -euo pipefail

# Crash-recovery test: SIGKILL during mining, restart, verify consistency
# Run with VERIFY_CRASH_RECOVERY=1 to enable

if [[ "${VERIFY_CRASH_RECOVERY:-0}" != "1" ]]; then
  echo "ℹ️  Crash-recovery test disabled (set VERIFY_CRASH_RECOVERY=1 to enable)"
  exit 0
fi

say() { printf "%s\n" "$*"; }
fail() { say "❌ $*"; exit 1; }

TMPTAG="verify-crash-$$"
INSTANCE_TAG="crash-test-$$"
DATADIR="/tmp/${TMPTAG}"
PORT=$(( 22000 + (RANDOM % 10000) ))
TIMEOUT=${TIMEOUT:-120}
JSON_OUT="${JSON_OUT:-}"

# Parse command line arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --json) JSON_OUT="$2"; shift 2;;
    --timeout) TIMEOUT="$2"; shift 2;;
    *) fail "Unknown arg: $1";;
  esac
done

cleanup() {
  # Kill any remaining processes
  pkill -f "$INSTANCE_TAG" >/dev/null 2>&1 || true
  sleep 1
  # Force cleanup if needed
  pkill -9 -f "$INSTANCE_TAG" >/dev/null 2>&1 || true
  
  # Preserve datadir for analysis if test fails
  if [[ "${PRESERVE_ON_FAIL:-0}" == "1" ]]; then
    say "📁 Datadir preserved for analysis: $DATADIR"
  else
    rm -rf "$DATADIR" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

# --- Test Setup ---
mkdir -p "$DATADIR"
say "💥 Starting crash-recovery test"
say "   📁 Datadir: $DATADIR"
say "   🔌 RPC Port: $PORT"
say "   🏷️  Instance: $INSTANCE_TAG"

D_BIN=
for p in ./build-test/bin/dinerod ./build/bin/dinerod ./bin/dinerod ./dinerod; do
  if [[ -x "$p" ]]; then D_BIN="$p"; break; fi
done
[[ -n "$D_BIN" ]] || fail "dinerod binary not found (build first)"

rpc() {
  local payload="$1"
  curl -s --basic --user "$AUTH" -H 'content-type: application/json' --data "$payload" "http://127.0.0.1:${PORT}/"
}

get_tip_height() {
  rpc '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' | jq -r '.result // 0'
}

get_tip_hash() {
  local height=$(get_tip_height)
  if [[ "$height" -gt 0 ]]; then
    rpc "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockhash\",\"params\":[$height]}" | jq -r '.result'
  else
    echo "genesis"
  fi
}

start_daemon() {
  say "🚀 Starting daemon..."
  "$D_BIN" -regtest -datadir="$DATADIR" -rpcport="$PORT" -instance-tag="$INSTANCE_TAG" -printtoconsole >"$DATADIR/daemon.log" 2>&1 &
  DAEMON_PID=$!
  
  # Wait for startup
  local deadline=$(( SECONDS + 30 ))
  until [[ -f "$DATADIR/regtest/.cookie" ]]; do
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
      fail "Daemon died during startup"
    fi
    (( SECONDS > deadline )) && fail "Daemon startup timeout"
    sleep 0.1
  done
  
  AUTH="$(cat "$DATADIR/regtest/.cookie")"
  
  until curl -m 1 -s "http://127.0.0.1:${PORT}/" >/dev/null 2>&1; do
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
      fail "Daemon died during HTTP startup"
    fi
    (( SECONDS > deadline )) && fail "HTTP startup timeout"
    sleep 0.1
  done
  
  say "✅ Daemon ready (PID: $DAEMON_PID)"
}

# --- Phase 1: Initial State ---
start_daemon

say "📊 Recording initial state..."
INITIAL_HEIGHT=$(get_tip_height)
INITIAL_HASH=$(get_tip_hash)
say "   Initial height: $INITIAL_HEIGHT"
say "   Initial hash: $INITIAL_HASH"

# Mine some blocks to establish state
say "⛏️  Mining 3 blocks to establish state..."
rpc '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[true]}' >/dev/null

# Wait for mining to start
sleep 2

PRE_CRASH_HEIGHT=$(get_tip_height)
PRE_CRASH_HASH=$(get_tip_hash)
say "   Pre-crash height: $PRE_CRASH_HEIGHT"
say "   Pre-crash hash: $PRE_CRASH_HASH"

# --- Phase 2: Crash During Mining ---
say "💥 Simulating crash during active mining..."

# Start mining and immediately kill
rpc '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[true]}' >/dev/null &

# Give mining a moment to start, then crash
sleep 1
say "   Sending SIGKILL to daemon (PID: $DAEMON_PID)..."
kill -9 "$DAEMON_PID" 2>/dev/null || true

# Wait for process to die
sleep 2

# Verify process is dead
if kill -0 "$DAEMON_PID" 2>/dev/null; then
  fail "Daemon survived SIGKILL"
fi
say "✅ Daemon crashed successfully"

# --- Phase 3: Recovery ---
say "🔄 Testing recovery..."

# Check for corruption indicators in datadir
CORRUPTION_FOUND=0
if find "$DATADIR" -name "*.log" -exec grep -l "corruption\|corrupt\|invalid" {} \; | head -1 >/dev/null; then
  say "⚠️  Potential corruption indicators found in logs"
  CORRUPTION_FOUND=1
fi

# Restart daemon
start_daemon

say "📊 Verifying post-recovery state..."
RECOVERY_HEIGHT=$(get_tip_height)
RECOVERY_HASH=$(get_tip_hash)
say "   Recovery height: $RECOVERY_HEIGHT"
say "   Recovery hash: $RECOVERY_HASH"

# --- Phase 4: Consistency Checks ---
RECOVERY_OK=true
RECOVERY_ISSUES=()

# Check 1: Height should be >= pre-crash (might have mined more)
if [[ "$RECOVERY_HEIGHT" -lt "$PRE_CRASH_HEIGHT" ]]; then
  RECOVERY_OK=false
  RECOVERY_ISSUES+=("Height regressed: $PRE_CRASH_HEIGHT → $RECOVERY_HEIGHT")
fi

# Check 2: If height is same, hash should match
if [[ "$RECOVERY_HEIGHT" -eq "$PRE_CRASH_HEIGHT" && "$RECOVERY_HASH" != "$PRE_CRASH_HASH" ]]; then
  RECOVERY_OK=false
  RECOVERY_ISSUES+=("Hash mismatch at same height: $PRE_CRASH_HASH → $RECOVERY_HASH")
fi

# Check 3: Daemon should be responsive
if ! rpc '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' | jq -e '.result' >/dev/null; then
  RECOVERY_OK=false
  RECOVERY_ISSUES+=("Daemon not responsive after recovery")
fi

# Check 4: Should be able to mine new blocks
say "🧪 Testing post-recovery mining..."
rpc '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[true]}' >/dev/null
sleep 3
rpc '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[false]}' >/dev/null

POST_MINING_HEIGHT=$(get_tip_height)
if [[ "$POST_MINING_HEIGHT" -le "$RECOVERY_HEIGHT" ]]; then
  RECOVERY_OK=false
  RECOVERY_ISSUES+=("Unable to mine after recovery: $RECOVERY_HEIGHT → $POST_MINING_HEIGHT")
fi

# Check 5: Database integrity (no corruption messages in new logs)
if grep -q "corruption\|corrupt\|invalid\|database.*error" "$DATADIR/daemon.log" 2>/dev/null; then
  RECOVERY_OK=false
  RECOVERY_ISSUES+=("Database corruption messages in recovery logs")
fi

# Check 6: Test database repair functionality
say "🔧 Testing database repair functionality..."
if "$D_BIN" -regtest -datadir="$DATADIR" --db-repair >/dev/null 2>&1; then
  say "   ✅ Database repair completed successfully"
else
  RECOVERY_OK=false
  RECOVERY_ISSUES+=("Database repair functionality failed")
fi

# --- Results ---
if [[ "$RECOVERY_OK" == "true" ]]; then
  say "✅ Crash-recovery test PASSED"
  say "   📊 Height progression: $INITIAL_HEIGHT → $PRE_CRASH_HEIGHT → $RECOVERY_HEIGHT → $POST_MINING_HEIGHT"
  say "   🔄 Recovery successful, mining resumed"
else
  say "❌ Crash-recovery test FAILED"
  for issue in "${RECOVERY_ISSUES[@]}"; do
    say "   ⚠️  $issue"
  done
fi

# --- JSON Output ---
if [[ -n "$JSON_OUT" ]]; then
  tmp="$JSON_OUT.tmp.$$"
  {
    printf '{\n'
    printf '  "ok": %s,\n' "$([[ "$RECOVERY_OK" == "true" ]] && echo "true" || echo "false")"
    printf '  "crash_recovery": {\n'
    printf '    "recovery_ok": %s,\n' "$([[ "$RECOVERY_OK" == "true" ]] && echo "true" || echo "false")"
    printf '    "corruption_found": %s,\n' "$([[ "$CORRUPTION_FOUND" -eq 0 ]] && echo "false" || echo "true")"
    printf '    "heights": {\n'
    printf '      "initial": %d,\n' "$INITIAL_HEIGHT"
    printf '      "pre_crash": %d,\n' "$PRE_CRASH_HEIGHT"
    printf '      "recovery": %d,\n' "$RECOVERY_HEIGHT"
    printf '      "post_mining": %d\n' "$POST_MINING_HEIGHT"
    printf '    },\n'
    printf '    "issues": [%s]\n' "$(printf '"%s",' "${RECOVERY_ISSUES[@]}" | sed 's/,$//')"
    printf '  },\n'
    printf '  "rpc": {"port": "%s", "datadir": "%s"}\n' "$PORT" "$DATADIR"
    printf '}\n'
  } > "$tmp"
  mv "$tmp" "$JSON_OUT"
  say "🧾 Wrote JSON: $JSON_OUT"
fi

# Stop daemon cleanly
rpc '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[false]}' >/dev/null || true
kill "$DAEMON_PID" 2>/dev/null || true
sleep 1

if [[ "$RECOVERY_OK" == "true" ]]; then
  say "🎉 Crash-recovery test completed successfully"
  exit 0
else
  say "💥 Crash-recovery test failed - data integrity issues detected"
  exit 1
fi
