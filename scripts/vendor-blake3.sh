#!/usr/bin/env bash
# ============================================================================
# Vendor BLAKE3 for DineroCoin Lightning Network
# Fast channel hashing, onion routing optimization
# Priority: P2 (Optional performance enhancement)
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
BLAKE3_DIR="$PROJECT_ROOT/third_party/blake3"

echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${BLUE}Vendoring BLAKE3 for DineroCoin Lightning Performance${NC}"
echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo -e "${BLUE}Purpose:${NC} Fast channel hashing (10x faster than SHA256)"
echo -e "${BLUE}Version:${NC} 1.5.0+"
echo -e "${BLUE}License:${NC} Apache 2.0 / CC0 (dual-licensed)"
echo ""

# Check if already exists
if [ -d "$BLAKE3_DIR" ]; then
    echo -e "${YELLOW}⚠️  BLAKE3 already exists at:${NC}"
    echo "  $BLAKE3_DIR"
    echo ""
    read -p "Re-clone and rebuild? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo -e "${GREEN}✅ Using existing BLAKE3${NC}"
        exit 0
    fi
    echo -e "${YELLOW}🗑️  Removing existing installation...${NC}"
    rm -rf "$BLAKE3_DIR"
fi

# Clone repository
echo -e "${BLUE}📥 Cloning BLAKE3 from GitHub...${NC}"
cd "$PROJECT_ROOT/third_party"
git clone https://github.com/BLAKE3-team/BLAKE3 blake3
cd blake3

BLAKE3_VERSION=$(git describe --tags --abbrev=0 2>/dev/null || echo "latest")
echo -e "${GREEN}✅ Cloned version: $BLAKE3_VERSION${NC}"
echo ""

# Check for CMake
if ! command -v cmake &> /dev/null; then
    echo -e "${RED}❌ CMake not found${NC}"
    echo ""
    echo -e "${YELLOW}Install with:${NC}"
    if [[ "$OSTYPE" == "darwin"* ]]; then
        echo "  brew install cmake"
    else
        echo "  sudo apt-get install cmake"
    fi
    exit 1
fi

echo -e "${GREEN}✅ CMake found: $(cmake --version | head -1)${NC}"
echo ""

# Build BLAKE3 C library
echo -e "${BLUE}🔧 Configuring BLAKE3...${NC}"

cd c

cmake -B build \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DBLAKE3_INTRINSICS=ON

echo ""
echo -e "${BLUE}🔨 Building BLAKE3...${NC}"

# Detect cores
if [[ "$OSTYPE" == "darwin"* ]]; then
    CORES=$(sysctl -n hw.ncpu)
else
    CORES=$(nproc)
fi

echo -e "${BLUE}Using $CORES CPU cores${NC}"
cmake --build build -j"$CORES"

# Verify build
echo ""
echo -e "${BLUE}🔍 Verifying build...${NC}"

if [ ! -f "build/libblake3.a" ]; then
    echo -e "${RED}❌ Build failed: libblake3.a not found${NC}"
    exit 1
fi

LIB_SIZE=$(du -h build/libblake3.a | cut -f1)

echo -e "${GREEN}✅ BLAKE3 built successfully!${NC}"
echo ""
echo -e "${GREEN}Library created:${NC}"
echo "  libblake3.a: $LIB_SIZE"
echo "  Location: $BLAKE3_DIR/c/build/libblake3.a"
echo ""

# Display features
echo -e "${BLUE}📋 Enabled optimizations:${NC}"
echo "  ✅ SIMD intrinsics (SSE2, AVX2, AVX-512 on x86)"
echo "  ✅ NEON intrinsics (ARM)"
echo "  ✅ Portable C fallback"
echo ""
echo -e "${BLUE}💨 Performance vs. SHA256:${NC}"
echo "  ⚡ ~10x faster hashing"
echo "  ⚡ ~5x faster keyed hashing (HMAC alternative)"
echo "  ⚡ Parallelizable (multi-threaded hashing)"
echo ""

# Run quick benchmark
echo -e "${BLUE}🧪 Running quick benchmark...${NC}"
if [ -f "build/blake3_bench" ]; then
    ./build/blake3_bench 2>/dev/null | head -5 || true
else
    echo -e "${YELLOW}  Benchmark binary not found (optional)${NC}"
fi
echo ""

echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}✅ BLAKE3 ready for Lightning performance${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo -e "Next steps:"
echo -e "  1. ${BLUE}Update CMakeLists.txt:${NC}"
echo -e "     add_library(blake3 STATIC IMPORTED)"
echo -e "     set_target_properties(blake3 PROPERTIES"
echo -e "         IMPORTED_LOCATION \${CMAKE_SOURCE_DIR}/third_party/blake3/c/build/libblake3.a"
echo -e "         INTERFACE_INCLUDE_DIRECTORIES \${CMAKE_SOURCE_DIR}/third_party/blake3/c"
echo -e "     )"
echo ""
echo -e "  2. ${BLUE}Replace SHA256 in hot paths:${NC}"
echo -e "     #include <blake3.h>"
echo -e "     blake3_hasher_init(&hasher);"
echo -e "     blake3_hasher_update(&hasher, data, len);"
echo -e "     blake3_hasher_finalize(&hasher, output, BLAKE3_OUT_LEN);"
echo ""
echo -e "  3. ${BLUE}Benchmark results:${NC}"
echo -e "     Compare onion routing performance before/after"
echo ""
