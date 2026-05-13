#!/usr/bin/env bash
set -euo pipefail

# Generate build attestation for reproducible builds
# Outputs: BUILD_ATTESTATION.json

VERSION="${RELEASE_VERSION:-unknown}"
PLATFORM=$(uname -s | tr A-Z a-z)
ARCH=$(uname -m)
COMMIT=$(git rev-parse HEAD 2>/dev/null || echo "unknown")
COMMIT_SHORT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
BUILD_DATE=$(date -u +"%Y-%m-%dT%H:%M:%SZ" 2>/dev/null || date -u)
TAG=$(git describe --tags --exact-match 2>/dev/null || echo "untagged")

# Detect toolchain
if command -v cc &> /dev/null; then
    CC_VERSION=$(cc --version 2>/dev/null | head -1 || echo "unknown")
else
    CC_VERSION="unknown"
fi

if command -v cmake &> /dev/null; then
    CMAKE_VERSION=$(cmake --version 2>/dev/null | head -1 || echo "unknown")
else
    CMAKE_VERSION="unknown"
fi

# Get artifact hash if it exists
ARTIFACT_FILE="dist/DineroCoin-${VERSION}-${PLATFORM}-${ARCH}.tar.gz"
if [ -f "$ARTIFACT_FILE" ]; then
    ARTIFACT_HASH=$(shasum -a 256 "$ARTIFACT_FILE" | cut -d' ' -f1)
    ARTIFACT_SIZE=$(stat -f%z "$ARTIFACT_FILE" 2>/dev/null || stat -c%s "$ARTIFACT_FILE" 2>/dev/null || echo "0")
else
    ARTIFACT_HASH="not-yet-built"
    ARTIFACT_SIZE="0"
fi

# Get vendored dependency versions from submodules
ROCKSDB_COMMIT=$(cd third_party/rocksdb 2>/dev/null && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
GTEST_COMMIT=$(cd third_party/googletest 2>/dev/null && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
ROCKSDB_TAG=$(cd third_party/rocksdb 2>/dev/null && git describe --tags --exact-match 2>/dev/null || echo "unknown")
GTEST_TAG=$(cd third_party/googletest 2>/dev/null && git describe --tags --exact-match 2>/dev/null || echo "v1.14.0")

# Generate attestation JSON
cat > dist/BUILD_ATTESTATION.json <<EOF
{
  "version": "1.0",
  "attestation_type": "dinero-build-attestation",
  "generated_at": "${BUILD_DATE}",
  "release": {
    "version": "${VERSION}",
    "git_commit": "${COMMIT}",
    "git_commit_short": "${COMMIT_SHORT}",
    "git_tag": "${TAG}"
  },
  "build": {
    "platform": "${PLATFORM}",
    "architecture": "${ARCH}",
    "timestamp": "${BUILD_DATE}",
    "source_date_epoch": "$(git log -1 --format=%ct 2>/dev/null || echo '0')"
  },
  "toolchain": {
    "compiler": "${CC_VERSION}",
    "cmake": "${CMAKE_VERSION}",
    "build_type": "Release"
  },
  "artifacts": [
    {
      "filename": "$(basename ${ARTIFACT_FILE})",
      "sha256": "${ARTIFACT_HASH}",
      "size_bytes": ${ARTIFACT_SIZE}
    }
  ],
  "dependencies": {
    "mode": "vendored",
    "rocksdb": {
      "source": "facebook/rocksdb",
      "commit": "${ROCKSDB_COMMIT}",
      "tag": "${ROCKSDB_TAG}"
    },
    "googletest": {
      "source": "google/googletest",
      "commit": "${GTEST_COMMIT}",
      "tag": "${GTEST_TAG}"
    }
  },
  "reproducibility": {
    "instructions": "https://github.com/Trucker2827/Dinero-Coin/blob/main/.github/workflows/README.md",
    "script": "scripts/build-release-node.sh",
    "environment": "GitHub Actions (native runner)"
  }
}
EOF

echo "✅ Build attestation generated: dist/BUILD_ATTESTATION.json"
cat dist/BUILD_ATTESTATION.json
