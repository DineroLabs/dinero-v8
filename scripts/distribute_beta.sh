#!/bin/bash

# Dinero Desktop v0.9.0-beta1 Distribution Script

set -e

VERSION="v0.9.0-beta1"
DMG_NAME="dinero-desktop-${VERSION}.dmg"
RELEASE_DIR="releases/dinero-desktop-${VERSION}"

echo "🚀 === DINERO DESKTOP BETA DISTRIBUTION ==="
echo ""

# Check if release package exists
if [ ! -d "$RELEASE_DIR" ]; then
    echo "❌ Release directory not found: $RELEASE_DIR"
    echo "Run the packaging script first"
    exit 1
fi

echo "✅ Release package found: $RELEASE_DIR"

# Create distributable DMG if it doesn't exist
if [ ! -f "$RELEASE_DIR/$DMG_NAME" ]; then
    echo "Creating distributable DMG..."
    
    # Create temporary directory for DMG contents
    TEMP_DMG_DIR=$(mktemp -d)
    
    # Copy app bundle
    cp -r "$RELEASE_DIR/dinero-desktop.app" "$TEMP_DMG_DIR/"
    
    # Copy daemon
    cp "$RELEASE_DIR/dinerod" "$TEMP_DMG_DIR/"
    
    # Copy release notes
    cp RELEASE_NOTES_${VERSION}.md "$TEMP_DMG_DIR/Release Notes.md"
    
    # Create README for testers
    cat > "$TEMP_DMG_DIR/README - Beta Testers.txt" << EOF
Dinero Desktop ${VERSION} - Beta Testing Instructions

INSTALLATION:
1. Drag "dinero-desktop.app" to your Applications folder
2. Launch the app from Applications
3. The app will automatically discover your daemon via nodeinfo.json

TESTING FOCUS:
✅ Real-time events (mining, blocks, transactions)
✅ Connection resilience (daemon restart, network issues)
✅ PSBT transaction workflow
✅ WebSocket performance under load

SAFETY:
- This beta defaults to REGTEST (safe testing)
- For mainnet: explicit checkbox required
- Report any issues or unexpected behavior

FEEDBACK:
- Test real-time mining updates
- Try daemon restart while GUI is open
- Test send transactions with PSBT workflow
- Monitor connection status indicators

Thank you for beta testing!
Dinero Development Team
EOF

    # Create DMG using hdiutil
    hdiutil create -volname "Dinero Desktop $VERSION Beta" \
        -srcfolder "$TEMP_DMG_DIR" \
        -ov -format UDZO \
        "$RELEASE_DIR/$DMG_NAME"
    
    # Cleanup
    rm -rf "$TEMP_DMG_DIR"
    
    echo "✅ DMG created: $RELEASE_DIR/$DMG_NAME"
fi

# Show DMG info
echo ""
echo "📋 DISTRIBUTION PACKAGE:"
ls -lh "$RELEASE_DIR/$DMG_NAME"
echo ""

# Calculate checksums for verification
echo "🔐 CHECKSUMS (for verification):"
echo "SHA256: $(shasum -a 256 "$RELEASE_DIR/$DMG_NAME" | cut -d' ' -f1)"
echo "MD5:    $(md5 -q "$RELEASE_DIR/$DMG_NAME")"
echo ""

# Show distribution options
echo "📤 DISTRIBUTION OPTIONS:"
echo ""
echo "1️⃣ GitHub Releases (Recommended):"
echo "   - Push tag: git push origin $VERSION"
echo "   - Create GitHub release"
echo "   - Attach: $RELEASE_DIR/$DMG_NAME"
echo ""
echo "2️⃣ Cloud Storage:"
echo "   - Upload to Google Drive/Dropbox"
echo "   - Share download link with testers"
echo ""
echo "3️⃣ Direct Email:"
echo "   - File size: $(ls -lh "$RELEASE_DIR/$DMG_NAME" | awk '{print $5}')"
echo "   - May need to use cloud storage if >25MB"
echo ""
echo "4️⃣ TestFlight Alternative (macOS):"
echo "   - Consider using GitHub Releases for version tracking"
echo ""

echo "🎯 BETA TESTING CHECKLIST FOR TESTERS:"
echo ""
echo "✅ Install and launch app"
echo "✅ Connect to daemon (auto-discovery)"
echo "✅ Start mining and watch real-time updates"
echo "✅ Test connection resilience (restart daemon)"
echo "✅ Try PSBT send transaction workflow"
echo "✅ Monitor WebSocket connection status"
echo "✅ Report any crashes or unexpected behavior"
echo ""

echo "🚀 Ready to distribute Dinero Desktop $VERSION Beta!"
