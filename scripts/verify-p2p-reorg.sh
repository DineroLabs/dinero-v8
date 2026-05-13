#!/usr/bin/env bash
set -euo pipefail

# P2P reorg test: 2 nodes, mine competing chains, connect, verify best chain wins
# Run with VERIFY_REORG=1 to enable

if [[ "${VERIFY_REORG:-0}" != "1" ]]; then
  echo "ℹ️  P2P reorg test disabled (set VERIFY_REORG=1 to enable)"
  exit 0
fi

say() { printf "%s\n" "$*"; }
fail() { say "❌ $*"; exit 1; }

TMPTAG="verify-reorg-$$"
DATADIR_A="/tmp/${TMPTAG}-nodeA"
DATADIR_B="/tmp/${TMPTAG}-nodeB"
INSTANCE_A="reorg-nodeA-$$"
INSTANCE_B="reorg-nodeB-$$"
PORT_A=$(( 22000 + (RANDOM % 5000) ))
PORT_B=$(( PORT_A + 1 ))
P2P_PORT_A=$(( 20000 + (RANDOM % 5000) ))
P2P_PORT_B=$(( P2P_PORT_A + 1 ))
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
  say "🧹 Cleaning up nodes..."
  pkill -f "$INSTANCE_A" >/dev/null 2>&1 || true
  pkill -f "$INSTANCE_B" >/dev/null 2>&1 || true
  sleep 1
  pkill -9 -f "$INSTANCE_A" >/dev/null 2>&1 || true
  pkill -9 -f "$INSTANCE_B" >/dev/null 2>&1 || true
  
  rm -rf "$DATADIR_A" "$DATADIR_B" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

# --- Test Setup ---
mkdir -p "$DATADIR_A" "$DATADIR_B"
say "🔗 Starting P2P reorg test"
say "   📁 Node A: $DATADIR_A (RPC:$PORT_A, P2P:$P2P_PORT_A)"
say "   📁 Node B: $DATADIR_B (RPC:$PORT_B, P2P:$P2P_PORT_B)"

D_BIN=
for p in ./build-test/bin/dinerod ./build/bin/dinerod ./bin/dinerod ./dinerod; do
  if [[ -x "$p" ]]; then D_BIN="$p"; break; fi
done
[[ -n "$D_BIN" ]] || fail "dinerod binary not found (build first)"

rpc_a() {
  local payload="$1"
  curl -s --basic --user "$AUTH_A" -H 'content-type: application/json' --data "$payload" "http://127.0.0.1:${PORT_A}/"
}

rpc_b() {
  local payload="$1"
  curl -s --basic --user "$AUTH_B" -H 'content-type: application/json' --data "$payload" "http://127.0.0.1:${PORT_B}/"
}

get_tip_height() {
  local node="$1"
  if [[ "$node" == "A" ]]; then
    rpc_a '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' | jq -r '.result // 0'
  else
    rpc_b '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' | jq -r '.result // 0'
  fi
}

get_tip_hash() {
  local node="$1"
  local height=$(get_tip_height "$node")
  if [[ "$height" -gt 0 ]]; then
    if [[ "$node" == "A" ]]; then
      rpc_a "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockhash\",\"params\":[$height]}" | jq -r '.result'
    else
      rpc_b "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getblockhash\",\"params\":[$height]}" | jq -r '.result'
    fi
  else
    echo "genesis"
  fi
}

start_node() {
  local node="$1"
  local datadir="$2"
  local instance="$3"
  local rpc_port="$4"
  local p2p_port="$5"
  
  say "🚀 Starting node $node..."
  "$D_BIN" -regtest -datadir="$datadir" -rpcport="$rpc_port" -port="$p2p_port" \
    -instance-tag="$instance" -printtoconsole >"$datadir/daemon.log" 2>&1 &
  
  local pid=$!
  
  # Wait for startup
  local deadline=$(( SECONDS + 30 ))
  until [[ -f "$datadir/regtest/.cookie" ]]; do
    if ! kill -0 "$pid" 2>/dev/null; then
      fail "Node $node died during startup"
    fi
    (( SECONDS > deadline )) && fail "Node $node startup timeout"
    sleep 0.1
  done
  
  local auth="$(cat "$datadir/regtest/.cookie")"
  
  until curl -m 1 -s "http://127.0.0.1:${rpc_port}/" >/dev/null 2>&1; do
    if ! kill -0 "$pid" 2>/dev/null; then
      fail "Node $node died during HTTP startup"
    fi
    (( SECONDS > deadline )) && fail "Node $node HTTP startup timeout"
    sleep 0.1
  done
  
  say "✅ Node $node ready (PID: $pid)"
  
  if [[ "$node" == "A" ]]; then
    AUTH_A="$auth"
    PID_A="$pid"
  else
    AUTH_B="$auth"
    PID_B="$pid"
  fi
}

mine_blocks() {
  local node="$1"
  local count="$2"
  
  say "⛏️  Mining $count blocks on node $node..."
  
  if [[ "$node" == "A" ]]; then
    rpc_a '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[true]}' >/dev/null
  else
    rpc_b '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[true]}' >/dev/null
  fi
  
  local start_height=$(get_tip_height "$node")
  local target_height=$((start_height + count))
  
  # Wait for mining to complete
  local deadline=$(( SECONDS + 60 ))
  while :; do
    local current_height=$(get_tip_height "$node")
    if (( current_height >= target_height )); then
      break
    fi
    if (( SECONDS > deadline )); then
      fail "Mining timeout on node $node"
    fi
    sleep 1
  done
  
  # Stop mining
  if [[ "$node" == "A" ]]; then
    rpc_a '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[false]}' >/dev/null
  else
    rpc_b '{"jsonrpc":"2.0","id":1,"method":"setgenerate","params":[false]}' >/dev/null
  fi
  
  local final_height=$(get_tip_height "$node")
  say "   ✅ Node $node mined to height $final_height"
}

# --- Phase 1: Start Isolated Nodes ---
start_node "A" "$DATADIR_A" "$INSTANCE_A" "$PORT_A" "$P2P_PORT_A"
start_node "B" "$DATADIR_B" "$INSTANCE_B" "$PORT_B" "$P2P_PORT_B"

# Verify initial state
HEIGHT_A_INIT=$(get_tip_height "A")
HEIGHT_B_INIT=$(get_tip_height "B")
HASH_A_INIT=$(get_tip_hash "A")
HASH_B_INIT=$(get_tip_hash "B")

say "📊 Initial state:"
say "   Node A: height $HEIGHT_A_INIT, hash $HASH_A_INIT"
say "   Node B: height $HEIGHT_B_INIT, hash $HASH_B_INIT"

# --- Phase 2: Create Competing Chains ---
say "🥊 Creating competing chains..."

# Mine 3 blocks on A, 4 blocks on B (B should win)
mine_blocks "A" 3
mine_blocks "B" 4

HEIGHT_A_PRE=$(get_tip_height "A")
HEIGHT_B_PRE=$(get_tip_height "B")
HASH_A_PRE=$(get_tip_hash "A")
HASH_B_PRE=$(get_tip_hash "B")

say "📊 Pre-connection state:"
say "   Node A: height $HEIGHT_A_PRE, hash $HASH_A_PRE"
say "   Node B: height $HEIGHT_B_PRE, hash $HASH_B_PRE"

# Verify chains are different
if [[ "$HASH_A_PRE" == "$HASH_B_PRE" ]]; then
  fail "Nodes have same chain before connection (test setup error)"
fi

# --- Phase 3: Connect Nodes and Test Reorg ---
say "🔗 Connecting nodes for reorg..."

# Connect A to B
rpc_a "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"addnode\",\"params\":[\"127.0.0.1:$P2P_PORT_B\",\"add\"]}" >/dev/null || true

# Wait for reorg to complete
say "⏳ Waiting for reorg to complete..."
sleep 5

# Check final state
HEIGHT_A_POST=$(get_tip_height "A")
HEIGHT_B_POST=$(get_tip_height "B")
HASH_A_POST=$(get_tip_hash "A")
HASH_B_POST=$(get_tip_hash "B")

say "📊 Post-connection state:"
say "   Node A: height $HEIGHT_A_POST, hash $HASH_A_POST"
say "   Node B: height $HEIGHT_B_POST, hash $HASH_B_POST"

# --- Phase 4: Validation ---
REORG_OK=true
REORG_ISSUES=()

# Check 1: Both nodes should have same tip
if [[ "$HASH_A_POST" != "$HASH_B_POST" ]]; then
  REORG_OK=false
  REORG_ISSUES+=("Nodes have different tips after connection")
fi

# Check 2: Both nodes should have same height
if [[ "$HEIGHT_A_POST" != "$HEIGHT_B_POST" ]]; then
  REORG_OK=false
  REORG_ISSUES+=("Nodes have different heights after connection")
fi

# Check 3: Longer chain should win (B had 4 blocks vs A's 3)
if [[ "$HEIGHT_A_POST" -lt "$HEIGHT_B_PRE" ]]; then
  REORG_OK=false
  REORG_ISSUES+=("Longer chain did not win: expected >= $HEIGHT_B_PRE, got $HEIGHT_A_POST")
fi

# Check 4: Both nodes should be responsive
if ! rpc_a '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' | jq -e '.result' >/dev/null; then
  REORG_OK=false
  REORG_ISSUES+=("Node A not responsive after reorg")
fi

if ! rpc_b '{"jsonrpc":"2.0","id":1,"method":"getblockcount","params":[]}' | jq -e '.result' >/dev/null; then
  REORG_OK=false
  REORG_ISSUES+=("Node B not responsive after reorg")
fi

# Check 5: No crash indicators in logs
if grep -q "crash\|abort\|terminate\|segmentation" "$DATADIR_A/daemon.log" "$DATADIR_B/daemon.log" 2>/dev/null; then
  REORG_OK=false
  REORG_ISSUES+=("Crash indicators found in daemon logs")
fi

# --- Results ---
if [[ "$REORG_OK" == "true" ]]; then
  say "✅ P2P reorg test PASSED"
  say "   🔗 Nodes successfully connected and reorged"
  say "   📊 Final consensus: height $HEIGHT_A_POST, hash $HASH_A_POST"
else
  say "❌ P2P reorg test FAILED"
  for issue in "${REORG_ISSUES[@]}"; do
    say "   ⚠️  $issue"
  done
fi

# --- JSON Output ---
if [[ -n "$JSON_OUT" ]]; then
  tmp="$JSON_OUT.tmp.$$"
  {
    printf '{\n'
    printf '  "ok": %s,\n' "$([[ "$REORG_OK" == "true" ]] && echo "true" || echo "false")"
    printf '  "p2p_reorg": {\n'
    printf '    "reorg_ok": %s,\n' "$([[ "$REORG_OK" == "true" ]] && echo "true" || echo "false")"
    printf '    "consensus_reached": %s,\n' "$([[ "$HASH_A_POST" == "$HASH_B_POST" ]] && echo "true" || echo "false")"
    printf '    "node_a": {\n'
    printf '      "pre_height": %d,\n' "$HEIGHT_A_PRE"
    printf '      "post_height": %d,\n' "$HEIGHT_A_POST"
    printf '      "post_hash": "%s"\n' "$HASH_A_POST"
    printf '    },\n'
    printf '    "node_b": {\n'
    printf '      "pre_height": %d,\n' "$HEIGHT_B_PRE"
    printf '      "post_height": %d,\n' "$HEIGHT_B_POST"
    printf '      "post_hash": "%s"\n' "$HASH_B_POST"
    printf '    },\n'
    printf '    "issues": [%s]\n' "$(printf '"%s",' "${REORG_ISSUES[@]}" | sed 's/,$//')"
    printf '  }\n'
    printf '}\n'
  } > "$tmp"
  mv "$tmp" "$JSON_OUT"
  say "🧾 Wrote JSON: $JSON_OUT"
fi

if [[ "$REORG_OK" == "true" ]]; then
  say "🎉 P2P reorg test completed successfully"
  exit 0
else
  say "🔗 P2P reorg test failed - network consensus issues detected"
  exit 1
fi
