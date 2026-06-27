#!/bin/bash
# packaging/mac/build-installer.sh
#
# Stage the dinerod stack + dinero-qt.app + Qt6 runtime (via macdeployqt,
# already invoked by qt/CMakeLists.txt POST_BUILD), then produce the macOS
# user package and a headless operator tarball:
#
#   dist/Dinero-v<VERSION>-macOS-arm64-qt.zip
#   dist/Dinero-v<VERSION>-macOS-arm64.dmg
#   dist/dinero-operator-v<VERSION>-macOS-arm64.tar.gz
#
# Operator runs packaging/mac/sign-notarize-release.sh after this script.
# That finalizer signs the *final staged app* after all helper binaries have
# been copied, notarizes/staples the app, rebuilds the public ZIP/DMG, signs
# and notarizes/staples the DMG, and writes the final checksum file.
#
# This is the macOS counterpart to packaging/windows/build-installer.ps1
# and packaging/linux/build-installer.sh — same -Version flag, same
# staging pattern, same expectation of "build first, then run me".
#
# Prerequisites (one-time per machine):
#   - Xcode Command Line Tools (xcode-select --install)
#   - Qt 6.9.x for macOS at $HOME/Qt/6.9.1/macos
#   - hdiutil + codesign + xcrun (ship with macOS)
#   - The monorepo stack already built under build-release/, e.g.:
#       cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
#             -DDINERO_BUILD_QT=ON -DDINERO_BUILD_MINER=ON \
#             -DDINERO_BUILD_SEEDER=ON \
#             -DMINER_ENABLE_METAL=ON -DMINER_ENABLE_OPENCL=ON \
#             -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.1/macos"
#       cmake --build build-release -j16 \
#             --target dinerod dinero-cli dinero-solo-miner-cli \
#                      dinero-qt dinero-seeder
#
# Usage:
#   ./packaging/mac/build-installer.sh --version 8.0.0-rc1
#   ./packaging/mac/build-installer.sh --version 8.0.0 --build-dir build-release
#
# After this script:
#   ./packaging/mac/sign-notarize-release.sh \
#       --version 8.0.0-rc1 \
#       --notary-profile dinero-notarytool

set -euo pipefail

VERSION="8.0.0-dev"
BUILD_DIR=""
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
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

DIST_DIR="$PROJECT_ROOT/packaging/mac/dist"
STAGE_DIR="$DIST_DIR/stage"
OPERATOR_STAGE_DIR="$DIST_DIR/operator-stage"
APP_BUNDLE="$BUILD_DIR/bin/dinero-qt.app"

if [[ ! -d "$APP_BUNDLE" ]]; then
    echo "ERROR: dinero-qt.app not found at $APP_BUNDLE" >&2
    echo "Run cmake --build $BUILD_DIR --target dinero-qt first." >&2
    exit 1
fi

"$PROJECT_ROOT/packaging/mac/assert-v8-release-lane.sh" \
    --version "$VERSION" \
    --build-dir "$BUILD_DIR" \
    --app "$APP_BUNDLE"

# Verify the daemon stack the Qt app embeds. dinero-seeder is required because
# the Cmd+K dashboard exposes a Start Seeder control; shipping the GUI without
# the helper makes that button fail at runtime.
for bin in dinerod dinero-cli seeder/dinero-seeder; do
    if [[ ! -x "$BUILD_DIR/$bin" ]]; then
        echo "ERROR: $bin not found at $BUILD_DIR/$bin" >&2
        echo "Run cmake --build $BUILD_DIR --target dinerod dinero-cli dinero-seeder first." >&2
        exit 1
    fi
done

# Startup smoke test (release gate): the daemon stack the .app embeds MUST
# actually launch. rc30 shipped a macOS dinerod that aborted at startup (a
# static initializer logged before the logger mutex was constructed -> SIGABRT,
# PR #227) because it was packaged + notarized without ever being run. `--version`
# exercises all static initializers then exits; a launch crash kills the process
# with a signal (exit >= 128) or prints a C++ terminate/abort message. Refuse to
# package a broken release.
echo "Smoke-testing the daemon stack launches (--version)..."
for bin in dinerod dinero-cli seeder/dinero-seeder; do
    if smoke_out=$("$BUILD_DIR/$bin" --version 2>&1); then smoke_rc=0; else smoke_rc=$?; fi
    if [[ "$smoke_rc" -ge 128 ]] || printf '%s' "$smoke_out" | grep -qiE "libc\+\+abi|terminating due to|mutex lock failed|Abort trap|Segmentation fault"; then
        echo "ERROR: $bin crashed at startup (exit $smoke_rc) -- refusing to package a broken release." >&2
        printf '%s\n' "$smoke_out" | tail -5 >&2
        exit 1
    fi
done
echo "  OK: daemon stack launches cleanly"

echo "----------------------------------------------------------"
echo "Building Dinero macOS installer -- v$VERSION"
echo "----------------------------------------------------------"

rm -rf "$DIST_DIR"
mkdir -p "$STAGE_DIR"
mkdir -p "$OPERATOR_STAGE_DIR/bin"

echo "Copying dinero-qt.app (with embedded dinerod + Qt frameworks)..."
ditto "$APP_BUNDLE" "$STAGE_DIR/dinero-qt.app"

# Keep the drag-to-Applications install self-contained for advanced
# network operators. The companion binary remains available at the DMG
# root too, but embedding it inside the .app lets future Qt UI affordances
# launch the seeder without requiring users to preserve sidecar files.
cp "$BUILD_DIR/seeder/dinero-seeder" "$STAGE_DIR/dinero-qt.app/Contents/MacOS/dinero-seeder"
chmod +x "$STAGE_DIR/dinero-qt.app/Contents/MacOS/dinero-seeder"
mkdir -p "$STAGE_DIR/dinero-qt.app/Contents/Resources"
cp "$BUILD_DIR/seeder/dinero-seeder" "$STAGE_DIR/dinero-qt.app/Contents/Resources/dinero-seeder"
chmod +x "$STAGE_DIR/dinero-qt.app/Contents/Resources/dinero-seeder"

# Bundle the AssumeUTXO snapshot so a FRESH wallet fast-syncs from it instead of
# syncing from genesis (the slow, catch-up-stall-prone first run). qt/src/main.cpp
# passes --assumeutxo_snapshot pointing at this file on a fresh datadir, and the
# daemon verifies its SHA256 against the compiled-in trust anchor before use.
# The .dat is NOT committed to git (~19 MB) — place it at $DINERO_SNAPSHOT_DAT or
# the default path below (e.g. the published release asset) before packaging.
# Missing → the wallet still works, just syncs from genesis (logged); build does
# not fail.
SNAPSHOT_DAT="${DINERO_SNAPSHOT_DAT:-$PROJECT_ROOT/packaging/mac/snapshot/utxo-snapshot-52066.dat}"
if [[ -f "$SNAPSHOT_DAT" ]]; then
    echo "Bundling AssumeUTXO snapshot: $(basename "$SNAPSHOT_DAT") ($(stat -f%z "$SNAPSHOT_DAT") bytes)"
    cp "$SNAPSHOT_DAT" "$STAGE_DIR/dinero-qt.app/Contents/Resources/utxo-snapshot-52066.dat"
else
    echo "WARNING: AssumeUTXO snapshot not found at $SNAPSHOT_DAT — fresh wallets will sync from genesis." >&2
    echo "         Set DINERO_SNAPSHOT_DAT or place the .dat there to bundle fast-sync." >&2
fi

# Embed the standalone daemon stack alongside the .app for users who
# want the CLI tools without launching the Qt UI. Mirrors what the
# Linux tarball + Windows installer ship.
echo "Copying standalone daemon binaries..."
for bin in dinerod dinero-cli; do
    if [[ -x "$BUILD_DIR/$bin" ]]; then
        cp "$BUILD_DIR/$bin" "$STAGE_DIR/$bin"
    fi
done

# Optional: solo miner CLI if it was built.
if [[ -x "$BUILD_DIR/miner/dinero-solo-miner" ]]; then
    cp "$BUILD_DIR/miner/dinero-solo-miner" "$STAGE_DIR/dinero-solo-miner"
fi

cp "$BUILD_DIR/seeder/dinero-seeder" "$STAGE_DIR/dinero-seeder"

# Optional: standalone miner / wallet tools if this release host built them.
for bin in dinero-gpu-miner dinero-miner dinero-stratum-worker dinero-wallet-cli; do
    if [[ -x "$BUILD_DIR/$bin" ]]; then
        cp "$BUILD_DIR/$bin" "$STAGE_DIR/$bin"
    fi
done

cp "$PROJECT_ROOT/LICENSE" "$STAGE_DIR/LICENSE" 2>/dev/null || true

echo ""
echo "Stage contents:"
find "$STAGE_DIR" -maxdepth 2 -type f -o -type d -maxdepth 1 | sort

# Produce a .zip first for notarization (Apple's notarytool takes
# .zip or .dmg; .zip avoids the disk-image quirks).
ZIP_PATH="$DIST_DIR/Dinero-v${VERSION}-macOS-arm64-qt.zip"
echo ""
echo "Producing $ZIP_PATH..."
ditto -c -k --sequesterRsrc --keepParent "$STAGE_DIR/dinero-qt.app" "$ZIP_PATH"

# Produce the user-facing DMG.
DMG_PATH="$DIST_DIR/Dinero-v${VERSION}-macOS-arm64.dmg"
echo "Producing $DMG_PATH..."
hdiutil create \
    -volname "Dinero v${VERSION}" \
    -srcfolder "$STAGE_DIR" \
    -ov -format UDZO \
    "$DMG_PATH"

# Produce a headless operator archive for macOS node operators. This is
# intentionally separate from the GUI DMG so server-style macOS hosts do
# not have to install or notarize the wallet bundle just to run dinerod.
OPERATOR_ROOT="$OPERATOR_STAGE_DIR/dinero-operator-v${VERSION}-macOS-arm64"
mkdir -p "$OPERATOR_ROOT/bin"
cp "$BUILD_DIR/dinerod" "$OPERATOR_ROOT/bin/dinerod"
cp "$BUILD_DIR/dinero-cli" "$OPERATOR_ROOT/bin/dinero-cli"
cp "$BUILD_DIR/seeder/dinero-seeder" "$OPERATOR_ROOT/bin/dinero-seeder"
cp "$PROJECT_ROOT/LICENSE" "$OPERATOR_ROOT/LICENSE" 2>/dev/null || true
cat > "$OPERATOR_ROOT/README.txt" <<EOF
Dinero macOS operator archive ${VERSION}

This archive is for headless macOS node operators. It includes the daemon,
RPC CLI, and dinero-seeder.

Common entry points:
  ./bin/dinerod
  ./bin/dinero-cli
  ./bin/dinero-seeder
EOF
(cd "$OPERATOR_ROOT" && shasum -a 256 bin/* > SHA256SUMS)
OPERATOR_TARBALL="$DIST_DIR/dinero-operator-v${VERSION}-macOS-arm64.tar.gz"
echo "Producing $OPERATOR_TARBALL..."
tar -czf "$OPERATOR_TARBALL" -C "$OPERATOR_STAGE_DIR" "$(basename "$OPERATOR_ROOT")"

# Hashes for SHA256SUMS-vNNN.
ZIP_HASH=$(shasum -a 256 "$ZIP_PATH" | awk '{print $1}')
DMG_HASH=$(shasum -a 256 "$DMG_PATH" | awk '{print $1}')
OPERATOR_HASH=$(shasum -a 256 "$OPERATOR_TARBALL" | awk '{print $1}')

echo ""
echo "----------------------------------------------------------"
echo "Stage ready (unsigned/not notarized). Next step for the operator:"
echo "----------------------------------------------------------"
echo "  ./packaging/mac/sign-notarize-release.sh \\"
echo "      --version $VERSION \\"
echo "      --notary-profile dinero-notarytool"
echo ""
echo "  Unsigned $ZIP_PATH"
echo "    SHA256: $ZIP_HASH"
echo "  Unsigned $DMG_PATH"
echo "    SHA256: $DMG_HASH"
echo "  Operator $OPERATOR_TARBALL"
echo "    SHA256: $OPERATOR_HASH"
echo ""
echo "Do not upload macOS GUI artifacts until the finalizer reports"
echo "Notarized Developer ID for both the app and DMG."
