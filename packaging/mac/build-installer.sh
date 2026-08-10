#!/bin/bash
# packaging/mac/build-installer.sh
#
# Stage the dinerod stack + dinero-qt.app + Qt6 runtime (via macdeployqt,
# already invoked by qt/CMakeLists.txt POST_BUILD), then produce the macOS
# user package and a headless operator tarball:
#
#   dist/Dinero-v<VERSION>-macOS-<ARCH>-qt.zip
#   dist/Dinero-v<VERSION>-macOS-<ARCH>.dmg
#   dist/dinero-operator-v<VERSION>-macOS-<ARCH>.tar.gz
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
#   ./packaging/mac/build-installer.sh --version 8.0.0 --arch x86_64
#
# After this script:
#   ./packaging/mac/sign-notarize-release.sh \
#       --version 8.0.0-rc1 \
#       --notary-profile dinero-notarytool

set -euo pipefail

VERSION="8.0.0-dev"
BUILD_DIR=""
ARCH="${DINERO_RELEASE_ARCH:-$(uname -m)}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --arch) ARCH="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

[[ "$ARCH" == "aarch64" ]] && ARCH="arm64"
case "$ARCH" in
    arm64|x86_64) ;;
    *) echo "ERROR: unsupported macOS release architecture: $ARCH" >&2; exit 2 ;;
esac

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
echo "Architecture: $ARCH"
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

# Bundle the new primary plus the previous desktop release's exact-base fallback.
# The fallback prevents an app upgrade during first-run validation from stranding
# the persisted 65300 lifecycle after the old app bundle has been replaced.
SNAPSHOT_DAT="${DINERO_SNAPSHOT_DAT:-$PROJECT_ROOT/packaging/mac/snapshot/dinero-assumeutxo-84131-v4.dat}"
SNAPSHOT_FALLBACK_DAT="${DINERO_SNAPSHOT_FALLBACK_DAT:-$PROJECT_ROOT/packaging/mac/snapshot/utxo-snapshot-65300.dat}"
SNAPSHOT_MANIFEST="${DINERO_SNAPSHOT_MANIFEST:-${SNAPSHOT_DAT%.dat}.manifest.json}"
SNAPSHOT_FALLBACK_MANIFEST="${DINERO_SNAPSHOT_FALLBACK_MANIFEST:-${SNAPSHOT_FALLBACK_DAT}.manifest.json}"

verify_snapshot_pair() {
    local data_path="$1"
    local manifest_path="$2"
    local installed_name="$3"
    python3 - "$data_path" "$manifest_path" "$installed_name" <<'PY'
import hashlib
import json
import pathlib
import sys

data_path = pathlib.Path(sys.argv[1])
manifest_path = pathlib.Path(sys.argv[2])
installed_name = sys.argv[3]
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))["snapshot"]
if manifest.get("snapshot_file") != installed_name:
    raise SystemExit(
        f"manifest snapshot_file={manifest.get('snapshot_file')!r}, expected {installed_name!r}"
    )
payload = data_path.read_bytes()
actual_sha256 = hashlib.sha256(payload).hexdigest()
if manifest.get("sha256", "").lower() != actual_sha256:
    raise SystemExit(
        f"snapshot sha256={actual_sha256}, manifest={manifest.get('sha256')!r}"
    )
if int(manifest.get("bytes", -1)) != len(payload):
    raise SystemExit(
        f"snapshot bytes={len(payload)}, manifest={manifest.get('bytes')!r}"
    )
print(f"Verified {installed_name}: {len(payload)} bytes, sha256 {actual_sha256}")
PY
}

if [[ "${DINERO_SKIP_SNAPSHOTS:-0}" == "1" ]]; then
    echo "WARNING: snapshot bundling explicitly disabled (DINERO_SKIP_SNAPSHOTS=1)" >&2
elif [[ ! -f "$SNAPSHOT_DAT" ]]; then
    echo "ERROR: required primary AssumeUTXO snapshot missing at $SNAPSHOT_DAT" >&2
    echo "       Set DINERO_SNAPSHOT_DAT or use DINERO_SKIP_SNAPSHOTS=1 only for a developer package." >&2
    exit 1
elif [[ ! -f "$SNAPSHOT_FALLBACK_DAT" ]]; then
    echo "ERROR: required AssumeUTXO lifecycle fallback missing at $SNAPSHOT_FALLBACK_DAT" >&2
    exit 1
else
    echo "Bundling AssumeUTXO snapshot: $(basename "$SNAPSHOT_DAT") ($(stat -f%z "$SNAPSHOT_DAT") bytes)"
    [[ -f "$SNAPSHOT_MANIFEST" ]] || { echo "ERROR: primary snapshot manifest missing at $SNAPSHOT_MANIFEST" >&2; exit 1; }
    verify_snapshot_pair "$SNAPSHOT_DAT" "$SNAPSHOT_MANIFEST" "dinero-assumeutxo-84131-v4.dat"
    cp "$SNAPSHOT_DAT" "$STAGE_DIR/dinero-qt.app/Contents/Resources/dinero-assumeutxo-84131-v4.dat"
    cp "$SNAPSHOT_MANIFEST" "$STAGE_DIR/dinero-qt.app/Contents/Resources/dinero-assumeutxo-84131-v4.dat.manifest.json"
    echo "Bundling AssumeUTXO lifecycle fallback: $(basename "$SNAPSHOT_FALLBACK_DAT")"
    [[ -f "$SNAPSHOT_FALLBACK_MANIFEST" ]] || { echo "ERROR: fallback snapshot manifest missing at $SNAPSHOT_FALLBACK_MANIFEST" >&2; exit 1; }
    verify_snapshot_pair "$SNAPSHOT_FALLBACK_DAT" "$SNAPSHOT_FALLBACK_MANIFEST" "utxo-snapshot-65300.dat"
    cp "$SNAPSHOT_FALLBACK_DAT" "$STAGE_DIR/dinero-qt.app/Contents/Resources/utxo-snapshot-65300.dat"
    cp "$SNAPSHOT_FALLBACK_MANIFEST" "$STAGE_DIR/dinero-qt.app/Contents/Resources/utxo-snapshot-65300.dat.manifest.json"
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
ZIP_PATH="$DIST_DIR/Dinero-v${VERSION}-macOS-${ARCH}-qt.zip"
echo ""
echo "Producing $ZIP_PATH..."
ditto -c -k --sequesterRsrc --keepParent "$STAGE_DIR/dinero-qt.app" "$ZIP_PATH"

# Produce the user-facing DMG.
# Drag-to-install affordance: an "Applications" shortcut inside the DMG so the
# user drags dinero-qt.app straight onto it in the same window. Everything (the
# daemon, CLI, miners, seeder, and the AssumeUTXO snapshot) is embedded INSIDE
# dinero-qt.app; the loose binaries beside it are optional standalone copies for
# advanced users and are not needed to install or run the wallet.
ln -sfn /Applications "$STAGE_DIR/Applications"
DMG_PATH="$DIST_DIR/Dinero-v${VERSION}-macOS-${ARCH}.dmg"
echo "Producing $DMG_PATH..."
hdiutil create \
    -volname "Dinero v${VERSION}" \
    -srcfolder "$STAGE_DIR" \
    -ov -format UDZO \
    "$DMG_PATH"

# Produce a headless operator archive for macOS node operators. This is
# intentionally separate from the GUI DMG so server-style macOS hosts do
# not have to install or notarize the wallet bundle just to run dinerod.
OPERATOR_ROOT="$OPERATOR_STAGE_DIR/dinero-operator-v${VERSION}-macOS-${ARCH}"
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
OPERATOR_TARBALL="$DIST_DIR/dinero-operator-v${VERSION}-macOS-${ARCH}.tar.gz"
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
echo "      --arch $ARCH \\"
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
