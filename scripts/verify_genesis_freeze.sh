#!/bin/bash
set -euo pipefail

echo "🔍 DINERO GENESIS FREEZE VERIFICATION"
echo "===================================="

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

# Expected constants from genesis_print
EXPECTED_GENESIS="0527307a952837a2bef8f1f709803506a8c00ad4ce817646a67b2f9bd8d0c76b"
EXPECTED_MERKLE="abcf15b08679499b542d4f33660205e53d9f4d64355d48a94664498a114855c7"
EXPECTED_PREMINE_SCRIPT="00142e662f068292d576957452939515b8435ff4b2a9"
EXPECTED_PREMINE_ADDRESS="din1q9enz7p5zjt2hd9t522fe29dcgd0lfv4f8n374k"

echo "🎯 Expected Genesis Constants:"
echo "  Genesis Hash: $EXPECTED_GENESIS"
echo "  Merkle Root:  $EXPECTED_MERKLE"
echo "  Premine Script: $EXPECTED_PREMINE_SCRIPT"
echo "  Premine Address: $EXPECTED_PREMINE_ADDRESS"
echo

# 1. Verify genesis_print output matches expected values
echo "🧪 Testing genesis_print consistency..."
GENESIS_OUTPUT=$(./build/genesis_print)

if echo "$GENESIS_OUTPUT" | grep -q "$EXPECTED_GENESIS"; then
    echo "✅ Genesis hash matches"
else
    echo "❌ Genesis hash mismatch!"
    exit 1
fi

if echo "$GENESIS_OUTPUT" | grep -q "$EXPECTED_MERKLE"; then
    echo "✅ Merkle root matches"
else
    echo "❌ Merkle root mismatch!"
    exit 1
fi

if echo "$GENESIS_OUTPUT" | grep -q "$EXPECTED_PREMINE_SCRIPT"; then
    echo "✅ Premine script matches"
else
    echo "❌ Premine script mismatch!"
    exit 1
fi

if echo "$GENESIS_OUTPUT" | grep -q "$EXPECTED_PREMINE_ADDRESS"; then
    echo "✅ Premine address matches"
else
    echo "❌ Premine address mismatch!"
    exit 1
fi

# 2. Verify chainparams contains the frozen constants
echo
echo "🔒 Verifying chainparams freeze..."

if grep -q "$EXPECTED_GENESIS" src/consensus/chainparams.cpp; then
    echo "✅ Genesis hash frozen in chainparams"
else
    echo "❌ Genesis hash not found in chainparams!"
    exit 1
fi

if grep -q "$EXPECTED_MERKLE" src/consensus/chainparams.cpp; then
    echo "✅ Merkle root frozen in chainparams"
else
    echo "❌ Merkle root not found in chainparams!"
    exit 1
fi

# 3. Verify assertions are present
if grep -q "assert.*$EXPECTED_GENESIS" src/consensus/chainparams.cpp; then
    echo "✅ Genesis hash assertion present"
else
    echo "❌ Genesis hash assertion missing!"
    exit 1
fi

if grep -q "assert.*$EXPECTED_MERKLE" src/consensus/chainparams.cpp; then
    echo "✅ Merkle root assertion present"
else
    echo "❌ Merkle root assertion missing!"
    exit 1
fi

# 4. Verify checkpoint bytes are correct
echo
echo "🔍 Verifying checkpoint bytes..."
CHECKPOINT_PATTERN="0x05, 0x27, 0x30, 0x7a"
if grep -q "$CHECKPOINT_PATTERN" src/consensus/chainparams.cpp; then
    echo "✅ Checkpoint bytes match genesis hash"
else
    echo "❌ Checkpoint bytes mismatch!"
    exit 1
fi

# 5. Verify network ports are production-ready
echo
echo "🌐 Verifying network configuration..."
if grep -q "p2p_port = 40999" src/consensus/chainparams.cpp; then
    echo "✅ P2P port set to 40999"
else
    echo "❌ P2P port not set correctly!"
    exit 1
fi

if grep -q "rpc_port = 20999" src/consensus/chainparams.cpp; then
    echo "✅ RPC port set to 20999"
else
    echo "❌ RPC port not set correctly!"
    exit 1
fi

# 6. Verify currency system consistency
echo
echo "🪙 Verifying currency system..."
if ./build/test_currency_system > /dev/null 2>&1; then
    echo "✅ Currency system tests pass"
else
    echo "❌ Currency system tests failed!"
    exit 1
fi

# 7. Verify premine structure
echo
echo "💰 Verifying premine structure..."
PREMINE_COUNT=$(echo "$GENESIS_OUTPUT" | grep -c "Output [0-3]: 500000.000000 DIN")
if [ "$PREMINE_COUNT" -eq 4 ]; then
    echo "✅ Exactly 4 premine outputs of 500K DIN each"
else
    echo "❌ Premine structure incorrect! Found $PREMINE_COUNT outputs"
    exit 1
fi

if echo "$GENESIS_OUTPUT" | grep -q "Total Premine: 2000000.000000 DIN"; then
    echo "✅ Total premine is 2M DIN"
else
    echo "❌ Total premine amount incorrect!"
    exit 1
fi

# 8. Verify launch documentation exists
echo
echo "📋 Verifying launch documentation..."
if [ -f "docs/launch/GENESIS_FREEZE.md" ]; then
    echo "✅ Genesis freeze documentation exists"
else
    echo "❌ Genesis freeze documentation missing!"
    exit 1
fi

if [ -f "docs/launch/genesis_constants.txt" ]; then
    echo "✅ Genesis constants archived"
else
    echo "❌ Genesis constants archive missing!"
    exit 1
fi

echo
echo "🎉 ALL GENESIS FREEZE VERIFICATIONS PASSED!"
echo "✅ Genesis constants are locked and consistent"
echo "✅ Chainparams assertions are in place"
echo "✅ Network configuration is production-ready"
echo "✅ Currency system is standardized"
echo "✅ Premine structure is correct"
echo "✅ Documentation is complete"
echo
echo "🔒 DINERO GENESIS IS PRODUCTION FROZEN!"
echo "Ready for mainnet launch with GO_LIVE.sh"
