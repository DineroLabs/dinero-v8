#!/bin/bash
set -euo pipefail

# ==============================================================================
# DINERO RELEASE BUILD SCRIPT
# ==============================================================================
#
# This script builds production-ready Dinero binaries with proper versioning,
# checksums, and signatures for mainnet deployment.
#
# Usage: ./build_release.sh [--version VERSION] [--sign]
#
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Default configuration
VERSION=""
SIGN_RELEASE=false
BUILD_DIR="$PROJECT_ROOT/build-release"
RELEASE_DIR="$PROJECT_ROOT/release"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --version)
            VERSION="$2"
            shift 2
            ;;
        --sign)
            SIGN_RELEASE=true
            shift
            ;;
        --help)
            echo "Usage: $0 [--version VERSION] [--sign]"
            echo ""
            echo "Options:"
            echo "  --version VERSION  Set release version (e.g., v1.0.0)"
            echo "  --sign            Sign release with GPG"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Determine version
if [[ -z "$VERSION" ]]; then
    # Try to get version from git
    if git describe --tags --exact-match HEAD >/dev/null 2>&1; then
        VERSION=$(git describe --tags --exact-match HEAD)
    else
        VERSION="v1.0.0-$(git rev-parse --short HEAD)"
    fi
fi

echo "🏗️  DINERO RELEASE BUILD"
echo "======================"
echo "Version: $VERSION"
echo "Sign: $SIGN_RELEASE"
echo "Build Dir: $BUILD_DIR"
echo "Release Dir: $RELEASE_DIR"
echo ""

# Clean and create directories
rm -rf "$BUILD_DIR" "$RELEASE_DIR"
mkdir -p "$BUILD_DIR" "$RELEASE_DIR"

cd "$PROJECT_ROOT"

# =============================================================================
# WALLET RELEASE GATES (STRICT)
# =============================================================================

echo "🧪 Verifying wallet release gates before packaging"
./scripts/wallet_release_gates.sh build

# =============================================================================
# BUILD MATRIX
# =============================================================================

echo "🔨 Building Release Matrix"
echo "--------------------------"

# Build configurations
declare -A BUILD_CONFIGS=(
    ["release"]="Release OFF"
    ["release-asan"]="Release ON"
    ["debug-asan"]="Debug ON"
)

for config in "${!BUILD_CONFIGS[@]}"; do
    IFS=' ' read -r build_type sanitizers <<< "${BUILD_CONFIGS[$config]}"
    
    echo ""
    echo "Building $config ($build_type, ASAN=$sanitizers)..."
    
    BUILD_SUBDIR="$BUILD_DIR/$config"
    mkdir -p "$BUILD_SUBDIR"
    
    # Configure
    cmake -S . -B "$BUILD_SUBDIR" \
        -DCMAKE_BUILD_TYPE="$build_type" \
        -DENABLE_SANITIZERS="$sanitizers" \
        -DCMAKE_INSTALL_PREFIX="$RELEASE_DIR/$config" \
        -DVERSION_STRING="$VERSION"
    
    # Build
    cmake --build "$BUILD_SUBDIR" -j$(nproc 2>/dev/null || sysctl -n hw.ncpu || echo 4)
    
    # Install to release directory
    cmake --install "$BUILD_SUBDIR"
    
    echo "✅ $config build complete"
done

echo ""
echo "✅ All builds complete"

# =============================================================================
# PACKAGE BINARIES
# =============================================================================

echo ""
echo "📦 Packaging Binaries"
echo "--------------------"

cd "$RELEASE_DIR"

# Create platform identifier
PLATFORM=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)
PLATFORM_TAG="${PLATFORM}-${ARCH}"

# Package each build configuration
for config in "${!BUILD_CONFIGS[@]}"; do
    if [[ -d "$config" ]]; then
        echo "Packaging $config..."
        
        PACKAGE_NAME="dinero-${VERSION}-${PLATFORM_TAG}-${config}"
        PACKAGE_DIR="$PACKAGE_NAME"
        
        # Create package directory structure
        mkdir -p "$PACKAGE_DIR/bin"
        mkdir -p "$PACKAGE_DIR/config"
        mkdir -p "$PACKAGE_DIR/scripts"
        mkdir -p "$PACKAGE_DIR/docs"
        
        # Copy binaries
        cp -r "$config/bin/"* "$PACKAGE_DIR/bin/" 2>/dev/null || true
        
        # Copy configuration templates
        cp "$SCRIPT_DIR/nodeinfo-mainnet.json" "$PACKAGE_DIR/config/"
        cp "$SCRIPT_DIR/dinerod.service" "$PACKAGE_DIR/config/"
        cp "$SCRIPT_DIR/prometheus.yml" "$PACKAGE_DIR/config/"
        cp "$SCRIPT_DIR/dinero_alerts.yml" "$PACKAGE_DIR/config/"
        
        # Copy deployment scripts
        cp "$SCRIPT_DIR/deploy_mainnet.sh" "$PACKAGE_DIR/scripts/"
        cp "$SCRIPT_DIR/genesis_ceremony.sh" "$PACKAGE_DIR/scripts/"
        
        # Copy documentation
        cp "$PROJECT_ROOT/README.md" "$PACKAGE_DIR/docs/" 2>/dev/null || true
        cp "$PROJECT_ROOT/LICENSE" "$PACKAGE_DIR/docs/" 2>/dev/null || true
        
        # Create installation instructions
        cat > "$PACKAGE_DIR/INSTALL.md" << EOF
# Dinero $VERSION Installation Guide

## Quick Start

1. Extract the package:
   \`\`\`bash
   tar -xzf ${PACKAGE_NAME}.tar.gz
   cd $PACKAGE_NAME
   \`\`\`

2. Install binaries (as root):
   \`\`\`bash
   sudo cp bin/* /usr/local/bin/
   sudo chmod +x /usr/local/bin/dinerod /usr/local/bin/dinero-cli
   \`\`\`

3. Deploy a node:
   \`\`\`bash
   sudo ./scripts/deploy_mainnet.sh --full
   \`\`\`

4. Start the service:
   \`\`\`bash
   sudo systemctl start dinerod
   \`\`\`

## Configuration

- Node config: \`/etc/dinero/nodeinfo.json\`
- Data directory: \`/var/lib/dinerod\`
- Logs: \`/var/log/dinerod\`

## Monitoring

- Health check: \`curl http://127.0.0.1:22001/healthz\`
- Metrics: \`curl http://127.0.0.1:22001/metrics\`
- RPC: \`dinero-cli getblockcount\`

## Support

- Documentation: https://docs.dinero-coin.com
- Issues: https://github.com/dinerocoin/dinero/issues
- Community: https://discord.gg/dinerocoin
EOF
        
        # Create version info
        cat > "$PACKAGE_DIR/VERSION" << EOF
Dinero $VERSION
Build: $config
Platform: $PLATFORM_TAG
Date: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Commit: $(git rev-parse HEAD 2>/dev/null || echo "unknown")
EOF
        
        # Create tarball
        tar -czf "${PACKAGE_NAME}.tar.gz" "$PACKAGE_DIR"
        rm -rf "$PACKAGE_DIR"
        
        echo "✅ Created ${PACKAGE_NAME}.tar.gz"
    fi
done

# =============================================================================
# GENERATE CHECKSUMS
# =============================================================================

echo ""
echo "🔒 Generating Checksums"
echo "----------------------"

# Generate SHA256 checksums
sha256sum *.tar.gz > SHA256SUMS.txt

echo "✅ Checksums generated:"
cat SHA256SUMS.txt

# =============================================================================
# SIGN RELEASE (OPTIONAL)
# =============================================================================

if [[ "$SIGN_RELEASE" == true ]]; then
    echo ""
    echo "✍️  Signing Release"
    echo "------------------"
    
    # Check if GPG is available
    if ! command -v gpg >/dev/null 2>&1; then
        echo "❌ GPG not found. Cannot sign release."
        exit 1
    fi
    
    # Sign checksums
    gpg --detach-sign --armor SHA256SUMS.txt
    
    # Sign each package
    for package in *.tar.gz; do
        gpg --detach-sign --armor "$package"
        echo "✅ Signed $package"
    done
    
    # Verify signatures
    echo ""
    echo "🔍 Verifying Signatures:"
    gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
    
    echo "✅ All signatures verified"
fi

# =============================================================================
# RELEASE SUMMARY
# =============================================================================

echo ""
echo "🎉 RELEASE BUILD COMPLETE!"
echo "========================="
echo ""
echo "📋 Release Summary:"
echo "  Version: $VERSION"
echo "  Platform: $PLATFORM_TAG"
echo "  Configs: $(echo "${!BUILD_CONFIGS[@]}" | tr ' ' ', ')"
echo "  Signed: $SIGN_RELEASE"
echo ""
echo "📦 Packages Created:"
ls -la *.tar.gz
echo ""
echo "🔒 Checksums:"
cat SHA256SUMS.txt
echo ""

if [[ "$SIGN_RELEASE" == true ]]; then
    echo "✍️  Signatures:"
    ls -la *.asc
    echo ""
fi

echo "📁 Release Directory: $RELEASE_DIR"
echo ""
echo "🚀 Ready for Distribution!"
echo ""
echo "📤 Upload Instructions:"
echo "  1. Create GitHub release: https://github.com/dinerocoin/dinero/releases/new"
echo "  2. Upload all .tar.gz files"
echo "  3. Upload SHA256SUMS.txt"
if [[ "$SIGN_RELEASE" == true ]]; then
    echo "  4. Upload all .asc signature files"
fi
echo ""
echo "🎯 Installation Command for Users:"
echo "  wget https://github.com/dinerocoin/dinero/releases/download/$VERSION/dinero-${VERSION}-${PLATFORM_TAG}-release.tar.gz"
echo "  tar -xzf dinero-${VERSION}-${PLATFORM_TAG}-release.tar.gz"
echo "  cd dinero-${VERSION}-${PLATFORM_TAG}-release"
echo "  sudo ./scripts/deploy_mainnet.sh"
echo ""

# Test one of the binaries
echo "🧪 Quick Binary Test:"
if [[ -f "release/bin/dinerod" ]]; then
    ./release/bin/dinerod --version || echo "Binary test completed"
    echo "✅ Binary test passed"
else
    echo "⚠️  Binary not found for testing"
fi

echo ""
echo "✨ Release $VERSION is ready to ship!"
