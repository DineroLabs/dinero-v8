#!/usr/bin/env bash
# ============================================================================
# Vendor libwally-core for DineroCoin Lightning Network
# PSBT, BOLT #3 primitives, script utilities
# Priority: P0 (Critical for production Lightning)
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
WALLY_DIR="$PROJECT_ROOT/third_party/libwally-core"
WALLY_VERSION="release_1.3.0"  # Stable release

echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Vendoring libwally-core for DineroCoin Lightning${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo -e "${BLUE}Purpose:${NC} PSBT, BOLT #3 commitment txs, script utilities"
echo -e "${BLUE}Version:${NC} $WALLY_VERSION"
echo -e "${BLUE}License:${NC} BSD-3-Clause (Blockstream)"
echo ""

# Check if already exists
if [ -d "$WALLY_DIR" ]; then
    echo -e "${YELLOW}⚠️  libwally-core already exists at:${NC}"
    echo "  $WALLY_DIR"
    echo ""
    read -p "Re-clone and rebuild? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${GREEN}✅ Using existing libwally-core${NC}"
        exit 0
    fi
    echo -e "${YELLOW}🗑️  Removing existing installation...${NC}"
    rm -rf "$WALLY_DIR"
fi

# Clone repository
echo -e "${BLUE}📥 Cloning libwally-core from GitHub...${NC}"
cd "$PROJECT_ROOT/third_party"
git clone https://github.com/ElementsProject/libwally-core
cd libwally-core
git checkout "$WALLY_VERSION"

echo -e "${GREEN}✅ Cloned version: $(git describe --tags)${NC}"
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

# Build libwally-core
echo -e "${BLUE}🔧 Configuring libwally-core...${NC}"

./tools/autogen.sh

./configure \
    --disable-shared \
    --enable-static \
    --disable-tests \
    --disable-swig-python \
    --disable-swig-java \
    --enable-elements

echo ""
echo -e "${BLUE}🔨 Building libwally-core (this may take 2-3 minutes)...${NC}"

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

if [ ! -f "src/.libs/libwallycore.a" ]; then
    echo -e "${RED}❌ Build failed: libwallycore.a not found${NC}"
    exit 1
fi

LIB_SIZE=$(du -h src/.libs/libwallycore.a | cut -f1)

echo -e "${GREEN}✅ libwally-core built successfully!${NC}"
echo ""
echo -e "${GREEN}Library created:${NC}"
echo "  libwallycore.a: $LIB_SIZE"
echo "  Location: $WALLY_DIR/src/.libs/libwallycore.a"
echo ""

# Display features
echo -e "${BLUE}📋 Enabled features:${NC}"
echo "  ✅ PSBT (Partially Signed Bitcoin Transactions)"
echo "  ✅ BOLT #3 commitment transaction utilities"
echo "  ✅ Script construction and parsing"
echo "  ✅ BIP174 support"
echo "  ✅ Elements extensions (Liquid compatibility)"
echo ""

echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}✅ libwally-core ready for DineroCoin Lightning${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo -e "Next steps:"
echo -e "  1. ${BLUE}Rebuild DineroCoin:${NC}"
echo -e "     cmake -B build -S ."
echo -e "     cmake --build build -j${CORES}"
echo ""
echo -e "  2. ${BLUE}Test PSBT integration:${NC}"
echo -e "     ./build/test_lightning_psbt"
echo ""
echo -e "  3. ${BLUE}Update NOTICE file with attribution:${NC}"
echo -e "     Add: libwally-core (BSD-3-Clause, Blockstream)"
echo ""
