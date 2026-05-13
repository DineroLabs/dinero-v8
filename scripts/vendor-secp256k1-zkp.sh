#!/usr/bin/env bash
# ============================================================================
# Vendor secp256k1-zkp for DineroCoin Lightning Network
# MuSig2, adaptor signatures, scriptless scripts, Taproot channels
# Priority: P1 (Advanced Lightning features, BOLT #12)
# ============================================================================

set -e  # Exit on error

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Get script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ZKP_DIR="$PROJECT_ROOT/third_party/secp256k1-zkp"

echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Vendoring secp256k1-zkp for DineroCoin Lightning${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo -e "${BLUE}Purpose:${NC} MuSig2, adaptor sigs, BOLT #12, Taproot channels"
echo -e "${BLUE}Version:${NC} master (Elements Project)"
echo -e "${BLUE}License:${NC} MIT"
echo ""

# Check if already exists
if [ -d "$ZKP_DIR" ]; then
    echo -e "${YELLOW}⚠️  secp256k1-zkp already exists at:${NC}"
    echo "  $ZKP_DIR"
    echo ""
    read -p "Re-clone and rebuild? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${GREEN}✅ Using existing secp256k1-zkp${NC}"
        exit 0
    fi
    echo -e "${YELLOW}🗑️  Removing existing installation...${NC}"
    rm -rf "$ZKP_DIR"
fi

# Clone repository
echo -e "${BLUE}📥 Cloning secp256k1-zkp from GitHub...${NC}"
cd "$PROJECT_ROOT/third_party"
git clone https://github.com/ElementsProject/secp256k1-zkp
cd secp256k1-zkp

echo -e "${GREEN}✅ Cloned latest: $(git log -1 --format='%h %s' | head -c 60)${NC}"
echo ""

# Check dependencies
echo -e "${BLUE}🔍 Checking build dependencies...${NC}"

MISSING_DEPS=()

if ! command -v autoconf &> /dev/null; then
    MISSING_DEPS+=("autoconf")
fi

if ! command -v automake &> /dev/null; then
    MISSING_DEPS+=("automake")
fi

if ! command -v libtool &> /dev/null; then
    MISSING_DEPS+=("libtool")
fi

if [ ${#MISSING_DEPS[@]} -ne 0 ]; then
    echo -e "${RED}❌ Missing build dependencies:${NC}"
    for dep in "${MISSING_DEPS[@]}"; do
        echo "  - $dep"
    done
    echo ""
    echo -e "${YELLOW}Install with:${NC}"
    if [[ "$OSTYPE" == "darwin"* ]]; then
        echo "  brew install autoconf automake libtool"
    else
        echo "  sudo apt-get install autoconf automake libtool"
    fi
    exit 1
fi

echo -e "${GREEN}✅ All dependencies found${NC}"
echo ""

# Build secp256k1-zkp
echo -e "${BLUE}🔧 Configuring secp256k1-zkp...${NC}"

./autogen.sh

./configure \
    --disable-shared \
    --enable-static \
    --enable-module-recovery \
    --enable-module-schnorrsig \
    --enable-module-extrakeys \
    --enable-module-ecdh \
    --enable-module-musig \
    --enable-experimental \
    --enable-module-generator \
    --enable-module-rangeproof \
    --enable-module-whitelist \
    --enable-module-surjectionproof \
    --enable-benchmark=no \
    --enable-tests=no

echo ""
echo -e "${BLUE}🔨 Building secp256k1-zkp...${NC}"

# Detect cores
if [[ "$OSTYPE" == "darwin"* ]]; then
    CORES=$(sysctl -n hw.ncpu)
else
    CORES=$(nproc)
fi

echo -e "${BLUE}Using $CORES CPU cores${NC}"
make -j"$CORES"

# Verify build
echo ""
echo -e "${BLUE}🔍 Verifying build...${NC}"

if [ ! -f ".libs/libsecp256k1.a" ]; then
    echo -e "${RED}❌ Build failed: libsecp256k1.a not found${NC}"
    exit 1
fi

LIB_SIZE=$(du -h .libs/libsecp256k1.a | cut -f1)

echo -e "${GREEN}✅ secp256k1-zkp built successfully!${NC}"
echo ""
echo -e "${GREEN}Library created:${NC}"
echo "  libsecp256k1.a: $LIB_SIZE"
echo "  Location: $ZKP_DIR/.libs/libsecp256k1.a"
echo ""

# Display features
echo -e "${BLUE}📋 Enabled zero-knowledge modules:${NC}"
echo "  ✅ MuSig2 - Multi-signature aggregation (BOLT #12)"
echo "  ✅ Schnorr signatures - Taproot compatibility"
echo "  ✅ ECDH - Key exchange for channels"
echo "  ✅ Generator commitments - Confidential transactions"
echo "  ✅ Range proofs - Amount privacy"
echo "  ✅ Surjection proofs - Asset privacy"
echo ""
echo -e "${YELLOW}⚠️  Note: This builds alongside base secp256k1${NC}"
echo "   Link against secp256k1-zkp for advanced features,"
echo "   or base secp256k1 for standard operations."
echo ""

echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}✅ secp256k1-zkp ready for advanced Lightning${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo -e "Next steps:"
echo -e "  1. ${BLUE}Update CMakeLists.txt:${NC}"
echo -e "     Add secp256k1-zkp to target_link_libraries"
echo ""
echo -e "  2. ${BLUE}Test MuSig2 aggregation:${NC}"
echo -e "     ./build/test_lightning_musig2"
echo ""
echo -e "  3. ${BLUE}Implement BOLT #12 offers:${NC}"
echo -e "     Uses MuSig2 for async payment channels"
echo ""
