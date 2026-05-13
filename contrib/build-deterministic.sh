#!/usr/bin/env bash
# DineroCoin Deterministic Build Script
# Phase Z.1: Reproducible Builds
#
# Purpose: Produce bit-for-bit identical binaries from the same git tag
# Usage: ./contrib/build-deterministic.sh
#
# Requirements:
# - Exact compiler versions (see docs/REPRODUCIBLE_BUILDS.md)
# - Pinned dependencies (see docs/DEPENDENCIES.md)
# - Clean git checkout at release tag
#
# Output:
# - build-deterministic/bin/dinerod
# - build-deterministic/bin/dinero-cli (if built)
# - SHA256 hashes for verification

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Print functions
print_header() {
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${GREEN}$1${NC}"
    echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

print_info() {
    echo -e "${GREEN}✓${NC} $1"
}

print_warn() {
    echo -e "${YELLOW}⚠${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

# ============================================================================
# Step 1: Environment Setup
# ============================================================================

print_header "Phase Z.1: Deterministic Build"

# Detect platform
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    PLATFORM="linux"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="macos"
else
    print_error "Unsupported platform: $OSTYPE"
    exit 1
fi

print_info "Platform: $PLATFORM"

# Set deterministic environment variables
export SOURCE_DATE_EPOCH=1700000000  # 2023-11-15 00:00:00 UTC
export TZ=UTC
export LC_ALL=C
export HOME=/reproducible
export USER=builder

print_info "Environment variables set (SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH)"

# ============================================================================
# Step 2: Compiler Version Verification
# ============================================================================

print_header "Compiler Version Verification"

COMPILER_OK=true

if [[ "$PLATFORM" == "linux" ]]; then
    # Check GCC version
    if command -v gcc-11 &> /dev/null; then
        GCC_VERSION=$(gcc-11 --version | head -n1)
        print_info "GCC: $GCC_VERSION"

        # Verify it's 11.4.0 (or close enough)
        if ! gcc-11 --version | grep -q "11\.4\.0"; then
            print_warn "GCC version mismatch (expected 11.4.0)"
            print_warn "Reproducibility may be affected"
        fi
    else
        print_error "GCC 11 not found (install: sudo apt install gcc-11 g++-11)"
        COMPILER_OK=false
    fi

    # Check Clang version (optional)
    if command -v clang-15 &> /dev/null; then
        CLANG_VERSION=$(clang-15 --version | head -n1)
        print_info "Clang: $CLANG_VERSION"
    fi

    # Set compiler
    export CC=gcc-11
    export CXX=g++-11

elif [[ "$PLATFORM" == "macos" ]]; then
    # Check AppleClang version
    if command -v clang &> /dev/null; then
        CLANG_VERSION=$(clang --version | head -n1)
        print_info "AppleClang: $CLANG_VERSION"

        # Verify it's AppleClang 14.x (Xcode 14.x)
        if ! clang --version | grep -q "Apple clang version 14\."; then
            print_warn "AppleClang version mismatch (expected 14.x)"
            print_warn "Reproducibility may be affected"
        fi
    else
        print_error "Clang not found (install: xcode-select --install)"
        COMPILER_OK=false
    fi

    # Set compiler
    export CC=clang
    export CXX=clang++
fi

# Check CMake version
if command -v cmake &> /dev/null; then
    CMAKE_VERSION=$(cmake --version | head -n1)
    print_info "CMake: $CMAKE_VERSION"

    # Verify it's 3.26.x or higher
    CMAKE_MAJOR=$(cmake --version | grep -oP 'cmake version \K[0-9]+' | head -1)
    CMAKE_MINOR=$(cmake --version | grep -oP 'cmake version [0-9]+\.\K[0-9]+' | head -1)

    if [[ "$CMAKE_MAJOR" -lt 3 ]] || { [[ "$CMAKE_MAJOR" -eq 3 ]] && [[ "$CMAKE_MINOR" -lt 26 ]]; }; then
        print_warn "CMake version too old (expected 3.26+)"
        print_warn "Reproducibility may be affected"
    fi
else
    print_error "CMake not found"
    COMPILER_OK=false
fi

if [[ "$COMPILER_OK" == false ]]; then
    print_error "Compiler requirements not met"
    exit 1
fi

# ============================================================================
# Step 3: Git Status Check
# ============================================================================

print_header "Git Status Check"

# Check if we're in a git repository
if ! git rev-parse --git-dir &> /dev/null; then
    print_error "Not in a git repository"
    exit 1
fi

# Check for uncommitted changes
if ! git diff-index --quiet HEAD --; then
    print_warn "Uncommitted changes detected"
    print_warn "For reproducibility, build from clean checkout at release tag"
fi

# Get current git tag/commit
GIT_TAG=$(git describe --tags --exact-match 2>/dev/null || echo "none")
GIT_COMMIT=$(git rev-parse --short HEAD)

if [[ "$GIT_TAG" == "none" ]]; then
    print_warn "Not at a tagged release (commit: $GIT_COMMIT)"
    print_warn "For official releases, checkout a release tag first:"
    print_warn "  git checkout v1.0.0-rc1"
else
    print_info "Building tag: $GIT_TAG (commit: $GIT_COMMIT)"
fi

# ============================================================================
# Step 4: Clean Build Directory
# ============================================================================

print_header "Clean Build Directory"

BUILD_DIR="build-deterministic"

if [[ -d "$BUILD_DIR" ]]; then
    print_info "Removing existing build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

print_info "Creating clean build directory: $BUILD_DIR"
mkdir -p "$BUILD_DIR"

# ============================================================================
# Step 5: CMake Configuration
# ============================================================================

print_header "CMake Configuration"

cd "$BUILD_DIR"

# Deterministic CMake flags
CMAKE_FLAGS=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_SKIP_RPATH=TRUE
    -DCMAKE_BUILD_RPATH=""
    -DCMAKE_INSTALL_RPATH=""
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=FALSE
    -DUSE_SYSTEM_OPENSSL=ON  # Use system OpenSSL for simplicity
    -DENABLE_GRPC=OFF        # Disable gRPC for minimal build
    -DENABLE_GPU_MINING=OFF  # Disable GPU mining for minimal build
    -DENABLE_LIGHTNING=OFF   # Disable Lightning for minimal build
)

print_info "CMake flags:"
for flag in "${CMAKE_FLAGS[@]}"; do
    echo "  $flag"
done

# Run CMake
if ! cmake .. "${CMAKE_FLAGS[@]}"; then
    cd ..
    print_error "CMake configuration failed"
    exit 1
fi

cd ..

# ============================================================================
# Step 6: Build
# ============================================================================

print_header "Building DineroCoin"

cd "$BUILD_DIR"

# Determine number of cores
if [[ "$PLATFORM" == "linux" ]]; then
    CORES=$(nproc)
elif [[ "$PLATFORM" == "macos" ]]; then
    CORES=$(sysctl -n hw.ncpu)
fi

print_info "Building with $CORES cores"

# Build dinerod
if ! cmake --build . --target dinerod -j"$CORES"; then
    cd ..
    print_error "Build failed"
    exit 1
fi

cd ..

# ============================================================================
# Step 7: Verify Binaries
# ============================================================================

print_header "Verify Binaries"

BINARIES=(
    "$BUILD_DIR/bin/dinerod"
)

ALL_EXIST=true

for binary in "${BINARIES[@]}"; do
    if [[ -f "$binary" ]]; then
        print_info "Found: $binary"
    else
        print_error "Missing: $binary"
        ALL_EXIST=false
    fi
done

if [[ "$ALL_EXIST" == false ]]; then
    print_error "Not all binaries were built"
    exit 1
fi

# ============================================================================
# Step 8: Generate SHA256 Hashes
# ============================================================================

print_header "SHA256 Hashes"

HASH_FILE="$BUILD_DIR/SHA256SUMS.txt"
rm -f "$HASH_FILE"

for binary in "${BINARIES[@]}"; do
    if [[ "$PLATFORM" == "linux" ]]; then
        HASH=$(sha256sum "$binary" | awk '{print $1}')
    elif [[ "$PLATFORM" == "macos" ]]; then
        HASH=$(shasum -a 256 "$binary" | awk '{print $1}')
    fi

    BINARY_NAME=$(basename "$binary")
    echo "$HASH  $BINARY_NAME" >> "$HASH_FILE"
    print_info "$BINARY_NAME: $HASH"
done

# ============================================================================
# Step 9: Success Summary
# ============================================================================

print_header "Build Complete"

print_info "Binaries:"
for binary in "${BINARIES[@]}"; do
    echo "  $binary"
done

print_info "Hashes: $HASH_FILE"
print_info ""
print_info "To verify reproducibility:"
print_info "  1. Build on a different machine with same environment"
print_info "  2. Compare SHA256 hashes"
print_info "  3. Hashes must match exactly"

if [[ "$GIT_TAG" != "none" ]]; then
    print_info ""
    print_info "For release $GIT_TAG, publish these hashes in:"
    print_info "  docs/RELEASE_HASHES_$GIT_TAG.txt"
fi

exit 0
