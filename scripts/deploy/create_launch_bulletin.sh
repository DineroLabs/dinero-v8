#!/bin/bash
set -euo pipefail

# ==============================================================================
# DINERO LAUNCH BULLETIN GENERATOR
# ==============================================================================
#
# Creates the official launch bulletin with all network parameters
# This becomes the permanent record of the mainnet launch
#
# Usage: ./create_launch_bulletin.sh
#
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "📰 DINERO LAUNCH BULLETIN GENERATOR"
echo "==================================="
echo ""

# Find genesis ceremony results
GENESIS_DIR=$(find "$PROJECT_ROOT" -name "genesis_ceremony_mainnet" -type d 2>/dev/null | head -1)

if [[ -z "$GENESIS_DIR" ]]; then
    echo "❌ Genesis ceremony results not found!"
    echo "Please run: ./scripts/deploy/genesis_ceremony.sh"
    exit 1
fi

echo "📁 Using genesis data from: $GENESIS_DIR"

# Extract genesis parameters
if [[ -f "$GENESIS_DIR/genesis_result.json" ]]; then
    GENESIS_HASH=$(jq -r '.hash' "$GENESIS_DIR/genesis_result.json")
    MERKLE_ROOT=$(jq -r '.merkle_root' "$GENESIS_DIR/genesis_result.json")
    GENESIS_TIME=$(jq -r '.timestamp' "$GENESIS_DIR/genesis_result.json")
    GENESIS_DATE=$(date -d "@$GENESIS_TIME" 2>/dev/null || date -r "$GENESIS_TIME" 2>/dev/null || echo "Invalid date")
    GENESIS_BITS=$(jq -r '.bits' "$GENESIS_DIR/genesis_result.json")
    GENESIS_NONCE=$(jq -r '.nonce' "$GENESIS_DIR/genesis_result.json")
else
    echo "❌ Genesis result JSON not found!"
    exit 1
fi

# Extract CIC
if [[ -f "$GENESIS_DIR/chain_params_final.json" ]]; then
    CIC_HASH=$(jq -r '.consensus.commitment' "$GENESIS_DIR/chain_params_final.json" 2>/dev/null || echo "CIC_NOT_FOUND")
else
    CIC_HASH="CIC_NOT_FOUND"
fi

# Extract dev fund address
if [[ -f "$GENESIS_DIR/dev_fund/address_1.txt" ]]; then
    DEV_FUND_ADDRESS=$(cat "$GENESIS_DIR/dev_fund/address_1.txt" | head -1)
else
    DEV_FUND_ADDRESS="DEV_FUND_ADDRESS_NOT_FOUND"
fi

# Get current platform
PLATFORM=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)
PLATFORM_TAG="${PLATFORM}-${ARCH}"

# Create launch bulletin
LAUNCH_DATE=$(date)
BULLETIN_FILE="$PROJECT_ROOT/DINERO_LAUNCH_BULLETIN.md"

echo "📝 Generating launch bulletin..."

cat > "$BULLETIN_FILE" << EOF
# 🚀 DINERO MAINNET LAUNCH BULLETIN

**OFFICIAL NETWORK LAUNCH ANNOUNCEMENT**

---

## 📋 NETWORK PARAMETERS (IMMUTABLE)

### Genesis Block
- **Hash:** \`$GENESIS_HASH\`
- **Time:** \`$GENESIS_TIME\` ($GENESIS_DATE)
- **Bits:** \`$GENESIS_BITS\`
- **Nonce:** \`$GENESIS_NONCE\`
- **Merkle Root:** \`$MERKLE_ROOT\`

### Chain Identity
- **CIC:** \`$CIC_HASH\`
- **Magic Bytes:** \`0xD14E5201\`
- **HRP:** \`din\`

### Network Ports
- **P2P:** \`40999\`
- **RPC:** \`20999\` (localhost only)
- **Health:** \`22001\` (localhost only)

---

## 💰 ECONOMICS

### Supply
- **Total:** 99,000,000 DIN
- **Developer Fund:** 2,000,000 DIN (2.02%)
- **Mining Rewards:** 97,000,000 DIN

### Developer Fund
- **Address:** \`$DEV_FUND_ADDRESS\`
- **Type:** 2-of-3 Multisig P2WSH
- **Purpose:** Development, security, ecosystem

### Mining Schedule
1. **Phase 1** (0-2M): Premine only
2. **Phase 2** (2M-20M): 100 DIN/block, CPU-friendly
3. **Phase 3** (20M+): Halving schedule, Bitcoin-level

---

## 🌐 NETWORK INFRASTRUCTURE

### DNS Seeds
- \`seed.dinero-coin.com\`
- \`dnsseed.dinero-coin.com\`

### Seed Nodes
- \`seed1.dinero-coin.com:40999\` (US-East)
- \`seed2.dinero-coin.com:40999\` (EU-West)
- \`seed3.dinero-coin.com:40999\` (Asia-Pacific)

---

## 📦 BINARIES & VERIFICATION

### Download
\`\`\`bash
wget https://github.com/dinerocoin/dinero/releases/download/v1.0.0-mainnet/dinero-v1.0.0-mainnet-$PLATFORM_TAG-release.tar.gz
\`\`\`

### Verification
\`\`\`bash
# Download checksums and signatures
wget https://github.com/dinerocoin/dinero/releases/download/v1.0.0-mainnet/SHA256SUMS.txt
wget https://github.com/dinerocoin/dinero/releases/download/v1.0.0-mainnet/SHA256SUMS.txt.asc

# Verify
gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
sha256sum --check SHA256SUMS.txt
\`\`\`

---

## ⛏️ START MINING

### Quick Start
\`\`\`bash
# Extract and deploy
tar -xzf dinero-v1.0.0-mainnet-$PLATFORM_TAG-release.tar.gz
cd dinero-v1.0.0-mainnet-$PLATFORM_TAG-release
sudo ./scripts/deploy_mainnet.sh --full

# Start mining
./scripts/launch_mining.sh --generate-address --threads 4
\`\`\`

### Mining Commands
\`\`\`bash
# Generate address
dinero-cli getnewaddress

# Check balance
dinero-cli getbalance

# Network status
dinero-cli getblockcount
dinero-cli getconnectioncount
\`\`\`

---

## 📊 MONITORING

### Health Check
\`\`\`bash
curl http://127.0.0.1:22001/healthz
\`\`\`

### RPC Status
\`\`\`bash
# Chain tips
curl -s -H 'content-type: application/json' \\
  -d '{"jsonrpc":"2.0","id":1,"method":"getchaintips","params":[]}' \\
  http://127.0.0.1:20999/

# Chainwork
curl -s -H 'content-type: application/json' \\
  -d '{"jsonrpc":"2.0","id":1,"method":"getchainwork","params":[]}' \\
  http://127.0.0.1:20999/

# Reorg status
curl -s -H 'content-type: application/json' \\
  -d '{"jsonrpc":"2.0","id":1,"method":"getreorgstatus","params":[]}' \\
  http://127.0.0.1:20999/
\`\`\`

---

## 🔐 SECURITY

### Consensus Protection
- Deep reorg protection (30+ blocks triggers safe-mode)
- Chain Identity Commitment prevents tampering
- Signed releases with GPG verification

### Operational Security
- RPC localhost-only binding
- Cookie authentication
- Systemd sandboxing
- Firewall configuration

### Security Contact
**security@dinero-coin.com**

---

## 🤝 COMMUNITY

### Official Channels
- **Website:** https://dinero-coin.com
- **Docs:** https://docs.dinero-coin.com
- **GitHub:** https://github.com/dinerocoin/dinero
- **Discord:** https://discord.gg/dinerocoin
- **Twitter:** https://twitter.com/dinerocoin

### Support
- **Issues:** https://github.com/dinerocoin/dinero/issues
- **Discussions:** https://github.com/dinerocoin/dinero/discussions
- **Email:** support@dinero-coin.com

---

## 🎯 LAUNCH CHECKLIST

### For Node Operators
- [ ] Download and verify binaries
- [ ] Deploy using \`deploy_mainnet.sh\`
- [ ] Verify network connectivity
- [ ] Configure monitoring
- [ ] Start mining (optional)

### For Developers
- [ ] Review RPC documentation
- [ ] Test integration on testnet first
- [ ] Implement proper confirmation depths
- [ ] Monitor reorg status endpoint

### For Exchanges
- [ ] Implement \`getchaintips\` monitoring
- [ ] Set confirmation requirements (6+ blocks)
- [ ] Monitor \`getreorgstatus\` for safe-mode
- [ ] Test deposit/withdrawal flows

---

## 📜 REPRODUCIBLE BUILDS

### Build Environment
- **OS:** Ubuntu 22.04 LTS
- **Compiler:** GCC 11.4
- **CMake:** 3.22+
- **Dependencies:** See \`CMakeLists.txt\`

### Verification
\`\`\`bash
# Clone and build
git clone https://github.com/dinerocoin/dinero.git
cd dinero
git checkout v1.0.0-mainnet

# Build release
./scripts/deploy/build_release.sh --version v1.0.0-mainnet

# Compare checksums
sha256sum build-release/release/bin/dinerod
\`\`\`

---

## 🎉 WELCOME TO DINERO!

**Dinero is now live and mining!** 

This bulletin serves as the permanent record of our mainnet launch. All parameters listed here are immutable consensus rules that cannot be changed without a hard fork.

**Join the CPU-friendly mining revolution!** ⛏️💎

---

**Launch Date:** $LAUNCH_DATE  
**Network Status:** 🔥 LIVE 🔥  
**Version:** v1.0.0-mainnet  

*This bulletin was automatically generated from genesis ceremony results.*
*Hash: $GENESIS_HASH*
*CIC: $CIC_HASH*

EOF

echo "✅ Launch bulletin created: $BULLETIN_FILE"
echo ""

# Create a copy in the deploy directory
cp "$BULLETIN_FILE" "$SCRIPT_DIR/launch_bulletin.md"
echo "📋 Copy created: $SCRIPT_DIR/launch_bulletin.md"

# Create social media friendly version
SOCIAL_FILE="$SCRIPT_DIR/launch_announcement.txt"
cat > "$SOCIAL_FILE" << EOF
🚀 DINERO MAINNET IS LIVE! 🚀

The first CPU-friendly cryptocurrency is now mining!

🎯 Key Facts:
• Total Supply: 99M DIN
• Block Time: 60 seconds  
• Mining: CPU-friendly until 20M DIN
• Address Format: din1... (Bech32)

⛏️ Start Mining:
wget https://github.com/dinerocoin/dinero/releases/download/v1.0.0-mainnet/dinero-v1.0.0-mainnet-$PLATFORM_TAG-release.tar.gz

🌐 Network:
• P2P Port: 40999
• Genesis: $GENESIS_HASH
• Launch: $LAUNCH_DATE

Join the revolution! No ASICs needed! 💎

#Dinero #Cryptocurrency #CPUMining #Blockchain
EOF

echo "📱 Social announcement: $SOCIAL_FILE"
echo ""

# Validate bulletin
echo "🔍 Validating bulletin..."
if [[ "$GENESIS_HASH" =~ ^[0-9a-f]{64}$ ]]; then
    echo "✅ Genesis hash format valid"
else
    echo "⚠️  Genesis hash format may be invalid: $GENESIS_HASH"
fi

if [[ "$GENESIS_TIME" =~ ^[0-9]{10}$ ]]; then
    echo "✅ Genesis timestamp valid"
else
    echo "⚠️  Genesis timestamp may be invalid: $GENESIS_TIME"
fi

echo ""
echo "🎉 LAUNCH BULLETIN COMPLETE!"
echo ""
echo "📋 Files created:"
echo "  • $BULLETIN_FILE"
echo "  • $SCRIPT_DIR/launch_bulletin.md"
echo "  • $SOCIAL_FILE"
echo ""
echo "🚀 Ready to publish and announce the launch!"
echo ""
echo "📤 Next steps:"
echo "  1. Review the bulletin for accuracy"
echo "  2. Commit to repository"
echo "  3. Create GitHub release"
echo "  4. Publish on website"
echo "  5. Announce on social media"
echo ""
echo "🎯 Dinero mainnet is officially documented and ready!"
