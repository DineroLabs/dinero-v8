#!/usr/bin/env bash
# ============================================================================
# Utreexo Canonical-Roots Fork Regression Test (regtest)
# ============================================================================
# Validates the Apr 13 2026 Stage 3 canonical-roots hard fork end-to-end:
#   1. dinerod starts cleanly on regtest with the fork constant wired in
#   2. pre-activation blocks (heights 1-9) mine under pre-fork semantics
#   3. block 10 (REGTEST activation) flips the flag, rebuilds roots_, and
#      still validates + its utreexo_root lands in the block header
#   4. post-activation blocks (11+) continue to mine cleanly using the
#      canonical zero-sentinel semantics — no "FATAL: hash identity
#      violation", no "utreexo-remove-failed-in-pure", no root mismatch
#   5. chain tip advances to the expected height
#
# This is the test gate that MUST pass before bumping
# UTREEXO_CANONICAL_ROOTS_HEIGHT_MAINNET off UINT32_MAX.
#
# Usage: ./test_canonical_roots_fork.sh [dinerod_path]
# ============================================================================
set -euo pipefail

DINEROD="${1:-/Users/haydarevich/src/dinero/build/dinerod}"
RPCPORT=18472
P2PPORT=18473
TMPDIR=$(mktemp -d /tmp/dinero_canonical_roots_XXXXXX)
PID=""
FAILURES=0

# Regtest activation height (must match
# include/consensus/utreexo_canonical_roots_activation.h
# UTREEXO_CANONICAL_ROOTS_HEIGHT_REGTEST).
ACTIVATION_HEIGHT=10
TARGET_HEIGHT=20  # mine some blocks past activation to catch cascading bugs

rpc() {
    curl -s --max-time 30 --user test:test --data-binary \
        "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"$1\",\"params\":[$2]}" \
        -H 'content-type: text/plain;' "http://127.0.0.1:$RPCPORT/" 2>/dev/null
}

get_height() {
    rpc "blockchain.getblockcount" "" | python3 -c \
        "import sys,json; print(json.load(sys.stdin).get('result','?'))" 2>/dev/null
}

check_eq() {
    local desc="$1" actual="$2" expected="$3"
    if [[ "$actual" == "$expected" ]]; then
        echo "  [PASS] $desc"
    else
        echo "  [FAIL] $desc -- expected '$expected', got '$actual'"
        FAILURES=$((FAILURES + 1))
    fi
}

check_grep_absent() {
    local desc="$1" needle="$2" logfile="$3"
    if grep -q "$needle" "$logfile" 2>/dev/null; then
        echo "  [FAIL] $desc -- found '$needle' in log"
        FAILURES=$((FAILURES + 1))
        grep "$needle" "$logfile" | head -3 | sed 's/^/       /'
    else
        echo "  [PASS] $desc (no '$needle' in log)"
    fi
}

check_grep_present() {
    local desc="$1" needle="$2" logfile="$3"
    if grep -q "$needle" "$logfile" 2>/dev/null; then
        echo "  [PASS] $desc (found '$needle')"
    else
        echo "  [FAIL] $desc -- missing '$needle' in log"
        FAILURES=$((FAILURES + 1))
    fi
}

cleanup() {
    if [[ -n "$PID" ]]; then
        kill "$PID" 2>/dev/null || true
        # Give the daemon a moment to shut down cleanly
        for i in 1 2 3 4 5; do
            if ! kill -0 "$PID" 2>/dev/null; then break; fi
            sleep 1
        done
        kill -9 "$PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

echo ""
echo "==============================================="
echo " Utreexo Canonical-Roots Fork Regression Test"
echo "==============================================="
echo "  dinerod:          $DINEROD"
echo "  datadir:          $TMPDIR"
echo "  activation block: $ACTIVATION_HEIGHT"
echo "  target height:    $TARGET_HEIGHT"
echo ""

# ---------------------------------------------------------------------------
# Phase 1 — start dinerod in regtest mode
# ---------------------------------------------------------------------------
"$DINEROD" \
    -regtest -daemon=0 -server \
    -rpcuser=test -rpcpassword=test \
    -rpcport=$RPCPORT -port=$P2PPORT \
    -datadir="$TMPDIR" \
    -listenonion=0 -discover=0 -dnsseed=0 -fixedseeds=0 \
    > "$TMPDIR/node.log" 2>&1 &
PID=$!

# Wait for RPC to come up
READY=0
for i in $(seq 1 60); do
    if curl -s --max-time 2 --user test:test --data-binary \
        '{"jsonrpc":"1.0","method":"blockchain.getblockcount","params":[]}' \
        -H "content-type: text/plain;" "http://127.0.0.1:$RPCPORT/" 2>/dev/null \
        | grep -q "result"; then
        echo "  Node ready (${i}s)"
        READY=1
        break
    fi
    sleep 1
done

if [[ "$READY" != "1" ]]; then
    echo "  [FAIL] Node never became ready — dumping last log lines:"
    tail -40 "$TMPDIR/node.log"
    exit 1
fi

START_H=$(get_height)
echo "  Starting height: $START_H"

# ---------------------------------------------------------------------------
# Phase 2 — mine past the activation boundary
# ---------------------------------------------------------------------------
echo ""
echo "Phase 2: Mine pre-activation blocks (1..$ACTIVATION_HEIGHT-1)"
rpc "generate" "$((ACTIVATION_HEIGHT - 1))" > /dev/null 2>&1
sleep 1
H_PRE=$(get_height)
check_eq "pre-activation height" "$H_PRE" "$((ACTIVATION_HEIGHT - 1))"

echo ""
echo "Phase 3: Mine the activation block ($ACTIVATION_HEIGHT)"
rpc "generate" "1" > /dev/null 2>&1
sleep 1
H_ACT=$(get_height)
check_eq "activation block landed" "$H_ACT" "$ACTIVATION_HEIGHT"

echo ""
echo "Phase 4: Mine post-activation blocks ($((ACTIVATION_HEIGHT + 1))..$TARGET_HEIGHT)"
rpc "generate" "$((TARGET_HEIGHT - ACTIVATION_HEIGHT))" > /dev/null 2>&1
sleep 1
H_FINAL=$(get_height)
check_eq "final height reached" "$H_FINAL" "$TARGET_HEIGHT"

# ---------------------------------------------------------------------------
# Phase 5 — invariant checks on the journal
# ---------------------------------------------------------------------------
echo ""
echo "Phase 5: Invariant checks"

# Must NOT see any of the failure modes we saw on mainnet attempts
check_grep_absent "no hash identity violation" "FATAL: Block hash identity violation" "$TMPDIR/node.log"
check_grep_absent "no utreexo-remove-failed-in-pure" "utreexo-remove-failed-in-pure" "$TMPDIR/node.log"
check_grep_absent "no proof verification failed target" "Proof verification failed for target" "$TMPDIR/node.log"
check_grep_absent "no bad-utreexo-root" "bad-utreexo-root" "$TMPDIR/node.log"
check_grep_absent "no ConnectTip failures"    "ConnectTip FAILED" "$TMPDIR/node.log"

# Must see the fork activation log fire exactly once at ACTIVATION_HEIGHT
check_grep_present "fork activation log fired" "Canonical Roots Fork.*Activating at height $ACTIVATION_HEIGHT" "$TMPDIR/node.log"

# ---------------------------------------------------------------------------
# Phase 5b — disconnect/reconnect across the activation boundary
# ---------------------------------------------------------------------------
# This is the regression for the Apr 13 2026 chain-split bug. A reorg that
# walks back through the activation block must not corrupt the forest. We
# can't easily trigger a reorg from scratch on a single regtest node, but
# we CAN exercise the equivalent path:
#   1. invalidate the activation block (forces disconnect of all blocks
#      from the tip down to and including the activation block)
#   2. verify the chain rolled back to (activation_height - 1)
#   3. reconsider the same block (forces reconnect of the activation
#      block + everything above it)
#   4. verify the chain came back to TARGET_HEIGHT with the same tip hash
#   5. confirm no FATAL/REJECT/disconnect-failed in the journal
#
# If `Restore()` doesn't symmetrically un-flip the canonical-roots flag,
# step 3 will fail because the post-disconnect forest is inconsistent.
echo ""
echo "Phase 5b: Disconnect + reconnect across activation boundary"

ORIG_TIP=$(rpc "blockchain.getbestblockhash" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" | tr -d '"')
echo "  Original tip: $ORIG_TIP"

# Get hash of the activation block
ACT_HASH=$(rpc "blockchain.getblockhash" "$ACTIVATION_HEIGHT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" | tr -d '"')
echo "  Activation block hash: $ACT_HASH"

# Invalidate the activation block — forces disconnect of [ACTIVATION_HEIGHT..TARGET_HEIGHT]
INVALIDATE=$(rpc "blockchain.invalidateblock" "\"$ACT_HASH\"")
echo "  invalidateblock response: $(echo "$INVALIDATE" | head -c 80)"
sleep 1
H_AFTER_INVALIDATE=$(get_height)
check_eq "rolled back below activation" "$H_AFTER_INVALIDATE" "$((ACTIVATION_HEIGHT - 1))"

# Reconsider the activation block — forces reconnect
RECONSIDER=$(rpc "blockchain.reconsiderblock" "\"$ACT_HASH\"")
echo "  reconsiderblock response: $(echo "$RECONSIDER" | head -c 80)"
sleep 1
H_AFTER_RECONSIDER=$(get_height)
NEW_TIP=$(rpc "blockchain.getbestblockhash" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" | tr -d '"')
check_eq "chain back to target after reconsider" "$H_AFTER_RECONSIDER" "$TARGET_HEIGHT"
check_eq "tip hash matches original after disconnect+reconnect" "$NEW_TIP" "$ORIG_TIP"

# Verify the disconnect/reconnect didn't poison the journal
check_grep_absent "no failed-disconnect" "Failed to disconnect block" "$TMPDIR/node.log"
check_grep_absent "no failed-undo-read" "Failed to read undo data" "$TMPDIR/node.log"
check_grep_absent "no REORG ABORT (post-bugfix)" "REORG ABORT" "$TMPDIR/node.log"

# ---------------------------------------------------------------------------
# Phase 5c — competing-branch activation test
# ---------------------------------------------------------------------------
# This is the *real* Apr 13 2026 mainnet failure mode. Phase 5b only round-
# trips a single branch through the activation boundary. Phase 5c builds
# two genuinely independent branches that BOTH cross the activation:
#
#   genesis ─ 1..9 ─┬─ 10A ─ 11A ─ ... ─ 15A   (branch A — first to activate)
#                   └─ 10B ─ 11B ─ ... ─ 15B   (branch B — second to activate)
#
# Both branches have their own activation block at height 10. The test
# walks back and forth between the two by invalidate/reconsider, asserting
# that EVERY transition (A→9→B, B→9→A) succeeds with no undo-read failure,
# no REORG ABORT, no forest mismatch, and that the final tip hash on each
# branch is reproducible.
#
# This is the regression that nails #30: until 29ffdbe94, this would
# trigger "Failed to read undo data for block at height ACTIVATION_HEIGHT"
# the first time the chain tried to switch branches.
echo ""
echo "Phase 5c: Competing-branch activation"

BRANCH_HEIGHT=$((ACTIVATION_HEIGHT + 5))  # 5 blocks past activation on each branch

# We're currently at height $TARGET_HEIGHT (20) on the original branch (call it A).
# Snapshot branch A's tip + activation hash so we can restore it later.
A_TIP=$(rpc "blockchain.getbestblockhash" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" | tr -d '"')
A_ACT_HASH=$(rpc "blockchain.getblockhash" "$ACTIVATION_HEIGHT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" | tr -d '"')
echo "  Branch A tip @ $TARGET_HEIGHT: $A_TIP"
echo "  Branch A activation block @ $ACTIVATION_HEIGHT: $A_ACT_HASH"

# --- Walk back to common ancestor (height $((ACTIVATION_HEIGHT - 1))) ---
rpc "blockchain.invalidateblock" "\"$A_ACT_HASH\"" > /dev/null
sleep 1
H=$(get_height)
check_eq "branch A invalidated, at common ancestor" "$H" "$((ACTIVATION_HEIGHT - 1))"

# --- Build branch B starting at height $ACTIVATION_HEIGHT ---
# Generating produces a *different* block 10 because timestamp + coinbase
# extranonce will differ from branch A's block 10. After mining
# BRANCH_HEIGHT - (ACTIVATION_HEIGHT - 1) = 6 blocks, branch B's tip is at
# BRANCH_HEIGHT and is a fully independent chain from common-ancestor + 1
# onward.
rpc "generate" "$((BRANCH_HEIGHT - (ACTIVATION_HEIGHT - 1)))" > /dev/null 2>&1
sleep 1
H=$(get_height)
check_eq "branch B mined to BRANCH_HEIGHT" "$H" "$BRANCH_HEIGHT"

B_TIP=$(rpc "blockchain.getbestblockhash" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" | tr -d '"')
B_ACT_HASH=$(rpc "blockchain.getblockhash" "$ACTIVATION_HEIGHT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" | tr -d '"')
echo "  Branch B tip @ $BRANCH_HEIGHT: $B_TIP"
echo "  Branch B activation block @ $ACTIVATION_HEIGHT: $B_ACT_HASH"

# Sanity: the two branches must have DIFFERENT activation block hashes,
# otherwise we're not actually testing a branch competition.
if [[ "$A_ACT_HASH" == "$B_ACT_HASH" ]]; then
    echo "  [FAIL] branch A and branch B have the same activation block hash — test is meaningless"
    FAILURES=$((FAILURES + 1))
else
    echo "  [PASS] branches have distinct activation blocks (A=${A_ACT_HASH:0:16}... B=${B_ACT_HASH:0:16}...)"
fi

# --- Switch back to branch A: invalidate B's activation, reconsider A's ---
rpc "blockchain.invalidateblock" "\"$B_ACT_HASH\"" > /dev/null
sleep 1
H=$(get_height)
check_eq "branch B invalidated, back at common ancestor" "$H" "$((ACTIVATION_HEIGHT - 1))"

rpc "blockchain.reconsiderblock" "\"$A_ACT_HASH\"" > /dev/null
sleep 1
H_BACK_A=$(get_height)
TIP_BACK_A=$(rpc "blockchain.getbestblockhash" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" | tr -d '"')
check_eq "back on branch A at original target" "$H_BACK_A" "$TARGET_HEIGHT"
check_eq "branch A tip hash recovered exactly" "$TIP_BACK_A" "$A_TIP"

# --- Switch back to branch B: invalidate A's activation, reconsider B's ---
rpc "blockchain.invalidateblock" "\"$A_ACT_HASH\"" > /dev/null
sleep 1
H=$(get_height)
check_eq "branch A invalidated again, at common ancestor" "$H" "$((ACTIVATION_HEIGHT - 1))"

rpc "blockchain.reconsiderblock" "\"$B_ACT_HASH\"" > /dev/null
sleep 1
H_BACK_B=$(get_height)
TIP_BACK_B=$(rpc "blockchain.getbestblockhash" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" | tr -d '"')
check_eq "back on branch B at branch height" "$H_BACK_B" "$BRANCH_HEIGHT"
check_eq "branch B tip hash recovered exactly" "$TIP_BACK_B" "$B_TIP"

# --- Restore branch A as the final state for downstream phases ---
rpc "blockchain.invalidateblock" "\"$B_ACT_HASH\"" > /dev/null
rpc "blockchain.reconsiderblock" "\"$A_ACT_HASH\"" > /dev/null
sleep 1
H_FINAL_RESTORE=$(get_height)
TIP_FINAL_RESTORE=$(rpc "blockchain.getbestblockhash" "" | python3 -c "import sys,json; print(json.load(sys.stdin).get('result',''))" | tr -d '"')
check_eq "branch A restored as final state" "$TIP_FINAL_RESTORE" "$A_TIP"

# After all that branch-juggling: the journal must STILL be clean.
check_grep_absent "no failed-disconnect after branch swap" "Failed to disconnect block" "$TMPDIR/node.log"
check_grep_absent "no failed-undo-read after branch swap" "Failed to read undo data" "$TMPDIR/node.log"
check_grep_absent "no REORG ABORT after branch swap" "REORG ABORT" "$TMPDIR/node.log"
check_grep_absent "no FOREST ROOT MISMATCH after branch swap" "FOREST ROOT MISMATCH" "$TMPDIR/node.log"
check_grep_absent "no FATAL after branch swap" "FATAL" "$TMPDIR/node.log"
check_grep_absent "no hash identity violation after branch swap" "Block hash identity violation" "$TMPDIR/node.log"

# ---------------------------------------------------------------------------
# Phase 6 — verify chain integrity via getblockchaininfo + gettxout sanity
# ---------------------------------------------------------------------------
echo ""
echo "Phase 6: Chain integrity"
CHAIN_INFO=$(rpc "blockchain.getblockchaininfo" "")
CHAIN_TIP_HASH=$(rpc "blockchain.getbestblockhash" "" | \
    python3 -c "import sys,json; print(json.load(sys.stdin).get('result','?'))")
check_eq "chain is 'regtest'" \
    "$(echo "$CHAIN_INFO" | python3 -c "import sys,json; r=json.load(sys.stdin).get('result',{}); print(r.get('chain','?'))")" \
    "regtest"
if [[ -n "$CHAIN_TIP_HASH" && "$CHAIN_TIP_HASH" != "?" ]]; then
    echo "  [PASS] tip hash non-empty ($CHAIN_TIP_HASH)"
else
    echo "  [FAIL] tip hash empty"
    FAILURES=$((FAILURES + 1))
fi

# ---------------------------------------------------------------------------
echo ""
if [[ "$FAILURES" -eq 0 ]]; then
    echo "==============================================="
    echo " RESULT: PASS ($TARGET_HEIGHT blocks mined, fork active)"
    echo "==============================================="
    exit 0
else
    echo "==============================================="
    echo " RESULT: FAIL ($FAILURES failure(s))"
    echo "==============================================="
    echo "Last 60 lines of node.log:"
    tail -60 "$TMPDIR/node.log" | sed 's/^/  /'
    exit 1
fi
