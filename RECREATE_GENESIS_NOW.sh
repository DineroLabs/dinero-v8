#!/bin/bash
# Quick script to recreate genesis with correct economics

set -e
cd /Users/haydarevich/Documents/DineroCoin

echo "🚀 RECREATING GENESIS WITH LOCKED ECONOMICS"
echo "═══════════════════════════════════════════════════════════════"
echo ""
echo "✅ Economics Locked:"
echo "   - Height 0: 99 DIN (burned)"
echo "   - Height 1: 1,000,000 DIN (spendable)"
echo "   - Total: 97,850,000 DIN"
echo ""
echo "📋 Steps:"
echo "   1. Add backward compatibility aliases"
echo "   2. Build genesis miner"
echo "   3. Mine new genesis (height 0)"
echo "   4. Mine premine block (height 1)"
echo "   5. Update simple_blockchain.cpp with new hashes"
echo ""
echo "⏱️  Estimated time: 15-20 minutes"
echo ""
read -p "Ready to proceed? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 1
fi

echo ""
echo "Step 1: Backward compatibility aliases added ✅"
echo ""
echo "Step 2: Building..."
cmake --build build -j8 --target genesis_miner 2>&1 | tail -10

if [ -f "build/bin/genesis_miner" ] || [ -f "build/tools/genesis_miner" ]; then
    echo "✅ genesis_miner built successfully"
else
    echo "❌ genesis_miner not found. Check build output."
    exit 1
fi

echo ""
echo "Step 3: Mine genesis (height 0 - 99 DIN burned)..."
echo "  This may take 5-10 minutes depending on difficulty..."
echo ""
echo "Run this command:"
echo "  ./build/bin/genesis_miner --bits 0x2100ffff --reward 9900000000 --message 'Dinero Genesis - 99 DIN burned'"
echo ""
echo "Step 4: Mine premine (height 1 - 1M DIN spendable)..."
echo "  After genesis is mined, run:"
echo "  ./build/bin/genesis_miner --height 1 --prev-hash <genesis_hash> --bits 0x2100ffff --reward 100000000000000 --address din1q0gqj8ush5026e5q97jkw2mhxg3sngjjlxcpjzn"
echo ""
echo "Step 5: Update simple_blockchain.cpp with new hashes"
echo "  Copy the output from steps 3 & 4 into:"
echo "  - src/daemon/simple_blockchain.cpp::create_genesis_block()"
echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "✅ Setup complete! Follow steps 3-5 above."
echo ""
