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
SIGN_HELPER="$(dirname "$0")/../cmake/sign_macos_bundle.py"
TEAM_ID="JXJS6ZA5FJ"

if [ ! -d "$APP" ]; then
    echo "ERROR: App bundle not found: $APP"
    exit 1
fi

echo "==> Signing: $APP"
echo "    Identity: $IDENTITY"
echo "    Entitlements: $ENTITLEMENTS"
echo ""

# Sign all nested Mach-O files inside-out, then seal the outer bundle.
#
# Keep one recursive implementation for both build-time ad-hoc signing and
# Developer-ID release signing. The former top-level Resources/* loop missed
# embedded Tor's nested tor/tor/{tor,libevent,pluggable_transports/*} files.
# codesign --deep made the outer bundle look locally valid, but did not replace
# those vendor/ad-hoc signatures with timestamped hardened-runtime signatures;
# Apple notarization correctly rejected the archive.
echo "--- Signing nested components..."
python3 "$SIGN_HELPER" \
    "$APP" \
    --identity "$IDENTITY" \
    --entitlements "$ENTITLEMENTS" \
    --runtime

# Verify the sealed bundle after every nested Mach-O has its own signature.
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
