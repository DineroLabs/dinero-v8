#!/bin/bash
# Dinero macOS Code Signing & Notarization Script
# Signs the app with Developer ID and submits for notarization
# Version: 1.0

set -e

echo "================================================================"
echo "  Dinero Code Signing & Notarization"
echo "================================================================"
echo ""

# Configuration
DEVELOPER_ID="Mirsad Hajdarevic (JXJS6ZA5FJ)"
TEAM_ID="JXJS6ZA5FJ"
APP_PATH="/Users/haydarevich/Desktop/Mac_Dinero_v0.1.0_7c898171/Dinero.app"
DMG_PATH="/Users/haydarevich/Desktop/Dinero-0.1.0-macOS.dmg"

# Check for required parameters
if [ -z "$1" ]; then
    echo "❌ Error: Apple ID email required"
    echo ""
    echo "Usage: ./sign-and-notarize.sh <apple-id-email> [app-specific-password]"
    echo "Example: ./sign-and-notarize.sh you@apple.com"
    echo ""
    echo "Note: You'll need an app-specific password from:"
    echo "https://appleid.apple.com/account/manage → Security → App-Specific Passwords"
    exit 1
fi

APPLE_ID="$1"
APP_PASSWORD="$2"

# If password not provided, prompt for it
if [ -z "$APP_PASSWORD" ]; then
    echo "Enter app-specific password (or press Ctrl+C to cancel):"
    read -s APP_PASSWORD
    echo ""
fi

echo "Configuration:"
echo "  Developer ID: ${DEVELOPER_ID}"
echo "  Team ID: ${TEAM_ID}"
echo "  Apple ID: ${APPLE_ID}"
echo "  App Path: ${APP_PATH}"
echo ""

# Step 1: Verify app exists
if [ ! -d "${APP_PATH}" ]; then
    echo "❌ Error: App not found at ${APP_PATH}"
    exit 1
fi

echo "✅ App found"
echo ""

# Step 2: Remove extended attributes (quarantine flags)
echo "🔧 Step 1: Removing extended attributes..."
xattr -cr "${APP_PATH}"
echo "  ✅ Extended attributes removed"
echo ""

# Step 3: Sign the app
echo "🔐 Step 2: Signing app with Developer ID..."
echo "  This may take a few minutes..."

codesign --deep --force --options runtime \
  --entitlements /dev/null \
  --sign "Developer ID Application: ${DEVELOPER_ID}" \
  --timestamp \
  "${APP_PATH}"

echo "  ✅ App signed successfully"
echo ""

# Step 4: Verify signature
echo "🔍 Step 3: Verifying code signature..."
codesign --verify --verbose=4 "${APP_PATH}"
echo ""
codesign --display --verbose=4 "${APP_PATH}"
echo ""
echo "  ✅ Signature verified"
echo ""

# Step 5: Check Gatekeeper assessment
echo "🛡️  Step 4: Checking Gatekeeper assessment..."
spctl --assess --verbose "${APP_PATH}" || true
echo ""

# Step 6: Create signed DMG
echo "📦 Step 5: Creating signed DMG..."
if [ -f "${DMG_PATH}" ]; then
    rm -f "${DMG_PATH}"
    echo "  Removed old DMG"
fi

# Run the create-dmg script
cd /Users/haydarevich/Desktop/Mac_Dinero_v0.1.0_7c898171
./create-dmg.sh

echo "  ✅ DMG created: ${DMG_PATH}"
echo ""

# Step 7: Sign the DMG
echo "🔐 Step 6: Signing DMG..."
codesign --sign "Developer ID Application: ${DEVELOPER_ID}" \
  --timestamp \
  "${DMG_PATH}"

echo "  ✅ DMG signed"
echo ""

# Step 8: Submit for notarization
echo "📤 Step 7: Submitting to Apple for notarization..."
echo "  This may take 5-15 minutes..."
echo ""

xcrun notarytool submit "${DMG_PATH}" \
  --apple-id "${APPLE_ID}" \
  --team-id "${TEAM_ID}" \
  --password "${APP_PASSWORD}" \
  --wait

NOTARIZE_STATUS=$?

if [ $NOTARIZE_STATUS -eq 0 ]; then
    echo ""
    echo "  ✅ Notarization successful!"
    echo ""

    # Step 9: Staple the ticket
    echo "📎 Step 8: Stapling notarization ticket to DMG..."
    xcrun stapler staple "${DMG_PATH}"
    echo "  ✅ Ticket stapled"
    echo ""

    # Step 10: Verify stapling
    echo "🔍 Step 9: Verifying stapled ticket..."
    xcrun stapler validate "${DMG_PATH}"
    echo ""

    echo "================================================================"
    echo "  ✅ SUCCESS! Distribution Ready!"
    echo "================================================================"
    echo ""
    echo "Your DMG is now signed and notarized:"
    echo "  ${DMG_PATH}"
    echo ""
    echo "Users will see: 'Dinero' verified by Apple"
    echo "No Gatekeeper warnings!"
    echo ""
    echo "Next steps:"
    echo "  1. Test on a clean Mac (download from website)"
    echo "  2. Upload to GitHub Releases or your website"
    echo "  3. Announce the release!"
    echo ""
else
    echo ""
    echo "❌ Notarization failed"
    echo ""
    echo "To check the error log:"
    echo "  xcrun notarytool log <submission-id> \\"
    echo "    --apple-id ${APPLE_ID} \\"
    echo "    --team-id ${TEAM_ID} \\"
    echo "    --password ${APP_PASSWORD}"
    echo ""
    exit 1
fi
