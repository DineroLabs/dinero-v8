#!/bin/bash

# macOS Packaging Script for Dinero Desktop
# Creates a signed, notarized DMG ready for distribution

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
PACKAGE_DIR="$PROJECT_ROOT/packaging/mac"
OUTPUT_DIR="$PACKAGE_DIR/output"

# Configuration
APP_NAME="Dinero Desktop"
APP_BUNDLE="dinero-desktop.app"
DMG_NAME="DineroDesktop-2.1.2"
DEVELOPER_ID="Developer ID Application: Your Name (TEAM_ID)"
NOTARIZATION_PROFILE="notarization-profile"

echo "🍎 macOS Packaging for Dinero Desktop"
echo "======================================"
echo ""

# Clean and create output directory
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# Check if app bundle exists
if [ ! -d "$BUILD_DIR/src/gui-desktop/$APP_BUNDLE" ]; then
    echo "❌ App bundle not found: $BUILD_DIR/src/gui-desktop/$APP_BUNDLE"
    echo "Please build the application first with: make dinero-desktop"
    exit 1
fi

echo "📦 Step 1: Preparing app bundle..."
cp -R "$BUILD_DIR/src/gui-desktop/$APP_BUNDLE" "$OUTPUT_DIR/"

# Fix Qt framework paths and sign
echo "🔧 Step 2: Fixing framework paths..."
cd "$OUTPUT_DIR"

# Run the Qt deployment tool
if command -v macdeployqt >/dev/null 2>&1; then
    macdeployqt "$APP_BUNDLE" -verbose=2
else
    echo "⚠️ macdeployqt not found. Install Qt development tools."
    echo "Frameworks may not be properly bundled."
fi

# Code signing (if certificate available)
echo "✍️ Step 3: Code signing..."
if security find-identity -v -p codesigning | grep -q "Developer ID Application"; then
    echo "📝 Signing app bundle..."
    codesign --force --deep --sign "$DEVELOPER_ID" \
             --options runtime \
             --entitlements "$PACKAGE_DIR/entitlements.plist" \
             "$APP_BUNDLE"
    
    echo "✅ Code signing completed"
    
    # Verify signature
    codesign --verify --verbose=4 "$APP_BUNDLE"
    spctl --assess --verbose=4 "$APP_BUNDLE"
else
    echo "⚠️ No Developer ID certificate found. App will not be signed."
    echo "For distribution, you'll need to sign with a Developer ID certificate."
fi

# Create DMG
echo "💿 Step 4: Creating DMG..."
hdiutil create -volname "$APP_NAME" \
               -srcfolder "$APP_BUNDLE" \
               -ov -format UDZO \
               "$DMG_NAME.dmg"

# Sign DMG (if certificate available)
if security find-identity -v -p codesigning | grep -q "Developer ID Application"; then
    echo "📝 Signing DMG..."
    codesign --force --sign "$DEVELOPER_ID" "$DMG_NAME.dmg"
    
    # Notarization (requires Apple ID and app-specific password)
    echo "📋 Step 5: Notarization..."
    echo "To notarize, run:"
    echo "xcrun notarytool submit '$DMG_NAME.dmg' --keychain-profile '$NOTARIZATION_PROFILE' --wait"
    echo "xcrun stapler staple '$DMG_NAME.dmg'"
else
    echo "⚠️ Skipping notarization (no certificate)"
fi

echo ""
echo "✅ macOS packaging complete!"
echo "📁 Output: $OUTPUT_DIR/$DMG_NAME.dmg"
echo ""
echo "🚀 Ready for distribution!"
