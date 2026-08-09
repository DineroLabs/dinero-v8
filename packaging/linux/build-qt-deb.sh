#!/usr/bin/env bash
# Build a Debian package for the Dinero Qt desktop wallet.
#
# Produces: dist/dinero-qt-desktop_<VERSION>-1_amd64.deb
#
# This package is intentionally self-contained: it ships dinero-qt
# alongside the same complete daemon/miner/helper set as the AppImage under
# /opt/dinero-qt-desktop/ rather than depending on the dinero-core
# .deb. That keeps the two packages decoupled — installing the GUI
# does NOT also enable the systemd packaged-service node.
#
# Qt6 runtime libraries are declared as system Depends (libqt6widgets6,
# libqt6network6, libqt6gui6, libqt6svg6) so the .deb stays small
# (~10 MB) — install on Ubuntu 22.04+ derivatives that ship Qt 6.2+.
#
# Usage:
#   bash packaging/linux/build-qt-deb.sh 8.0.0-rc3
#   bash packaging/linux/build-qt-deb.sh 8.0.0-rc3 /path/to/build-linux /path/to/dist

set -euo pipefail

VERSION="${1:-8.0.0-dev}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${2:-${PROJECT_ROOT}/build-linux}"
DIST_DIR="${3:-${PROJECT_ROOT}/dist}"

# Debian version: 8.0.0-rc3 -> 8.0.0~rc3-1 (the ~ sorts before -, so
# 8.0.0~rc3 is less than 8.0.0, which is what we want for rc -> final).
DEB_VERSION="${VERSION//-rc/~rc}-1"
DEB_FILENAME="dinero-qt-desktop_${DEB_VERSION}_amd64.deb"
STAGE="${DIST_DIR}/dinero-qt-desktop-stage"

echo "----------------------------------------------------------"
echo "Building dinero-qt-desktop .deb -- v${VERSION} (deb ${DEB_VERSION})"
echo "----------------------------------------------------------"

# Verify the complete desktop binary contract. Keeping this list aligned with
# build-appimage.sh prevents one Linux package from silently losing controls
# that work in another package format.
SV2_ROOT="${DINERO_SV2_SOURCE_ROOT:-${PROJECT_ROOT}/../dinero-sv2}"
BIN_NAMES=(
    dinero-qt dinerod dinero-cli dinero-seeder
    dinero-miner dinero-solo-miner dinero-gpu-miner
    dinero-stratum-worker dinero-wallet-cli
    dinero-sv2-miner dinero-sv2-gpu-miner
)
BIN_PATHS=(
    "${BUILD_DIR}/bin/dinero-qt"
    "${BUILD_DIR}/dinerod"
    "${BUILD_DIR}/dinero-cli"
    "${BUILD_DIR}/seeder/dinero-seeder"
    "${BUILD_DIR}/dinero-miner"
    "${BUILD_DIR}/miner/dinero-solo-miner"
    "${BUILD_DIR}/dinero-gpu-miner"
    "${BUILD_DIR}/dinero-stratum-worker"
    "${BUILD_DIR}/dinero-wallet-cli"
    "${SV2_ROOT}/target/release/dinero-sv2-miner"
    "${SV2_ROOT}/target/release/dinero-sv2-gpu-miner"
)
for i in "${!BIN_NAMES[@]}"; do
    if [ ! -x "${BIN_PATHS[$i]}" ]; then
        echo "ERROR: expected desktop binary missing: ${BIN_NAMES[$i]} (${BIN_PATHS[$i]})" >&2
        exit 1
    fi
done

# Reset stage.
rm -rf "${STAGE}"
mkdir -p "${STAGE}/DEBIAN"
mkdir -p "${STAGE}/opt/dinero-qt-desktop/bin"
mkdir -p "${STAGE}/usr/bin"
mkdir -p "${STAGE}/usr/share/applications"
mkdir -p "${STAGE}/usr/share/icons/hicolor/256x256/apps"
mkdir -p "${STAGE}/usr/share/doc/dinero-qt-desktop"

# Binaries under /opt to make it clear these are GUI-bundle copies
# (separate from /usr/bin/dinerod if the operator also installs
# dinero-core later — dinero-core's /usr/bin/dinerod will win in
# PATH order, which is fine because the GUI uses an explicit path).
echo "Staging binaries..."
for i in "${!BIN_NAMES[@]}"; do
    cp "${BIN_PATHS[$i]}" "${STAGE}/opt/dinero-qt-desktop/bin/${BIN_NAMES[$i]}"
done
chmod 0755 "${STAGE}/opt/dinero-qt-desktop/bin/"*
echo "Staged all ${#BIN_NAMES[@]} expected desktop binaries."

# Keep the snapshot files beside dinero-qt, matching its runtime lookup.
"${SCRIPT_DIR}/stage-snapshot-pair.sh" "${STAGE}/opt/dinero-qt-desktop/bin"

# Convenience symlink in /usr/bin so `dinero-qt` is on PATH after
# install. Path is relative so dpkg --root install paths work.
ln -s ../../opt/dinero-qt-desktop/bin/dinero-qt "${STAGE}/usr/bin/dinero-qt"

# Icon.
ICON_SRC="${PROJECT_ROOT}/qt/Dinero.png"
if [ ! -f "${ICON_SRC}" ]; then
    echo "ERROR: icon ${ICON_SRC} not found" >&2
    exit 1
fi
if command -v convert >/dev/null 2>&1; then
    convert "${ICON_SRC}" -resize 256x256 \
        "${STAGE}/usr/share/icons/hicolor/256x256/apps/dinero-qt.png"
else
    echo "ERROR: ImageMagick (convert) required for icon resize" >&2
    exit 1
fi

# .desktop entry.
cat > "${STAGE}/usr/share/applications/dinero-qt.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Dinero
GenericName=Dinero Wallet
Comment=Dinero blockchain wallet and node
Exec=/opt/dinero-qt-desktop/bin/dinero-qt
Icon=dinero-qt
Terminal=false
Categories=Office;Finance;Network;
Keywords=cryptocurrency;blockchain;wallet;dinero;
StartupWMClass=dinero-qt
StartupNotify=true
EOF

# Copyright.
cp "${PROJECT_ROOT}/LICENSE" "${STAGE}/usr/share/doc/dinero-qt-desktop/copyright"

# DEBIAN/control. Section: x11 (desktop GUI), depends on Qt6 system libs.
# The exact Qt6 .so names on Ubuntu 22.04 are libqt6widgets6, libqt6gui6,
# libqt6network6 etc. — dpkg-shlibdeps would compute these automatically
# if we used dpkg-buildpackage, but since we're hand-building we list
# them explicitly. xdg-utils is for xdg-open (RPC URL handling) and is
# a tiny standard dep.
INSTALLED_SIZE=$(du -sk "${STAGE}/opt" "${STAGE}/usr" | awk '{s+=$1} END {print s}')
cat > "${STAGE}/DEBIAN/control" <<EOF
Package: dinero-qt-desktop
Version: ${DEB_VERSION}
Section: x11
Priority: optional
Architecture: amd64
Installed-Size: ${INSTALLED_SIZE}
Maintainer: Trucker2827 <trucker2827@gmail.com>
Homepage: https://github.com/DineroLabs/dinero-v8
Depends: libqt6widgets6 (>= 6.2), libqt6gui6 (>= 6.2), libqt6network6 (>= 6.2), libqt6core6 (>= 6.2), libqt6dbus6 (>= 6.2), libc6 (>= 2.34), libudev1, libusb-1.0-0, libhidapi-hidraw0 | libhidapi-libusb0, ocl-icd-libopencl1, xdg-utils
Description: Dinero (DIN) Qt desktop wallet
 dinero-qt-desktop ships dinero-qt, the Qt6 GUI wallet for the Dinero
 blockchain, together with its full bundled daemon, CLI, seeder, CPU/GPU,
 solo, Stratum, SV2, and wallet helper set. The GUI auto-starts the bundled
 daemon and connects to the v8 bootstrap peers on launch.
 .
 This package is independent from dinero-core. Installing it does NOT
 enable the systemd packaged-service node — the daemon runs only while
 dinero-qt is open, under the user's account. Operators who want a
 system-wide node should install dinero-core as well.
EOF

# postinst: refresh desktop database + icon cache so the new menu
# entry shows up immediately. Both commands are no-ops if the helper
# isn't present (Debian server installs without a desktop env).
cat > "${STAGE}/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
    fi
fi
exit 0
POSTINST
chmod 0755 "${STAGE}/DEBIAN/postinst"

# postrm: refresh caches again on removal so the menu entry disappears.
cat > "${STAGE}/DEBIAN/postrm" <<'POSTRM'
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
    fi
fi
exit 0
POSTRM
chmod 0755 "${STAGE}/DEBIAN/postrm"

# Build the .deb.
DEB_PATH="${DIST_DIR}/${DEB_FILENAME}"
mkdir -p "${DIST_DIR}"
dpkg-deb --build --root-owner-group "${STAGE}" "${DEB_PATH}"
DEB_SIZE=$(stat -c %s "${DEB_PATH}")
DEB_HASH=$(sha256sum "${DEB_PATH}" | awk '{print $1}')

echo ""
echo "----------------------------------------------------------"
echo ".deb ready"
echo "----------------------------------------------------------"
echo "  Path:   ${DEB_PATH}"
echo "  Size:   ${DEB_SIZE} bytes ($(awk "BEGIN{printf \"%.2f\", ${DEB_SIZE}/1024/1024}") MB)"
echo "  SHA256: ${DEB_HASH}"
echo ""
echo "Quick check:"
echo "  dpkg-deb -I ${DEB_FILENAME}      # package metadata"
echo "  dpkg-deb -c ${DEB_FILENAME}      # file list"
