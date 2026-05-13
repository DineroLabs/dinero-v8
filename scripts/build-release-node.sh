#!/usr/bin/env bash
set -euo pipefail

VERSION="${RELEASE_VERSION:-v2.0.1-dinero-rings}"
BUILD_DIR="build-release"
PLATFORM=$(uname -s | tr A-Z a-z)
ARCH=$(uname -m)

echo "Building DineroCoin ${VERSION} for ${PLATFORM}-${ARCH}..."

# Clean previous build
rm -rf ${BUILD_DIR} dist

# Configure build
# Use system OpenSSL if available, otherwise vendor all dependencies
if [ -d "/usr/include/openssl" ] || [ -d "/opt/homebrew/opt/openssl" ]; then
  USE_SYSTEM_SSL="-DUSE_SYSTEM_OPENSSL=ON"
else
  USE_SYSTEM_SSL=""
fi

cmake -S . -B ${BUILD_DIR} \
  -DCMAKE_BUILD_TYPE=Release \
  -DDINERO_USE_VENDORED_DEPS=ON \
  ${USE_SYSTEM_SSL} \
  -DENABLE_TESTS=OFF \
  -DENABLE_BENCHMARKS=OFF

# Build binaries
cmake --build ${BUILD_DIR} --config Release --parallel $(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Create distribution structure
mkdir -p dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/bin

# Copy binaries
if [ -f ${BUILD_DIR}/dinerod ]; then
    cp ${BUILD_DIR}/dinerod dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/bin/
fi
if [ -f ${BUILD_DIR}/dinero-cli ]; then
    cp ${BUILD_DIR}/dinero-cli dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/bin/
fi

# Bundle GUI if it exists (macOS)
if [ -d ${BUILD_DIR}/gui/dinero-qt.app ]; then
    cp -r ${BUILD_DIR}/gui/dinero-qt.app dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/
    # Bundle daemon inside app
    cp ${BUILD_DIR}/dinerod dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/dinero-qt.app/Contents/MacOS/ 2>/dev/null || true
    cp ${BUILD_DIR}/dinero-cli dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/dinero-qt.app/Contents/MacOS/ 2>/dev/null || true
fi

# Copy documentation
cp README.md dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/ 2>/dev/null || true
cp docs/EXECUTIVE_SUMMARY.md dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/ 2>/dev/null || true
cp docs/EXCHANGE_DUE_DILIGENCE.md dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/ 2>/dev/null || true
cp docs/AUDITOR_ONBOARDING_PACK.md dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/ 2>/dev/null || true
cp docs/SOC_AUDITOR_CHECKLIST.md dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/ 2>/dev/null || true

# Create INSTALL.md
cat > dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/INSTALL.md <<EOF
# DineroCoin ${VERSION} — Installation Guide

**Platform:** ${PLATFORM} (${ARCH})
**Build Date:** $(date -u +"%Y-%m-%d %H:%M UTC" 2>/dev/null || date -u)
**Commit:** $(git rev-parse --short HEAD 2>/dev/null || echo "unknown")

## Quick Start

### GUI Wallet (if available)
1. Extract this archive
2. Run \`dinero-qt\` (or \`dinero-qt.app\` on macOS)

### Command Line
\`\`\`bash
cd bin
./dinerod -daemon
./dinero-cli getblockchaininfo
\`\`\`

## Support
- Repository: https://github.com/Trucker2827/Dinero-Coin
- Security: security@dinero-coin.com
EOF

# Strip binaries (reduce size)
if command -v strip &> /dev/null; then
    strip dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}/bin/* 2>/dev/null || true
fi

# Create tarball
cd dist
tar -czf DineroCoin-${VERSION}-${PLATFORM}-${ARCH}.tar.gz DineroCoin-${VERSION}-${PLATFORM}-${ARCH}
cd ..

# Generate checksums
shasum -a 256 dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}.tar.gz > dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}.tar.gz.sha256

# Generate build attestation
if [ -f "scripts/generate-build-attestation.sh" ]; then
    bash scripts/generate-build-attestation.sh 2>/dev/null || echo "⚠️  Attestation generation skipped"
fi

# GPG sign checksums (if GPG available and key configured)
if command -v gpg &> /dev/null && gpg --list-secret-keys >/dev/null 2>&1; then
    cd dist
    cat *.sha256 > SHA256SUMS
    gpg --detach-sign --armor SHA256SUMS 2>/dev/null && echo "✅ GPG signature: SHA256SUMS.asc" || echo "⚠️  GPG signing skipped"
    cd ..
fi

echo "Build complete: dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}.tar.gz"
cat dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}.tar.gz.sha256
