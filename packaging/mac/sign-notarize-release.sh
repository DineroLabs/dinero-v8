#!/usr/bin/env bash
# Finalize macOS release artifacts for public distribution.
#
# This script intentionally runs after packaging/mac/build-installer.sh.
# The packaging step stages dinero-qt.app plus sidecar binaries; this
# finalizer signs that final staged layout, notarizes it, staples tickets,
# rebuilds the public ZIP/DMG/operator artifacts, and writes fresh checksums.
#
# Required:
#   - Developer ID Application certificate in the keychain
#   - notarytool credentials stored with:
#       xcrun notarytool store-credentials <profile> \
#         --apple-id <email> --team-id <team> --password <app-specific-password>
#
# Usage:
#   ./packaging/mac/sign-notarize-release.sh \
#     --version 8.0.0-rc21 \
#     --arch arm64 \
#     --notary-profile dinero-notary

set -euo pipefail

VERSION="8.0.0-dev"
ARCH="${DINERO_RELEASE_ARCH:-$(uname -m)}"
IDENTITY="Developer ID Application: DineroLabs LLC (JXJS6ZA5FJ)"
NOTARY_PROFILE=""
EXPECTED_SOURCE_HEAD=""
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIST_DIR="$PROJECT_ROOT/packaging/mac/dist"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --arch) ARCH="$2"; shift 2 ;;
        --identity) IDENTITY="$2"; shift 2 ;;
        --notary-profile) NOTARY_PROFILE="$2"; shift 2 ;;
        --expected-source-head) EXPECTED_SOURCE_HEAD="$2"; shift 2 ;;
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

if [[ -z "$NOTARY_PROFILE" ]]; then
    echo "ERROR: --notary-profile is required for public macOS releases." >&2
    exit 1
fi

STAGE_DIR="$DIST_DIR/stage"
APP="$STAGE_DIR/dinero-qt.app"
OPERATOR_STAGE_DIR="$DIST_DIR/operator-stage"
OPERATOR_ROOT="$OPERATOR_STAGE_DIR/dinero-operator-v${VERSION}-macOS-${ARCH}"
ZIP_PATH="$DIST_DIR/Dinero-v${VERSION}-macOS-${ARCH}-qt.zip"
DMG_PATH="$DIST_DIR/Dinero-v${VERSION}-macOS-${ARCH}.dmg"
OPERATOR_TARBALL="$DIST_DIR/dinero-operator-v${VERSION}-macOS-${ARCH}.tar.gz"
DESKTOP_SUMS_PATH="$DIST_DIR/SHA256SUMS-macos-${ARCH}-desktop-${VERSION}"
OPERATOR_SUMS_PATH="$DIST_DIR/SHA256SUMS-macos-${ARCH}-${VERSION}"

if [[ ! -d "$APP" ]]; then
    echo "ERROR: staged app not found at $APP" >&2
    echo "Run packaging/mac/build-installer.sh first." >&2
    exit 1
fi

guard_args=(--version "$VERSION" --app "$APP")
if [[ -n "$EXPECTED_SOURCE_HEAD" ]]; then
    guard_args+=(--expected-repo-head "$EXPECTED_SOURCE_HEAD")
fi
"$PROJECT_ROOT/packaging/mac/assert-v8-release-lane.sh" "${guard_args[@]}"

for path in \
    "$APP/Contents/MacOS/dinero-seeder" \
    "$APP/Contents/Resources/dinero-seeder" \
    "$STAGE_DIR/dinero-seeder"; do
    if [[ ! -x "$path" ]]; then
        echo "ERROR: required dinero-seeder missing from staged macOS release: $path" >&2
        echo "Rebuild with -DDINERO_BUILD_SEEDER=ON and target dinero-seeder." >&2
        exit 1
    fi
done

echo "----------------------------------------------------------"
echo "Finalizing Dinero macOS release -- v$VERSION"
echo "----------------------------------------------------------"
echo "Architecture:   $ARCH"
echo "Identity:       $IDENTITY"
echo "Notary profile: $NOTARY_PROFILE"

security find-identity -v -p codesigning | grep -F "$IDENTITY" >/dev/null
xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null

sign_macho_file() {
    local path="$1"
    if [[ ! -f "$path" ]]; then
        return
    fi
    if file "$path" | grep -q "Mach-O"; then
        codesign --force \
            --sign "$IDENTITY" \
            --options runtime \
            --timestamp \
            --entitlements "$PROJECT_ROOT/qt/packaging/entitlements.plist" \
            "$path"
    fi
}

echo ""
echo "Signing staged sidecar binaries..."
find "$STAGE_DIR" -maxdepth 1 -type f -perm +111 -print0 | while IFS= read -r -d '' f; do
    sign_macho_file "$f"
done

echo ""
echo "Signing staged app after final packaging mutations..."
"$PROJECT_ROOT/qt/scripts/sign-release.sh" "$APP" "$IDENTITY"

echo ""
echo "Submitting app bundle for notarization..."
TMPDIR="$(mktemp -d)"
cleanup() {
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

APP_NOTARY_ZIP="$TMPDIR/dinero-qt-app-notary.zip"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$APP_NOTARY_ZIP"
xcrun notarytool submit "$APP_NOTARY_ZIP" \
    --keychain-profile "$NOTARY_PROFILE" \
    --wait
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"
spctl -a -vvv -t exec "$APP"

echo ""
echo "Rebuilding public ZIP from stapled app..."
rm -f "$ZIP_PATH"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP_PATH"

echo ""
echo "Rebuilding, signing, notarizing, and stapling DMG..."
rm -f "$DMG_PATH"
hdiutil create \
    -volname "Dinero v${VERSION}" \
    -srcfolder "$STAGE_DIR" \
    -ov -format UDZO \
    "$DMG_PATH"
codesign --force --sign "$IDENTITY" --timestamp "$DMG_PATH"
codesign --verify --verbose=4 "$DMG_PATH"
xcrun notarytool submit "$DMG_PATH" \
    --keychain-profile "$NOTARY_PROFILE" \
    --wait
xcrun stapler staple "$DMG_PATH"
xcrun stapler validate "$DMG_PATH"
hdiutil verify "$DMG_PATH"
spctl -a -vvv -t open --context context:primary-signature "$DMG_PATH"

echo ""
echo "Rebuilding signed macOS operator archive..."
rm -rf "$OPERATOR_ROOT"
mkdir -p "$OPERATOR_ROOT/bin"
for bin in dinerod dinero-cli dinero-seeder; do
    if [[ -x "$STAGE_DIR/$bin" ]]; then
        cp "$STAGE_DIR/$bin" "$OPERATOR_ROOT/bin/$bin"
        sign_macho_file "$OPERATOR_ROOT/bin/$bin"
    fi
done
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
rm -f "$OPERATOR_TARBALL"
tar -czf "$OPERATOR_TARBALL" -C "$OPERATOR_STAGE_DIR" "$(basename "$OPERATOR_ROOT")"

echo ""
echo "Writing checksums..."
(
    cd "$DIST_DIR"
    shasum -a 256 \
        "$(basename "$ZIP_PATH")" \
        "$(basename "$DMG_PATH")" \
        > "$DESKTOP_SUMS_PATH"
    shasum -a 256 \
        "$(basename "$OPERATOR_TARBALL")" \
        > "$OPERATOR_SUMS_PATH"
)
cat "$DESKTOP_SUMS_PATH"
cat "$OPERATOR_SUMS_PATH"

echo ""
echo "macOS release artifacts are signed, notarized, stapled, and ready:"
echo "  $ZIP_PATH"
echo "  $DMG_PATH"
echo "  $OPERATOR_TARBALL"
echo "  $DESKTOP_SUMS_PATH"
echo "  $OPERATOR_SUMS_PATH"
