#!/bin/bash
# ==============================================================================
# DINERO SIMPLE GENESIS INTEGRATION
# ==============================================================================
#
# Simple integration of P2WPKH premine into existing genesis
# Uses the production keys from HLC_Drive
#
# ==============================================================================

set -Eeuo pipefail
trap 'code=$?; echo "[FATAL] $0 failed at line $LINENO (exit $code)"; exit $code' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "🏗️ DINERO SIMPLE GENESIS INTEGRATION"
echo "===================================="
echo "Timestamp: $(date)"
echo ""

# Read production premine parameters from HLC_Drive
SECURE_DIR="/Volumes/HLC_Drive/dinero_secure"
if [[ ! -f "$SECURE_DIR/keys/public_info.txt" ]]; then
    echo "❌ Public info file not found"
    echo "Please run generate_final_premine.sh first"
    exit 1
fi

echo "📖 Reading premine parameters..."

# Extract parameters
COMPRESSED_PUBKEY=$(grep "Compressed Public Key:" "$SECURE_DIR/keys/public_info.txt" | cut -d: -f2 | tr -d ' ')
SCRIPT_PUBKEY_HEX=$(grep "P2WPKH scriptPubKey:" "$SECURE_DIR/keys/public_info.txt" | cut -d: -f2 | tr -d ' ')
BECH32_ADDRESS=$(grep "Bech32 Address:" "$SECURE_DIR/keys/public_info.txt" | cut -d: -f2 | tr -d ' ')

echo "✅ Production premine parameters:"
echo "  Compressed pubkey: $COMPRESSED_PUBKEY"
echo "  scriptPubKey: $SCRIPT_PUBKEY_HEX"
echo "  Address: $BECH32_ADDRESS"

# =============================================================================
# 1. CREATE GENESIS COINBASE CODE
# =============================================================================

echo ""
echo "🏗️ 1. CREATING GENESIS COINBASE CODE"
echo "===================================="
echo ""

# Create the C++ code snippet for manual integration
cat > "$SECURE_DIR/docs/genesis_coinbase_integration.cpp" << EOF
// Dinero P2WPKH Premine Genesis Integration
// Generated: $(date)
// 
// Add this code to your genesis coinbase creation in chainparams.cpp

// P2WPKH premine parameters
const std::string PREMINE_SCRIPT_HEX = "$SCRIPT_PUBKEY_HEX";
const std::string PREMINE_ADDRESS = "$BECH32_ADDRESS";
const std::string PREMINE_DESCRIPTOR = "wpkh($COMPRESSED_PUBKEY)";

// Create premine outputs for genesis coinbase
std::vector<unsigned char> script_bytes = ParseHex(PREMINE_SCRIPT_HEX);
CScript premine_script(script_bytes.begin(), script_bytes.end());

// Create 4 outputs of 500,000 DIN each
const int64_t PREMINE_OUTPUT_AMOUNT = 500000 * 100000000LL;  // 500k DIN in una

// Add to genesis coinbase transaction
for (int i = 0; i < 4; i++) {
    CTxOut premine_output;
    premine_output.nValue = PREMINE_OUTPUT_AMOUNT;
    premine_output.scriptPubKey = premine_script;
    genesis_coinbase.vout.push_back(premine_output);
}

// Total premine: 2,000,000 DIN
// Descriptor for wallet import: wpkh($COMPRESSED_PUBKEY)
// Address: $BECH32_ADDRESS
EOF

echo "✅ Genesis coinbase integration code created"

# =============================================================================
# 2. CREATE VERIFICATION SCRIPT
# =============================================================================

echo ""
echo "🔍 2. CREATING VERIFICATION SCRIPT"
echo "================================="
echo ""

# Create verification script
cat > "$SECURE_DIR/docs/verify_genesis_integration.py" << EOF
#!/usr/bin/env python3
"""
Verify Genesis Integration
Generated: $(date)
"""

import hashlib

# Production parameters
COMPRESSED_PUBKEY = "$COMPRESSED_PUBKEY"
EXPECTED_SCRIPT_PUBKEY = "$SCRIPT_PUBKEY_HEX"
EXPECTED_ADDRESS = "$BECH32_ADDRESS"

def verify_integration():
    print("🔍 VERIFYING GENESIS INTEGRATION")
    print("=" * 35)
    
    # Verify P2WPKH script
    pub_bytes = bytes.fromhex(COMPRESSED_PUBKEY)
    sha256_hash = hashlib.sha256(pub_bytes).digest()
    
    try:
        ripemd160_hash = hashlib.new('ripemd160', sha256_hash).digest()
    except ValueError:
        from Crypto.Hash import RIPEMD
        ripemd160_hash = RIPEMD.new(sha256_hash).digest()
    
    script_pubkey = "0014" + ripemd160_hash.hex()
    
    print(f"Compressed pubkey: {COMPRESSED_PUBKEY}")
    print(f"Computed script:   {script_pubkey}")
    print(f"Expected script:   {EXPECTED_SCRIPT_PUBKEY}")
    
    if script_pubkey == EXPECTED_SCRIPT_PUBKEY:
        print("✅ Script verification PASSED")
    else:
        print("❌ Script verification FAILED")
        return False
    
    print()
    print("📊 Genesis Structure:")
    print("  Outputs: 4")
    print("  Amount per output: 500,000 DIN")
    print("  Total premine: 2,000,000 DIN")
    print(f"  Script: {EXPECTED_SCRIPT_PUBKEY}")
    print(f"  Address: {EXPECTED_ADDRESS}")
    
    print()
    print("🚀 READY FOR MANUAL INTEGRATION!")
    return True

if __name__ == "__main__":
    verify_integration()
EOF

chmod +x "$SECURE_DIR/docs/verify_genesis_integration.py"

# =============================================================================
# 3. CREATE INTEGRATION INSTRUCTIONS
# =============================================================================

echo ""
echo "📋 3. CREATING INTEGRATION INSTRUCTIONS"
echo "======================================="
echo ""

cat > "$SECURE_DIR/docs/INTEGRATION_INSTRUCTIONS.md" << EOF
# Dinero P2WPKH Premine Integration Instructions

Generated: $(date)

## Manual Integration Steps

### 1. Locate Genesis Coinbase Creation

Find the genesis coinbase creation in your chainparams file (likely \`src/consensus/chainparams.cpp\` or similar).

### 2. Add Premine Code

Add this code where the genesis coinbase transaction is created:

\`\`\`cpp
// P2WPKH premine integration
const std::string PREMINE_SCRIPT_HEX = "$SCRIPT_PUBKEY_HEX";
std::vector<unsigned char> script_bytes = ParseHex(PREMINE_SCRIPT_HEX);
CScript premine_script(script_bytes.begin(), script_bytes.end());

// Create 4 outputs of 500,000 DIN each
const int64_t PREMINE_OUTPUT_AMOUNT = 500000 * 100000000LL;  // 500k DIN in una

for (int i = 0; i < 4; i++) {
    CTxOut premine_output;
    premine_output.nValue = PREMINE_OUTPUT_AMOUNT;
    premine_output.scriptPubKey = premine_script;
    genesis_coinbase.vout.push_back(premine_output);
}
\`\`\`

### 3. Verify Integration

Run the verification script:
\`\`\`bash
python3 $SECURE_DIR/docs/verify_genesis_integration.py
\`\`\`

### 4. Build and Test

\`\`\`bash
cmake --build build -j4
\`\`\`

### 5. Run Genesis Ceremony

After integration, run your genesis ceremony to create the final genesis block.

## Premine Details

- **Compressed Pubkey**: \`$COMPRESSED_PUBKEY\`
- **scriptPubKey**: \`$SCRIPT_PUBKEY_HEX\`
- **Address**: \`$BECH32_ADDRESS\`
- **Descriptor**: \`wpkh($COMPRESSED_PUBKEY)\`
- **Total Amount**: 2,000,000 DIN (4 outputs of 500,000 DIN each)

## Wallet Import

When you need to spend the premine:

\`\`\`bash
dinero-cli importdescriptors '[{"desc":"wpkh($COMPRESSED_PUBKEY)","timestamp":"now","label":"premine"}]'
\`\`\`

## Security Notes

- Private keys are encrypted in: \`$SECURE_DIR/keys/dinero_premine_keys.enc\`
- Use \`$SECURE_DIR/docs/decrypt_keys.sh\` for emergency access
- Make additional encrypted backups before mainnet launch
EOF

echo "✅ Integration instructions created"

# =============================================================================
# 4. RUN VERIFICATION
# =============================================================================

echo ""
echo "🔍 4. RUNNING VERIFICATION"
echo "========================="
echo ""

python3 "$SECURE_DIR/docs/verify_genesis_integration.py"

# =============================================================================
# 5. SUMMARY
# =============================================================================

echo ""
echo "🎯 SIMPLE GENESIS INTEGRATION COMPLETE"
echo "======================================"
echo ""

echo "📋 FILES CREATED:"
echo "  📄 $SECURE_DIR/docs/genesis_coinbase_integration.cpp"
echo "  🔍 $SECURE_DIR/docs/verify_genesis_integration.py"
echo "  📋 $SECURE_DIR/docs/INTEGRATION_INSTRUCTIONS.md"
echo ""

echo "🔑 YOUR P2WPKH PREMINE:"
echo "  scriptPubKey: $SCRIPT_PUBKEY_HEX"
echo "  Address: $BECH32_ADDRESS"
echo "  Descriptor: wpkh($COMPRESSED_PUBKEY)"
echo ""

echo "🚀 NEXT STEPS:"
echo "  1. Manually integrate the C++ code into your chainparams"
echo "  2. Build and test the integration"
echo "  3. Run genesis ceremony"
echo "  4. Proceed with mainnet launch"
echo ""

echo "📖 See detailed instructions in:"
echo "   $SECURE_DIR/docs/INTEGRATION_INSTRUCTIONS.md"
echo ""

echo "🎉 P2WPKH PREMINE READY FOR MANUAL INTEGRATION!"
