#!/usr/bin/env bash
# Build a Dinero Qt GUI AppImage for Linux x86_64.
#
# Produces: dist/dinero-v<VERSION>-linux-x86_64.AppImage
#
# Bundles dinero-qt (Qt6 wallet) + the 6 daemon binaries (dinerod,
# dinero-cli, dinero-miner, dinero-stratum-worker, dinero-gpu-miner,
# dinero-wallet-cli) into a single self-contained AppImage that runs
# on any Linux distro with glibc 2.35+ (Ubuntu 22.04 and newer).
#
# Prerequisites:
#   - linuxdeploy + linuxdeploy-plugin-qt AppImages on PATH (or in ~/bin/)
#     Download from https://github.com/linuxdeploy/linuxdeploy/releases
#     and https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases
#   - libfuse2 installed (apt install libfuse2)
#   - dinero-v8 already built with -DDINERO_BUILD_QT=ON under build-linux/
#
# Usage:
#   bash packaging/linux/build-appimage.sh 8.0.0-rc3
#   bash packaging/linux/build-appimage.sh 8.0.0-rc3 /path/to/build-linux /path/to/out

set -euo pipefail

VERSION="${1:-8.0.0-dev}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${2:-${PROJECT_ROOT}/build-linux}"
OUTPUT_DIR="${3:-${PROJECT_ROOT}/dist}"

NAME="dinero-v${VERSION}-linux-x86_64"
APPDIR="${OUTPUT_DIR}/${NAME}.AppDir"

echo "----------------------------------------------------------"
echo "Building Dinero AppImage -- v${VERSION}"
echo "----------------------------------------------------------"
echo "BUILD_DIR : ${BUILD_DIR}"
echo "OUTPUT_DIR: ${OUTPUT_DIR}"

# Locate linuxdeploy + plugin
LINUXDEPLOY="$(command -v linuxdeploy-x86_64.AppImage || echo "${HOME}/bin/linuxdeploy-x86_64.AppImage")"
LINUXDEPLOY_QT="$(command -v linuxdeploy-plugin-qt-x86_64.AppImage || echo "${HOME}/bin/linuxdeploy-plugin-qt-x86_64.AppImage")"
if [ ! -x "${LINUXDEPLOY}" ]; then
    echo "ERROR: linuxdeploy-x86_64.AppImage not found (looked in PATH and ~/bin)" >&2
    exit 1
fi
if [ ! -x "${LINUXDEPLOY_QT}" ]; then
    echo "ERROR: linuxdeploy-plugin-qt-x86_64.AppImage not found (looked in PATH and ~/bin)" >&2
    exit 1
fi

# Verify expected binaries are present in the build dir.
DINERO_QT="${BUILD_DIR}/bin/dinero-qt"
if [ ! -x "${DINERO_QT}" ]; then
    echo "ERROR: dinero-qt not found at ${DINERO_QT}" >&2
    echo "Build it with: cmake --build ${BUILD_DIR} --target dinero-qt -j6" >&2
    exit 1
fi
# dinero-gpu-miner and dinero-seeder are part of the shipped desktop stack --
# the macOS .app embeds both, and the Qt GUI exposes GPU mining plus a Start
# Seeder control. Omitting them here meant the Linux desktop bundle silently
# shipped without the GPU miner users are told to mine with.
DAEMON_BINS=(dinerod dinero-cli dinero-miner dinero-stratum-worker dinero-wallet-cli dinero-gpu-miner)
for b in "${DAEMON_BINS[@]}"; do
    if [ ! -x "${BUILD_DIR}/${b}" ]; then
        echo "ERROR: ${b} not found at ${BUILD_DIR}/${b}" >&2
        exit 1
    fi
done
SOLO_MINER="${BUILD_DIR}/miner/dinero-solo-miner"
if [ ! -x "${SOLO_MINER}" ]; then
    echo "ERROR: dinero-solo-miner not found at ${SOLO_MINER}" >&2
    exit 1
fi
# Like the solo miner, the seeder builds into its own subdirectory.
SEEDER="${BUILD_DIR}/seeder/dinero-seeder"
if [ ! -x "${SEEDER}" ]; then
    echo "ERROR: dinero-seeder not found at ${SEEDER}" >&2
    echo "Build it with: cmake --build ${BUILD_DIR} --target dinero-seeder" >&2
    exit 1
fi

# SV2 pool miners come from the separate Rust repo (DineroLabs/dinero-sv2),
# built to target/release/. They are NOT produced by this project's CMake, so
# a missing checkout silently yields a bundle with no pool miners -- which is
# exactly how the first v8.1.1 macOS build shipped without them. Fail loudly
# instead, and allow an explicit override for out-of-tree checkouts.
SV2_ROOT="${DINERO_SV2_SOURCE_ROOT:-${PROJECT_ROOT}/../dinero-sv2}"
SV2_BINS=(dinero-sv2-miner dinero-sv2-gpu-miner)
for b in "${SV2_BINS[@]}"; do
    if [ ! -x "${SV2_ROOT}/target/release/${b}" ]; then
        echo "ERROR: ${b} not found at ${SV2_ROOT}/target/release/${b}" >&2
        echo "Clone DineroLabs/dinero-sv2 and run: cargo build --release" >&2
        echo "Or point DINERO_SV2_SOURCE_ROOT at an existing checkout." >&2
        exit 1
    fi
done

# Reset AppDir
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin"
mkdir -p "${APPDIR}/usr/share/applications"
mkdir -p "${APPDIR}/usr/share/icons/hicolor/256x256/apps"

# Copy executables. dinero-qt starts the bundled dinerod, so it needs
# to live in the same usr/bin/ next to the daemon binaries.
echo "Staging binaries..."
cp "${DINERO_QT}" "${APPDIR}/usr/bin/dinero-qt"
for b in "${DAEMON_BINS[@]}"; do
    cp "${BUILD_DIR}/${b}" "${APPDIR}/usr/bin/${b}"
done
cp "${SOLO_MINER}" "${APPDIR}/usr/bin/dinero-solo-miner"
cp "${SEEDER}" "${APPDIR}/usr/bin/dinero-seeder"
for b in "${SV2_BINS[@]}"; do
    cp "${SV2_ROOT}/target/release/${b}" "${APPDIR}/usr/bin/${b}"
done

# Assert the shipped binary set. Both bugs this guard exists for were SILENT:
# build-appimage.sh simply never listed dinero-gpu-miner/dinero-seeder, and the
# macOS build resolved an SV2 path that did not exist and bundled nothing. In
# both cases packaging reported success while shipping an incomplete wallet.
# Bundling "whatever resolved" is not a release contract; this is.
EXPECTED_BINS=(
    dinerod dinero-cli dinero-qt dinero-seeder
    dinero-miner dinero-solo-miner dinero-gpu-miner
    dinero-stratum-worker dinero-wallet-cli
    dinero-sv2-miner dinero-sv2-gpu-miner
)
missing=0
for b in "${EXPECTED_BINS[@]}"; do
    if [ ! -x "${APPDIR}/usr/bin/${b}" ]; then
        echo "ERROR: expected binary missing from bundle: ${b}" >&2
        missing=1
    fi
done
if [ "${missing}" -ne 0 ]; then
    echo "The desktop bundle is incomplete. Either build the missing target or" >&2
    echo "remove it from EXPECTED_BINS deliberately -- do not ship silently." >&2
    exit 1
fi
echo "Bundle contains all ${#EXPECTED_BINS[@]} expected binaries."

# Icon. The qt/ subdir's Dinero.png is the canonical 1024x1024 source
# that the macOS .icns and Windows .ico are regenerated from. linuxdeploy
# enforces freedesktop icon-resolution conventions (8/16/.../256/512), so
# we resize down to 256x256 to match the hicolor/256x256 path. ImageMagick
# `convert` is a standard Ubuntu/Debian dep — if it's missing the AppImage
# will be unusable anyway because the qt plugin pulls in other graphics
# tooling.
ICON_SRC="${PROJECT_ROOT}/qt/Dinero.png"
if [ ! -f "${ICON_SRC}" ]; then
    echo "ERROR: icon not found at ${ICON_SRC}" >&2
    exit 1
fi
ICON_DST="${APPDIR}/usr/share/icons/hicolor/256x256/apps/dinero-qt.png"
if command -v convert >/dev/null 2>&1; then
    convert "${ICON_SRC}" -resize 256x256 "${ICON_DST}"
elif command -v magick >/dev/null 2>&1; then
    magick "${ICON_SRC}" -resize 256x256 "${ICON_DST}"
else
    echo "ERROR: ImageMagick (convert/magick) required to resize ${ICON_SRC} to 256x256" >&2
    echo "Install with: apt install imagemagick" >&2
    exit 1
fi

# Desktop file. The Categories field uses freedesktop.org standard
# categories. StartupWMClass should match the X11 WM_CLASS the Qt
# application sets — for Qt apps this is typically the executable
# name with first letter capitalized.
cat > "${APPDIR}/usr/share/applications/dinero-qt.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Dinero
GenericName=Dinero Wallet
Comment=Dinero blockchain wallet and node
Exec=dinero-qt
Icon=dinero-qt
Terminal=false
Categories=Office;Finance;Network;
Keywords=cryptocurrency;blockchain;wallet;dinero;
StartupWMClass=dinero-qt
StartupNotify=true
EOF

# linuxdeploy reads desktop + icon from usr/share, so explicit
# --desktop-file / --icon-file flags aren't required — but pass them
# to be explicit and to surface errors if anything is missing.
echo "Running linuxdeploy + qt plugin..."
cd "${OUTPUT_DIR}"
export VERSION="${VERSION}"
export OUTPUT="${NAME}.AppImage"
# QMAKE points the qt plugin at Ubuntu's qmake6 (Qt 6.2 on 22.04).
export QMAKE="$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake)"
# WSL2 interop adds /mnt/c/... paths to PATH; linuxdeploy walks PATH
# looking for tools and crashes if any of those Windows dirs are
# permission-restricted (e.g. another user's AppData). Scrub PATH down
# to Linux-only locations before invoking linuxdeploy.
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:${HOME}/bin"

"${LINUXDEPLOY}" \
    --appdir "${APPDIR}" \
    --plugin qt \
    --executable "${APPDIR}/usr/bin/dinero-qt" \
    --desktop-file "${APPDIR}/usr/share/applications/dinero-qt.desktop" \
    --icon-file "${APPDIR}/usr/share/icons/hicolor/256x256/apps/dinero-qt.png" \
    --output appimage

APPIMAGE_PATH="${OUTPUT_DIR}/${NAME}.AppImage"
if [ ! -f "${APPIMAGE_PATH}" ]; then
    echo "ERROR: linuxdeploy ran but ${APPIMAGE_PATH} not produced" >&2
    exit 1
fi
chmod +x "${APPIMAGE_PATH}"
APPIMAGE_SIZE=$(stat -c %s "${APPIMAGE_PATH}")
APPIMAGE_HASH=$(sha256sum "${APPIMAGE_PATH}" | awk '{print $1}')

echo ""
echo "----------------------------------------------------------"
echo "AppImage ready"
echo "----------------------------------------------------------"
echo "  Path:   ${APPIMAGE_PATH}"
echo "  Size:   ${APPIMAGE_SIZE} bytes ($(awk "BEGIN{printf \"%.2f\", ${APPIMAGE_SIZE}/1024/1024}") MB)"
echo "  SHA256: ${APPIMAGE_HASH}"
