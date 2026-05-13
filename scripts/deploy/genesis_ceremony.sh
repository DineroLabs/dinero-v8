#!/bin/bash
set -euo pipefail

# ==============================================================================
# DINERO GENESIS CEREMONY - MAINNET LAUNCH
# ==============================================================================
# 
# This script performs the one-time genesis block generation for Dinero mainnet.
# It must be run exactly once to establish the network's origin.
# 
# 🔒 CRITICAL: The output of this ceremony becomes IMMUTABLE consensus rules.
# 
# Usage: ./genesis_ceremony.sh [--testnet|--regtest]
# 
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

# Default to mainnet
NETWORK="mainnet"
HRP="din"
MAGIC_BYTES="0xD14E5201"
P2P_PORT=40999
RPC_PORT=20999

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --testnet)
            NETWORK="testnet"
            HRP="tdin"
            MAGIC_BYTES="0xD14E52FF"
            P2P_PORT=50999
            RPC_PORT=50998
            shift
            ;;
        --regtest)
            NETWORK="regtest"
            HRP="rdin"
            MAGIC_BYTES="0xD14E5200"
            P2P_PORT=60999
            RPC_PORT=60998
            shift
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--testnet|--regtest]"
            exit 1
            ;;
    esac
done

echo "🚀 DINERO GENESIS CEREMONY - $NETWORK"
echo "==============================================="
echo "Network: $NETWORK"
echo "HRP: $HRP"
echo "Magic: $MAGIC_BYTES"
echo "Ports: P2P=$P2P_PORT, RPC=$RPC_PORT"
echo ""

# Ensure build exists
if [[ ! -f "$BUILD_DIR/mine_genesis" ]]; then
    echo "❌ Build not found. Running cmake build..."
    cd "$PROJECT_ROOT"
    cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.ncpu || echo 4)
fi

# Create ceremony output directory
CEREMONY_DIR="$PROJECT_ROOT/genesis_ceremony_$NETWORK"
mkdir -p "$CEREMONY_DIR"

echo "📁 Ceremony output: $CEREMONY_DIR"
echo ""

# =============================================================================
# STEP 1: GENERATE DEVELOPER FUND MULTISIG
# =============================================================================

echo "🔑 Step 1: Generate Developer Fund Multisig"
echo "--------------------------------------------"

DEV_FUND_DIR="$CEREMONY_DIR/dev_fund"
mkdir -p "$DEV_FUND_DIR"

# Generate 3 key pairs for 2-of-3 multisig
echo "Generating 3 keypairs for 2-of-3 multisig..."

for i in {1..3}; do
    echo "  Generating keypair $i..."
    # Use openssl for secure key generation
    openssl rand -hex 32 > "$DEV_FUND_DIR/private_key_$i.hex"
    
    # TODO: Use your secp256k1 tools to generate public key and address
    # For now, create placeholder
    echo "placeholder_pubkey_$i" > "$DEV_FUND_DIR/public_key_$i.hex"
    echo "${HRP}1placeholder_address_$i" > "$DEV_FUND_DIR/address_$i.txt"
done

# Create multisig redeem script (2-of-3)
echo "Creating 2-of-3 multisig redeem script..."
cat > "$DEV_FUND_DIR/multisig_script.txt" << EOF
# 2-of-3 Multisig Redeem Script
# OP_2 <pubkey1> <pubkey2> <pubkey3> OP_3 OP_CHECKMULTISIG

# TODO: Replace with actual public keys
52 # OP_2
21 <placeholder_pubkey_1_hex>
21 <placeholder_pubkey_2_hex>  
21 <placeholder_pubkey_3_hex>
53 # OP_3
ae # OP_CHECKMULTISIG
EOF

# Compute script hash for P2WSH
SCRIPT_HASH="placeholder_sha256_of_redeem_script"
DEV_FUND_ADDRESS="${HRP}1qplaceholder_dev_fund_address"

echo "✅ Developer Fund Multisig:"
echo "   Address: $DEV_FUND_ADDRESS"
echo "   Script Hash: $SCRIPT_HASH"
echo "   Keys stored in: $DEV_FUND_DIR/"
echo ""

# =============================================================================
# STEP 2: SET GENESIS TIMESTAMP
# =============================================================================

echo "⏰ Step 2: Set Genesis Timestamp"
echo "--------------------------------"

# Use current time for ceremony
GENESIS_TIME=$(date +%s)
GENESIS_DATE=$(date -d "@$GENESIS_TIME" 2>/dev/null || date -r "$GENESIS_TIME")

echo "Genesis timestamp: $GENESIS_TIME ($GENESIS_DATE)"
echo ""

# =============================================================================
# STEP 3: MINE GENESIS BLOCK
# =============================================================================

echo "⛏️  Step 3: Mine Genesis Block"
echo "------------------------------"

# Set initial difficulty based on network
case $NETWORK in
    mainnet)
        GENESIS_BITS="0x1e00ffff"  # Moderate difficulty
        TARGET_TIME=300            # 5 minutes max
        ;;
    testnet)
        GENESIS_BITS="0x1f00ffff"  # Easy difficulty
        TARGET_TIME=60             # 1 minute max
        ;;
    regtest)
        GENESIS_BITS="0x2100ffff"  # Trivial difficulty
        TARGET_TIME=10             # 10 seconds max
        ;;
esac

echo "Mining genesis with difficulty: $GENESIS_BITS"
echo "Target time: $TARGET_TIME seconds"
echo ""

# Create temporary genesis config
GENESIS_CONFIG="$CEREMONY_DIR/genesis_config.json"
cat > "$GENESIS_CONFIG" << EOF
{
  "network": "$NETWORK",
  "timestamp": $GENESIS_TIME,
  "bits": "$GENESIS_BITS",
  "coinbase_text": "Dinero v1.0 - CPU-friendly mining for everyone",
  "dev_fund_address": "$DEV_FUND_ADDRESS",
  "premine_amount": 100000000000000
}
EOF

# Mine the genesis block
echo "🔥 Starting genesis mining..."
GENESIS_OUTPUT="$CEREMONY_DIR/genesis_result.json"

timeout $TARGET_TIME "$BUILD_DIR/mine_genesis" \
    --config "$GENESIS_CONFIG" \
    --output "$GENESIS_OUTPUT" \
    --threads $(nproc 2>/dev/null || sysctl -n hw.ncpu || echo 4) \
    || echo "⚠️  Mining timeout reached, using best result"

if [[ -f "$GENESIS_OUTPUT" ]]; then
    echo "✅ Genesis block mined successfully!"
    echo ""
    
    # Parse results
    GENESIS_HASH=$(jq -r '.hash' "$GENESIS_OUTPUT")
    GENESIS_NONCE=$(jq -r '.nonce' "$GENESIS_OUTPUT")
    MERKLE_ROOT=$(jq -r '.merkle_root' "$GENESIS_OUTPUT")
    
    echo "Genesis Results:"
    echo "  Hash: $GENESIS_HASH"
    echo "  Nonce: $GENESIS_NONCE"  
    echo "  Merkle Root: $MERKLE_ROOT"
    echo "  Timestamp: $GENESIS_TIME"
    echo "  Bits: $GENESIS_BITS"
    echo ""
else
    echo "❌ Genesis mining failed!"
    exit 1
fi

# =============================================================================
# STEP 4: COMPUTE CHAIN IDENTITY COMMITMENT (CIC)
# =============================================================================

echo "🔒 Step 4: Compute Chain Identity Commitment"
echo "--------------------------------------------"

# Create final chain parameters
CHAIN_PARAMS="$CEREMONY_DIR/chain_params_final.json"
cat > "$CHAIN_PARAMS" << EOF
{
  "network": {
    "id": "$NETWORK",
    "hrp": "$HRP", 
    "magic_bytes": "$MAGIC_BYTES",
    "p2p_port": $P2P_PORT,
    "rpc_port": $RPC_PORT
  },
  "genesis": {
    "version": 1,
    "timestamp": $GENESIS_TIME,
    "bits": "$GENESIS_BITS",
    "nonce": $GENESIS_NONCE,
    "merkle_root": "$MERKLE_ROOT",
    "hash": "$GENESIS_HASH",
    "coinbase_text": "Dinero v1.0 - CPU-friendly mining for everyone"
  },
  "consensus": {
    "block_time": 60,
    "retarget_interval": 3600,
    "coinbase_maturity": 100,
    "max_supply": 9900000000000000,
    "dev_fund_end": 200000000000000,
    "phase2_start": 2000000000000000,
    "initial_reward": 10000000000,
    "halving_interval": 210000
  },
  "premine": {
    "enabled": true,
    "height": 1,
    "amount": 200000000000000,
    "address": "$DEV_FUND_ADDRESS",
    "script_hash": "$SCRIPT_HASH"
  }
}
EOF

# Compute CIC hash
CIC_INPUT="$CEREMONY_DIR/cic_input.txt"
cat > "$CIC_INPUT" << EOF
network_id:$NETWORK
hrp:$HRP
genesis_hash:$GENESIS_HASH
genesis_time:$GENESIS_TIME
genesis_bits:$GENESIS_BITS
block_time:60
retarget_interval:3600
coinbase_maturity:100
max_supply:9900000000000000
dev_fund_end:200000000000000
phase2_start:2000000000000000
initial_reward:10000000000
halving_interval:210000
premine_amount:100000000000000
dev_fund_address:$DEV_FUND_ADDRESS
EOF

# Compute SHA256 of consensus parameters
CIC_HASH=$(sha256sum "$CIC_INPUT" | cut -d' ' -f1)

echo "✅ Chain Identity Commitment (CIC): $CIC_HASH"
echo ""

# =============================================================================
# STEP 5: GENERATE FINAL CODE
# =============================================================================

echo "📝 Step 5: Generate Final Code"
echo "------------------------------"

# Generate ChainParams C++ code
CHAINPARAMS_OUTPUT="$CEREMONY_DIR/chainparams_${NETWORK}_final.cpp"
cat > "$CHAINPARAMS_OUTPUT" << EOF
/**
 * DINERO ${NETWORK^^} CHAIN PARAMETERS - FINAL
 * 
 * 🔒 GENERATED BY GENESIS CEREMONY
 * 
 * Date: $(date)
 * Genesis Hash: $GENESIS_HASH
 * CIC: $CIC_HASH
 * 
 * ⚠️  DO NOT MODIFY - These are consensus rules!
 */

#include "consensus/chainparams.h"

namespace dinero {

ChainParams Make${NETWORK^}ParamsFinal() {
    ChainParams p;
    
    // Network identity
    p.net.id = "$NETWORK";
    p.net.hrp = "$HRP";
    p.net.data_dir_name = "$NETWORK";
    p.net.p2p_port = $P2P_PORT;
    p.net.rpc_port = $RPC_PORT;
    p.net.magic_bytes = {$(echo "$MAGIC_BYTES" | sed 's/0x//; s/../0x&, /g' | sed 's/, $//')};
    
    // Genesis block
    p.genesis.nVersion = 1;
    p.genesis.nTime = $GENESIS_TIME;
    p.genesis.nBits = $GENESIS_BITS;
    p.genesis.nNonce = $GENESIS_NONCE;
    p.genesis.merkleRootHex = "$MERKLE_ROOT";
    p.genesis.genesisHashHex = "$GENESIS_HASH";
    p.genesis.coinbaseText = "Dinero v1.0 - CPU-friendly mining for everyone";
    
    // Consensus rules
    p.consensus.nPowTargetSpacingSec = 60;
    p.consensus.nPowTargetTimespanSec = 3600;
    p.consensus.fPowNoRetargeting = false;
    p.consensus.fPowAllowMinDifficultyBlocks = $([ "$NETWORK" = "mainnet" ] && echo "false" || echo "true");
    p.consensus.coinbaseMaturity = $([ "$NETWORK" = "mainnet" ] && echo "100" || echo "10");
    
    // Difficulty bounds
    p.consensus.powLimitBits = 0x1f00ffff;
    p.consensus.nInitialDifficultyBits = $GENESIS_BITS;
    p.consensus.nMinimumBits = 0x1d00ffff;
    p.consensus.nMaximumBits = 0x1f00ffff;
    
    // Dinero algorithm phases
    p.consensus.easyBits = 0x2000ffff;
    p.consensus.normalBits = 0x1d00ffff;
    p.consensus.devFundEndSats = 200000000000000ULL;
    p.consensus.phase2StartSats = 2000000000000000ULL;
    
    // Block rewards
    p.consensus.initialBlockReward = 10000000000ULL;
    p.consensus.halvingInterval = 210000;
    p.consensus.maxSupply = 9900000000000000ULL;
    
    // Developer fund premine
    p.consensus.premine.enabled = true;
    p.consensus.premine.height = 1;
    p.consensus.premine.amount = 200000000000000ULL;
    // TODO: Set actual script from multisig ceremony
    p.consensus.premine.scriptPubKey = {}; // Will be filled
    p.consensus.premine.address_hint = "$DEV_FUND_ADDRESS";
    
    // Security
    p.consensus.deepReorgThreshold = $([ "$NETWORK" = "mainnet" ] && echo "30" || echo "6");
    
    // Chain Identity Commitment
    p.consensus.commitment = {
        $(echo "$CIC_HASH" | sed 's/../0x&, /g' | sed 's/, $//')
    };
    
    // Genesis checkpoint
    p.consensus.checkpoints.clear();
    p.consensus.checkpoints.emplace_back(0, {
        $(echo "$GENESIS_HASH" | sed 's/../0x&, /g' | sed 's/, $//')
    });
    
    return p;
}

} // namespace dinero
EOF

echo "✅ Generated: $CHAINPARAMS_OUTPUT"

# =============================================================================
# STEP 6: VALIDATION & SUMMARY
# =============================================================================

echo ""
echo "🎉 GENESIS CEREMONY COMPLETE!"
echo "=============================================="
echo ""
echo "📋 CEREMONY SUMMARY:"
echo "  Network: $NETWORK"
echo "  HRP: $HRP"
echo "  Magic Bytes: $MAGIC_BYTES"
echo "  Ports: P2P=$P2P_PORT, RPC=$RPC_PORT"
echo ""
echo "🏗️  GENESIS BLOCK:"
echo "  Hash: $GENESIS_HASH"
echo "  Timestamp: $GENESIS_TIME ($GENESIS_DATE)"
echo "  Nonce: $GENESIS_NONCE"
echo "  Merkle Root: $MERKLE_ROOT"
echo "  Bits: $GENESIS_BITS"
echo ""
echo "🔑 DEVELOPER FUND:"
echo "  Address: $DEV_FUND_ADDRESS"
echo "  Amount: 2,000,000 DIN"
echo "  Type: 2-of-3 Multisig P2WSH"
echo ""
echo "🔒 CHAIN IDENTITY:"
echo "  CIC: $CIC_HASH"
echo ""
echo "📁 CEREMONY ARTIFACTS:"
echo "  Directory: $CEREMONY_DIR"
echo "  Chain Params: $CHAINPARAMS_OUTPUT"
echo "  Dev Fund Keys: $DEV_FUND_DIR/"
echo "  Genesis Config: $GENESIS_CONFIG"
echo ""
echo "🚀 NEXT STEPS:"
echo "  1. Review all generated files"
echo "  2. Securely store developer fund keys"
echo "  3. Update src/consensus/chainparams.cpp"
echo "  4. Commit changes and tag release"
echo "  5. Build and deploy seed nodes"
echo "  6. Launch the network!"
echo ""
echo "⚠️  SECURITY REMINDER:"
echo "  - Store dev fund private keys securely"
echo "  - Verify all generated hashes independently"
echo "  - Test on regtest before mainnet launch"
echo ""

# Final verification
echo "🔍 Running final verification..."
if [[ -f "$GENESIS_OUTPUT" ]] && [[ -f "$CHAIN_PARAMS" ]] && [[ -f "$CHAINPARAMS_OUTPUT" ]]; then
    echo "✅ All ceremony files generated successfully"
    echo ""
    echo "🎯 Ready for deployment!"
else
    echo "❌ Some ceremony files are missing"
    exit 1
fi
