#!/bin/bash
# DineroCoin CLI v1.0.0 Release Build Script
set -euo pipefail

VERSION="1.0.0"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${PROJECT_ROOT}/dist"

echo "Building DineroCoin CLI v${VERSION} release artifacts..."

# Clean and create dist directory
rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}"

# Build for multiple platforms
build_platform() {
    local os="$1"
    local arch="$2"
    local build_dir="build-${os}-${arch}"
    
    echo "Building for ${os}/${arch}..."
    
    # Configure build
    cmake -B "${build_dir}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="${arch}" \
        -DDINERO_CLI_VERSION="${VERSION}" \
        -DDINERO_CLI_GIT_SHA="$(git rev-parse --short HEAD)" \
        -DDINERO_CLI_BUILD_DATE="$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    
    # Build CLI
    cmake --build "${build_dir}" --target dinero-cli -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
    
    # Package binary
    local archive_name="dinero-cli-v${VERSION}-${os}-${arch}"
    local temp_dir="${DIST_DIR}/${archive_name}"
    
    mkdir -p "${temp_dir}"
    cp "${build_dir}/bin/dinero-cli" "${temp_dir}/"
    cp LICENSE "${temp_dir}/"
    cp RELEASE_NOTES_v${VERSION}.md "${temp_dir}/"
    cp README.md "${temp_dir}/" 2>/dev/null || echo "# DineroCoin CLI v${VERSION}" > "${temp_dir}/README.md"
    
    # Create tarball
    cd "${DIST_DIR}"
    tar -czf "${archive_name}.tar.gz" "${archive_name}"
    rm -rf "${archive_name}"
    cd "${PROJECT_ROOT}"
    
    echo "✓ Built ${archive_name}.tar.gz"
}

# Detect platform and build
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS builds
    build_platform "darwin" "arm64"
    build_platform "darwin" "x86_64"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    # Linux builds
    build_platform "linux" "amd64"
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        build_platform "linux" "arm64"
    else
        echo "Warning: aarch64 cross-compiler not found, skipping arm64 build"
    fi
fi

# Generate checksums
cd "${DIST_DIR}"
echo "Generating checksums..."
shasum -a 256 *.tar.gz > SHA256SUMS
echo "✓ Generated SHA256SUMS"

# Sign checksums (if GPG key available)
if command -v gpg >/dev/null 2>&1 && gpg --list-secret-keys >/dev/null 2>&1; then
    echo "Signing checksums..."
    gpg --detach-sign --armor SHA256SUMS
    echo "✓ Generated SHA256SUMS.asc"
else
    echo "Warning: GPG not available or no signing key, skipping signature"
fi

echo ""
echo "🎉 Release artifacts built successfully:"
ls -la "${DIST_DIR}"
echo ""
echo "Upload these files to GitHub Releases:"
echo "- All .tar.gz files"
echo "- SHA256SUMS"
echo "- SHA256SUMS.asc (if generated)"
