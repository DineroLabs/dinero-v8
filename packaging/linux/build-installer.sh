#!/bin/bash
# packaging/linux/build-installer.sh
#
# Stage the dinerod stack into per-binary tarballs, one full desktop
# bundle, and invoke dpkg-buildpackage for the Ubuntu 24.04+ .deb.
# Produces the full
# release artifact set the v2.2.6-rc1 line shipped on Linux, now under
# v8.0.0-rc1 naming:
#
#   dist/Dinero-<VERSION>-linux-x86_64-full.tar.gz
#   dist/dinero-core-<VERSION>-linux-x86_64.tar.gz
#   dist/dinero-qt-<VERSION>-linux-x86_64.tar.gz
#   dist/dinero-solo-miner-<VERSION>-linux-x86_64.tar.gz
#   dist/dinero-seeder-<VERSION>-linux-x86_64.tar.gz
#   dist/dinero-core_<VERSION>~rc1-1_amd64.deb (via dpkg-buildpackage)
#
# This is the Linux counterpart to packaging/windows/build-installer.ps1
# and packaging/mac/build-installer.sh — same -Version flag, same
# staging pattern.
#
# Prerequisites (one-time per machine):
#   - dpkg-dev, debhelper, devscripts (apt install dpkg-dev debhelper devscripts)
#   - lintian (for .deb verification)
#   - The monorepo stack already built. The Linux release surface is HEADLESS
#     (core/cli/seeder/solo-miner) and the daemon must be SERVER-SAFE, so build
#     with GPU mining OFF — this keeps libOpenCL/libCUDA out of dinerod's NEEDED
#     (a fresh Ubuntu VPS has no libOpenCL.so.1; a daemon linking it won't start):
#       cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
#             -DENABLE_GPU_MINING=OFF \
#             -DDINERO_BUILD_QT=OFF -DDINERO_BUILD_MINER=ON \
#             -DDINERO_BUILD_SEEDER=ON
#       cmake --build build-release -j$(nproc) \
#             --target dinerod dinero-cli dinero-solo-miner-cli \
#                      dinero-seeder
#   (A server-safety guard below refuses to package a dinerod that links a GPU
#    runtime. GPU mining belongs only in the separate GPU-miner build/targets.)
#
# Usage:
#   ./packaging/linux/build-installer.sh --version 8.0.0-rc1
#   ./packaging/linux/build-installer.sh --version 8.0.0 --build-dir build-release --skip-deb
#
# The --skip-deb flag is for dev iteration on the tarballs without
# triggering dpkg-buildpackage every time. Release builds run with .deb on.

set -euo pipefail

VERSION="8.0.0-dev"
BUILD_DIR=""
SKIP_DEB=0
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --skip-deb) SKIP_DEB=1; shift ;;
        -h|--help)
            sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$BUILD_DIR" ]]; then
    BUILD_DIR="$PROJECT_ROOT/build-release"
fi
if [[ ! -d "$BUILD_DIR" ]]; then
    echo "ERROR: build directory not found at $BUILD_DIR" >&2
    echo "Build the monorepo stack first (see top-of-file usage)." >&2
    exit 1
fi

# rc24: the daemon must be SERVER-SAFE — no GPU runtime dependency. GPU/OpenCL
# belongs only in GPU miner targets, never in dinerod. A fresh Ubuntu VPS has no
# libOpenCL.so.1, so a daemon that links it dies at startup with
# "error while loading shared libraries: libOpenCL.so.1" (found via the rc23
# independent-operator test). Build the Linux daemon with -DENABLE_GPU_MINING=OFF
# (drops libOpenCL/libCUDA from NEEDED). This guard refuses to package a daemon
# that would fail to start on a stock box.
if [[ -x "$BUILD_DIR/dinerod" ]] && command -v ldd >/dev/null 2>&1; then
    if ldd "$BUILD_DIR/dinerod" 2>/dev/null | grep -qiE 'libOpenCL|libcuda'; then
        echo "ERROR: $BUILD_DIR/dinerod links a GPU runtime (libOpenCL/libCUDA)." >&2
        echo "       The daemon must be server-safe; a stock box has no such lib." >&2
        echo "       Rebuild the Linux daemon with -DENABLE_GPU_MINING=OFF." >&2
        ldd "$BUILD_DIR/dinerod" 2>/dev/null | grep -iE 'opencl|cuda' >&2 || true
        exit 1
    fi
    echo "✅ dinerod is server-safe (no libOpenCL/libCUDA dependency)"
fi

DIST_DIR="$PROJECT_ROOT/packaging/linux/dist"
DEB_ARCH="$(dpkg --print-architecture 2>/dev/null || uname -m)"
case "$DEB_ARCH" in
    amd64) RELEASE_ARCH="x86_64" ;;
    arm64) RELEASE_ARCH="aarch64" ;;
    *) RELEASE_ARCH="$DEB_ARCH" ;;
esac

echo "----------------------------------------------------------"
echo "Building Dinero Linux installer -- v$VERSION ($RELEASE_ARCH)"
echo "----------------------------------------------------------"

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

# Pack a single binary into <name>-<version>-linux-<arch>.tar.gz with
# the binary placed at <name>-<version>/<name> inside the tarball.
pack_binary() {
    local label="$1"   # e.g. "dinero-core"
    local binary="$2"  # path to the actual binary
    local outname="${label}-${VERSION}-linux-${RELEASE_ARCH}.tar.gz"

    if [[ ! -x "$binary" ]]; then
        echo "  WARNING: $binary not built; skipping $outname" >&2
        return
    fi

    local stage; stage="$(mktemp -d)"
    local bname; bname="$(basename "$binary")"
    mkdir -p "$stage/${label}-${VERSION}"
    cp "$binary" "$stage/${label}-${VERSION}/$bname"
    tar -czf "$DIST_DIR/$outname" -C "$stage" "${label}-${VERSION}"
    rm -rf "$stage"

    local size; size=$(stat -c%s "$DIST_DIR/$outname" 2>/dev/null || \
                       stat -f%z "$DIST_DIR/$outname")
    local hash; hash=$(sha256sum "$DIST_DIR/$outname" | awk '{print $1}')
    echo "  $outname ($size bytes)"
    echo "    SHA256: $hash"
}

pack_full_bundle() {
    local outname="Dinero-${VERSION}-linux-${RELEASE_ARCH}-full.tar.gz"
    local stage; stage="$(mktemp -d)"
    local root="$stage/Dinero-${VERSION}-linux-${RELEASE_ARCH}-full"
    local missing=0

    mkdir -p "$root/bin"

    add_required_binary() {
        local binary="$1"
        local name="$2"
        if [[ ! -x "$binary" ]]; then
            echo "  WARNING: full bundle missing required binary: $binary" >&2
            missing=1
            return
        fi
        cp "$binary" "$root/bin/$name"
    }

    add_optional_binary() {
        local binary="$1"
        local name="$2"
        if [[ -x "$binary" ]]; then
            cp "$binary" "$root/bin/$name"
        fi
    }

    add_required_binary "$BUILD_DIR/dinerod" "dinerod"
    add_required_binary "$BUILD_DIR/dinero-cli" "dinero-cli"
    add_required_binary "$BUILD_DIR/bin/dinero-qt" "dinero-qt"
    add_required_binary "$BUILD_DIR/miner/dinero-solo-miner" "dinero-solo-miner"
    add_required_binary "$BUILD_DIR/seeder/dinero-seeder" "dinero-seeder"

    # Optional mining/worker tools. Include them when the builder produced them.
    add_optional_binary "$BUILD_DIR/dinero-gpu-miner" "dinero-gpu-miner"
    add_optional_binary "$BUILD_DIR/dinero-miner" "dinero-miner"
    add_optional_binary "$BUILD_DIR/dinero-stratum-worker" "dinero-stratum-worker"

    if [[ $missing -ne 0 ]]; then
        echo "  WARNING: skipping $outname; build all desktop targets first" >&2
        rm -rf "$stage"
        return
    fi

    [[ -f "$PROJECT_ROOT/LICENSE" ]] && cp "$PROJECT_ROOT/LICENSE" "$root/LICENSE"
    cat > "$root/VERSION" <<EOF
Dinero ${VERSION}
repo: dinero
platform: linux-${RELEASE_ARCH}
bundle: full desktop wallet
EOF

    cat > "$root/README.txt" <<EOF
Dinero Linux full desktop bundle ${VERSION}

This bundle is the Linux counterpart to the macOS and Windows wallet
packages. It includes the GUI wallet, daemon, CLI, seeder/crawler, and
the built miner tools.

Common entry points:
  ./bin/dinero-qt
  ./bin/dinerod
  ./bin/dinero-cli
  ./bin/dinero-solo-miner
  ./bin/dinero-seeder

Optional tools may be present when built on the release host:
  ./bin/dinero-gpu-miner
  ./bin/dinero-miner
  ./bin/dinero-stratum-worker

Operators who only need headless node binaries can use the standalone
operator daemon tarball instead.
EOF

    (cd "$root" && sha256sum bin/* > SHA256SUMS)
    tar -czf "$DIST_DIR/$outname" -C "$stage" "$(basename "$root")"
    rm -rf "$stage"

    local size; size=$(stat -c%s "$DIST_DIR/$outname" 2>/dev/null || \
                       stat -f%z "$DIST_DIR/$outname")
    local hash; hash=$(sha256sum "$DIST_DIR/$outname" | awk '{print $1}')
    echo "  $outname ($size bytes)"
    echo "    SHA256: $hash"
}

echo ""
echo "Producing standalone tarballs..."
pack_binary "dinero-core"        "$BUILD_DIR/dinerod"
pack_binary "dinero-cli"         "$BUILD_DIR/dinero-cli"
pack_binary "dinero-solo-miner"  "$BUILD_DIR/miner/dinero-solo-miner"
pack_binary "dinero-seeder"      "$BUILD_DIR/seeder/dinero-seeder"

# Optional: dinero-qt if Qt was built. dinero-qt binary on Linux is a
# single executable (not an .app bundle).
if [[ -x "$BUILD_DIR/bin/dinero-qt" ]]; then
    pack_binary "dinero-qt"      "$BUILD_DIR/bin/dinero-qt"
fi

# Optional: GPU/stratum miners.
[[ -x "$BUILD_DIR/dinero-gpu-miner" ]]       && pack_binary "dinero-gpu-miner"       "$BUILD_DIR/dinero-gpu-miner"
[[ -x "$BUILD_DIR/dinero-miner" ]]           && pack_binary "dinero-miner"           "$BUILD_DIR/dinero-miner"
[[ -x "$BUILD_DIR/dinero-stratum-worker" ]]  && pack_binary "dinero-stratum-worker"  "$BUILD_DIR/dinero-stratum-worker"

echo ""
echo "Producing full desktop bundle..."
pack_full_bundle

# .deb (Ubuntu 24.04+ packaged-service Core). The version in
# debian/changelog drives the .deb filename.
if [[ $SKIP_DEB -eq 0 ]]; then
    echo ""
    echo "Building .deb via dpkg-buildpackage..."
    if ! command -v dpkg-buildpackage >/dev/null 2>&1; then
        echo "  ERROR: dpkg-buildpackage not found (apt install dpkg-dev debhelper)" >&2
        exit 1
    fi
    pushd "$PROJECT_ROOT" >/dev/null
    dpkg-buildpackage -us -uc -b
    popd >/dev/null

    # dpkg drops the .deb next to the source tree's parent. Move it
    # into our dist/ for collection.
    deb_version=$(dpkg-parsechangelog -S Version)
    local_arch_deb="$(dirname "$PROJECT_ROOT")/dinero-core_${deb_version}_${DEB_ARCH}.deb"
    if [[ ! -f "$local_arch_deb" ]]; then
        local_arch_deb=$(ls -t "$(dirname "$PROJECT_ROOT")"/dinero-core_*"_${DEB_ARCH}.deb" 2>/dev/null | head -1 || true)
    fi
    if [[ -n "$local_arch_deb" && -f "$local_arch_deb" ]]; then
        cp "$local_arch_deb" "$DIST_DIR/"
        deb_name=$(basename "$local_arch_deb")
        deb_hash=$(sha256sum "$DIST_DIR/$deb_name" | awk '{print $1}')
        echo "  $deb_name"
        echo "    SHA256: $deb_hash"
    else
        echo "  WARNING: dpkg-buildpackage finished but no .deb found alongside the source" >&2
    fi
fi

echo ""
echo "----------------------------------------------------------"
echo "Linux artifacts ready in $DIST_DIR:"
echo "----------------------------------------------------------"
ls -la "$DIST_DIR"
echo ""
echo "Linux binaries don't require codesigning. Operators that want"
echo "GPG-signed SHA256SUMS files can run:"
echo "  cd $DIST_DIR && sha256sum *.tar.gz *.deb > SHA256SUMS-v${VERSION}"
echo "  gpg --detach-sign --armor SHA256SUMS-v${VERSION}"
echo "Full release-publication flow in RELEASE_v8.md Phase 3.E + Phase 6."
