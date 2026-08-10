#!/usr/bin/env bash
# Package the Linux Qt desktop bundle as a tarball.
#
# Produces: dist/dinero-v<VERSION>-linux-x86_64-qt.tar.gz
#
# Mirrors the AppImage payload (dinero-qt + 6 daemon binaries + Qt6 .so
# files + plugins + AppRun wrapper) but as a plain tarball instead of an
# AppImage. Lower magic: extract anywhere, run ./AppRun.
#
# Reuses the AppDir produced by build-appimage.sh — run that first.
#
# Usage:
#   bash packaging/linux/build-qt-tarball.sh 8.0.0-rc3
#   bash packaging/linux/build-qt-tarball.sh 8.0.0-rc3 /path/to/dist

set -euo pipefail

VERSION="${1:-8.0.0-dev}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DIST_DIR="${2:-${PROJECT_ROOT}/dist}"

NAME="dinero-v${VERSION}-linux-x86_64-qt"
APPDIR="${DIST_DIR}/dinero-v${VERSION}-linux-x86_64.AppDir"
STAGE="${DIST_DIR}/${NAME}"

echo "----------------------------------------------------------"
echo "Packaging Dinero Qt tarball -- v${VERSION}"
echo "----------------------------------------------------------"
echo "APPDIR: ${APPDIR}"
echo "STAGE : ${STAGE}"

if [ ! -d "${APPDIR}" ]; then
    echo "ERROR: ${APPDIR} not found. Run build-appimage.sh first to produce the AppDir." >&2
    exit 1
fi

rm -rf "${STAGE}"
cp -a "${APPDIR}" "${STAGE}"

# AppDir uses .DirIcon + a root-level .desktop file that linuxdeploy
# emits for AppImage assembly. They're harmless in a tarball but a bit
# odd; leave them in place so the tarball still works as a portable
# AppDir if someone wants to run linuxdeploy/appimagetool against it
# locally.

# LICENSE + README at the tarball root.
cp "${PROJECT_ROOT}/LICENSE" "${STAGE}/LICENSE"
COMMIT=$(cd "${PROJECT_ROOT}" && git rev-parse HEAD 2>/dev/null || echo unknown)
GCCVER=$(gcc -dumpversion)
QTVER=$(qmake6 --version 2>/dev/null | tail -1 | awk '{print $4}')
BUILTON=$(date -u +%Y-%m-%dT%H:%M:%SZ)
cat > "${STAGE}/README.txt" <<EOF
Dinero v${VERSION} -- linux-x86_64-qt user bundle
========================================

Build:      gcc ${GCCVER} (Ubuntu 22.04)
Qt:         ${QTVER} (system, bundled in usr/lib/)
Built UTC:  ${BUILTON}
Linkage:    Vendored OpenSSL 3.5.7 (static, no-shared) + bundled Qt6 .so
Source:     https://github.com/DineroLabs/dinero-v8
Commit:     ${COMMIT}

Included binaries (usr/bin/):
  dinero-qt              Qt6 GUI wallet (start here)
  dinerod                Full node daemon (started by dinero-qt automatically)
  dinero-cli             CLI RPC client
  dinero-seeder          Network seeder
  dinero-miner           CPU miner
  dinero-gpu-miner       OpenCL GPU miner
  dinero-stratum-worker  Stratum worker client
  dinero-wallet-cli      Reference wallet CLI
  dinero-solo-miner      Solo miner (mines against the bundled daemon)
  dinero-sv2-miner       Stratum V2 CPU miner
  dinero-sv2-gpu-miner   Stratum V2 GPU miner

Quick start:
  tar -xzf $(basename "${STAGE}").tar.gz
  cd $(basename "${STAGE}")
  sha256sum -c SHA256SUMS.txt
  ./AppRun                          # launches dinero-qt with bundled Qt6

The AppRun wrapper sets LD_LIBRARY_PATH and QT_PLUGIN_PATH so the
bundled Qt6 libraries take precedence over any system Qt install. This
makes the bundle portable across distros that ship a different Qt6 ABI.

The OpenCL GPU binaries are included. CUDA acceleration remains an optional
runtime path and is not required to launch or use the wallet and full node.

SHA256SUMS.txt at the archive root has per-file hashes for verification.
The archive itself has its own SHA256, listed on the GitHub release page.
EOF
echo "  README.txt + LICENSE"

# Per-file SHA256SUMS.txt (POSIX format so sha256sum -c works).
cd "${STAGE}"
( find . -type f ! -name 'SHA256SUMS.txt' -printf '%P\n' \
    | LC_ALL=C sort \
    | while IFS= read -r f; do sha256sum "${f}"; done ) > SHA256SUMS.txt
echo "  SHA256SUMS.txt ($(wc -l < SHA256SUMS.txt) entries)"

# Tar it up.
cd "${DIST_DIR}"
tar -czf "${NAME}.tar.gz" "${NAME}"
TARBALL="${DIST_DIR}/${NAME}.tar.gz"
SIZE=$(stat -c %s "${TARBALL}")
HASH=$(sha256sum "${TARBALL}" | awk '{print $1}')

echo ""
echo "----------------------------------------------------------"
echo "Qt tarball ready"
echo "----------------------------------------------------------"
echo "  Path:   ${TARBALL}"
echo "  Size:   ${SIZE} bytes ($(awk "BEGIN{printf \"%.2f\", ${SIZE}/1024/1024}") MB)"
echo "  SHA256: ${HASH}"
