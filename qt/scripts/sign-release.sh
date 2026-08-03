#!/bin/bash
# sign-release.sh — Properly sign dinero-qt.app with Developer ID for distribution.
# Run this after cmake build, before creating the release DMG/ZIP.
#
# Usage: ./scripts/sign-release.sh [path/to/dinero-qt.app] [signing-identity]
#
# The identity may also be supplied as DINERO_SIGN_IDENTITY. Callers that sign
# a whole release (packaging/mac/sign-notarize-release.sh) pass their own
# --identity through this way, so the app bundle and the DMG cannot end up
# signed by different certificates.
#
# Requirements:
#   - A Developer ID Application certificate in the Keychain. The default below
#     is the historical personal identity; org releases pass the DineroLabs one.
#   - Xcode command line tools

set -euo pipefail

APP="${1:-build/bin/dinero-qt.app}"
# Precedence: positional arg, then env, then the historical default. Previously
# this was hardcoded and BOTH the argument and the environment were ignored, so
# a caller asking for a different identity was silently overridden.
IDENTITY="${2:-${DINERO_SIGN_IDENTITY:-Developer ID Application: Mirsad Hajdarevic (JXJS6ZA5FJ)}}"
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
        # Skip the main bundle executable here — it must be signed LAST (step 2),
        # after every sibling helper. This loop iterates MacOS/* alphabetically,
        # and "dinero-qt" sorts before "dinero-seeder"/"dinero-solo-miner", so
        # signing it here seals the bundle while those helpers are still unsigned
        # → "code object is not signed at all / In subcomponent: dinero-seeder".
        if [ "$f" = "$APP/Contents/MacOS/dinero-qt" ]; then
            continue
        fi
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
