#!/bin/bash
# Create release artifacts with A→Z validation

set -Eeuo pipefail

VERSION="${1:-1.1.0-rc1}"
BUILD_DIR="build"
RELEASE_DIR="release"
A2Z_LOG="a2z-validation.log"

# Cleanup function
cleanup() {
    echo "🧹 Cleaning up..."
    pkill -f dinerod || true
    rm -rf /tmp/din-release-test-* || true
}
trap cleanup EXIT

echo "🚀 Creating release artifacts for v$VERSION"
echo "=============================================="

# Create release directory
mkdir -p "$RELEASE_DIR"

# Build the daemon
echo "📦 Building daemon..."
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target dinerod -j8

# Run A→Z validation
echo "🧪 Running A→Z validation..."
if bash scripts/run-a2z.sh > "$A2Z_LOG" 2>&1; then
    echo "✅ A→Z validation passed"
else
    echo "❌ A→Z validation failed"
    echo "Log output:"
    cat "$A2Z_LOG"
    exit 1
fi

# Get build info
BUILD_INFO=$(./"$BUILD_DIR"/bin/dinerod --version 2>&1 || echo "Version info unavailable")
GIT_HASH=$(git rev-parse --short=8 HEAD)
BUILD_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)

# Create macOS artifact
echo "📦 Creating macOS artifact..."
MACOS_DIR="$RELEASE_DIR/dinerod-macos-arm64-v$VERSION"
mkdir -p "$MACOS_DIR"

# Copy binary
cp "$BUILD_DIR"/bin/dinerod "$MACOS_DIR"/

# Create macOS package
cd "$RELEASE_DIR"
tar -czf "dinerod-macos-arm64-v$VERSION.tar.gz" "dinerod-macos-arm64-v$VERSION"
cd ..

# Create Linux artifact (if on Linux)
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    echo "📦 Creating Linux artifact..."
    LINUX_DIR="$RELEASE_DIR/dinerod-linux-x86_64-v$VERSION"
    mkdir -p "$LINUX_DIR"
    
    # Copy binary
    cp "$BUILD_DIR"/bin/dinerod "$LINUX_DIR"/
    
    # Create Linux package
    cd "$RELEASE_DIR"
    tar -czf "dinerod-linux-x86_64-v$VERSION.tar.gz" "dinerod-linux-x86_64-v$VERSION"
    cd ..
fi

# Create release manifest
echo "📋 Creating release manifest..."
cat > "$RELEASE_DIR/RELEASE_MANIFEST.txt" << EOF
DineroCoin v$VERSION Release Manifest
=====================================

Release Date: $BUILD_DATE
Git Hash: $GIT_HASH
Build Info: $BUILD_INFO

Artifacts:
$(ls -la "$RELEASE_DIR"/*.tar.gz 2>/dev/null || echo "No artifacts found")

A→Z Validation:
- Status: PASSED
- Log: $A2Z_LOG
- Test Coverage: Build, mining, PSBT, address validation, Bech32 corpus

Checksums:
$(cd "$RELEASE_DIR" && sha256sum *.tar.gz 2>/dev/null || echo "No checksums available")

Installation:
1. Extract the appropriate archive for your platform
2. Run: ./dinerod --help
3. For regtest: ./dinerod --regtest --datadir=/path/to/data

Documentation:
- vNext Developer Guide: docs/VNEXT_DEVELOPER_GUIDE.md
- Address API Examples: docs/ADDRESS_API_EXAMPLES.md
- Release Notes: RELEASE_NOTES_$VERSION.md

Support:
- GitHub Issues: https://github.com/dinerocoin/dinerocoin/issues
- Discussions: https://github.com/dinerocoin/dinerocoin/discussions
EOF

# Copy documentation
echo "📚 Copying documentation..."
cp "RELEASE_NOTES_$VERSION.md" "$RELEASE_DIR/" 2>/dev/null || echo "Release notes not found"
cp "docs/VNEXT_DEVELOPER_GUIDE.md" "$RELEASE_DIR/" 2>/dev/null || echo "Developer guide not found"
cp "docs/ADDRESS_API_EXAMPLES.md" "$RELEASE_DIR/" 2>/dev/null || echo "Address API examples not found"

# Copy A→Z log
cp "$A2Z_LOG" "$RELEASE_DIR/"

# Create final release archive
echo "📦 Creating final release archive..."
cd "$RELEASE_DIR"
tar -czf "dinerod-v$VERSION-release.tar.gz" *
cd ..

echo ""
echo "🎉 Release artifacts created successfully!"
echo "=========================================="
echo "Release directory: $RELEASE_DIR"
echo "Artifacts:"
ls -la "$RELEASE_DIR"/*.tar.gz 2>/dev/null || echo "No artifacts found"
echo ""
echo "A→Z validation log: $RELEASE_DIR/$A2Z_LOG"
echo "Release manifest: $RELEASE_DIR/RELEASE_MANIFEST.txt"
echo ""
echo "Next steps:"
echo "1. Review the A→Z validation log"
echo "2. Test the artifacts on target platforms"
echo "3. Create GitHub release with these artifacts"
echo "4. Tag the release: git tag v$VERSION"
echo "5. Push the tag: git push origin v$VERSION"
