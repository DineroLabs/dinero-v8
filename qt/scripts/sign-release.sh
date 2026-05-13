#!/bin/bash
# sign-release.sh — Properly sign dinero-qt.app with Developer ID for distribution.
# Run this after cmake build, before creating the release DMG/ZIP.
#
# Usage: ./scripts/sign-release.sh [path/to/dinero-qt.app]
#
# Requirements:
#   - Developer ID Application: Mirsad Hajdarevic (JXJS6ZA5FJ) in Keychain
#   - Xcode command line tools

set -euo pipefail

APP="${1:-build/bin/dinero-qt.app}"
IDENTITY="Developer ID Application: Mirsad Hajdarevic (JXJS6ZA5FJ)"
ENTITLEMENTS="$(dirname "$0")/../packaging/entitlements.plist"
TEAM_ID="JXJS6ZA5FJ"

if [ ! -d "$APP" ]; then
    echo "ERROR: App bundle not found: $APP"
    exit 1
fi

echo "==> Signing: $APP"
echo "    Identity: $IDENTITY"
echo "    Entitlements: $ENTITLEMENTS"
echo ""

# Step 1: Sign all nested executables, frameworks, dylibs, and bundles
# (inside-out: deepest components first, then the outer bundle)
echo "--- Signing nested components..."

# Apple notary requires `--timestamp` (secure timestamp) AND
# `--options runtime` (hardened runtime) on EVERY signed Mach-O,
# including the bundled tool binaries in Resources/ and MacOS/.
# Missing either flag fails notarization with cryptic errors like
# "The signature does not include a secure timestamp" — observed on
# the v2.1.28 first submission, fixed in this script.
SIGN_FLAGS=(--force --sign "$IDENTITY" --options runtime --timestamp --entitlements "$ENTITLEMENTS")

# Sign dylibs and frameworks inside Frameworks/
find "$APP/Contents/Frameworks" \( -name "*.dylib" -o -name "*.framework" \) 2>/dev/null | sort -r | while read f; do
    codesign "${SIGN_FLAGS[@]}" "$f"
done

# Sign PlugIns
find "$APP/Contents/PlugIns" \( -name "*.dylib" -o -name "*.so" -o -name "*.bundle" \) 2>/dev/null | while read f; do
    codesign "${SIGN_FLAGS[@]}" "$f"
done

# Sign every Mach-O executable in Resources/ and MacOS/. The dinero
# bundle ships ~9 standalone tools (dinerod, dinero-cli, dinero-stratum,
# dinero-stratum-worker, dinero-solo-miner, dinero-miner, dinero-gpu-miner,
# dinero-sv2-miner, dinero-sv2-gpu-miner) — all need full notary flags.
echo "--- Signing bundled tool binaries..."
for dir in "$APP/Contents/Resources" "$APP/Contents/MacOS"; do
    [ -d "$dir" ] || continue
    for f in "$dir"/*; do
        [ -f "$f" ] || continue
        if file "$f" 2>/dev/null | grep -q "Mach-O"; then
            codesign "${SIGN_FLAGS[@]}" "$f"
        fi
    done
done

# Step 2: Sign the main executable last (after every nested binary).
echo "--- Signing main executable..."
codesign "${SIGN_FLAGS[@]}" "$APP/Contents/MacOS/dinero-qt"

# Step 3: Sign the entire bundle (outer shell). `--deep` re-signs
# anything we missed; flags must match what notary requires.
echo "--- Signing app bundle..."
codesign "${SIGN_FLAGS[@]}" --deep "$APP"

# Step 4: Verify
echo ""
echo "--- Verifying signature..."
codesign --verify --deep --strict --verbose=2 "$APP"
spctl --assess --type exec --verbose "$APP" 2>&1 || echo "(spctl: Developer ID check — may need notarization for Gatekeeper)"

echo ""
echo "==> Signed successfully: $APP"
echo ""
echo "Next step — notarize for Gatekeeper on other Macs:"
echo "  xcrun notarytool submit $APP --apple-id <email> --team-id $TEAM_ID --password <app-specific-pw> --wait"
echo "  xcrun stapler staple $APP"
