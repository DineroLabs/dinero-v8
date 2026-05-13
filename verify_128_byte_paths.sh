#!/bin/bash
# Phase 3: Verify all code paths use 128-byte BlockHeader v1

echo "=== Phase 3: 128-Byte Header Verification ==="
echo ""

# 1. Check BlockHeader struct size
echo "1. BlockHeader struct definition:"
grep -A 2 "sizeof(BlockHeader)" /Users/haydarevich/Documents/DineroCoin/include/primitives/block.h | head -3
echo ""

# 2. Check header size constant
echo "2. Header size constant:"
grep "CURRENT_BLOCK_HEADER_SIZE = " /Users/haydarevich/Documents/DineroCoin/include/consensus/header_consensus.h
echo ""

# 3. Check consensus validation
echo "3. Consensus validation (block_validation.cpp):"
grep -A 1 "All blocks MUST use" /Users/haydarevich/Documents/DineroCoin/src/consensus/block_validation.cpp | head -2
echo ""

# 4. Check mining engine
echo "4. Mining engine serialization:"
grep -A 2 "SerializeForHash\(\)" /Users/haydarevich/Documents/DineroCoin/src/mining/miner_engine.cpp | head -3
echo ""

# 5. Check getblocktemplate
echo "5. getblocktemplate RPC:"
grep "result\[\"difficulty\"\]" /Users/haydarevich/Documents/DineroCoin/src/rpc/methods_mining_template.cpp | head -1
grep "result\[\"reserved\"\]" /Users/haydarevich/Documents/DineroCoin/src/rpc/methods_mining_template.cpp | head -1
echo ""

# 6. Check for any remaining 80/112 references
echo "6. Checking for legacy size references..."
LEGACY_REFS=$(grep -rn "80\|112" /Users/haydarevich/Documents/DineroCoin/src/consensus/*.cpp /Users/haydarevich/Documents/DineroCoin/src/mining/*.cpp 2>/dev/null | grep -i "byte\|size" | grep -v "comment" | grep -v "//" || true)
if [ -z "$LEGACY_REFS" ]; then
    echo "   ✅ No legacy 80/112-byte references in consensus/mining code"
else
    echo "   ⚠️  Found legacy references:"
    echo "$LEGACY_REFS"
fi
echo ""

echo "=== Verification Complete ==="
