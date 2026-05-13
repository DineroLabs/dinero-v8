#!/bin/bash
# Dinero Update Release Script
# Automates the process of building, signing, and deploying app updates
# Version: 1.0

set -e

echo "================================================================"
echo "  Dinero Update Release Script"
echo "================================================================"
echo ""

# Configuration
SOURCE_DIR="/Users/haydarevich/Documents/DineroCoin"
SPARKLE_DIR="${SOURCE_DIR}/third_party"
DESKTOP_DIR="/Users/haydarevich/Desktop"

# Check for version argument
if [ -z "$1" ]; then
    echo "❌ Error: Version number required"
    echo ""
    echo "Usage: ./release-update.sh <version>"
    echo "Example: ./release-update.sh 0.1.1"
    exit 1
fi

VERSION="$1"
DMG_NAME="Dinero-${VERSION}-macOS"
ZIP_NAME="${DMG_NAME}.zip"
RELEASE_DIR="${DESKTOP_DIR}/Mac_Dinero_v${VERSION}"

echo "Release Version: ${VERSION}"
echo "Release Directory: ${RELEASE_DIR}"
echo ""

# Step 1: Check if release directory exists
if [ ! -d "${RELEASE_DIR}" ]; then
    echo "❌ Error: Release directory not found: ${RELEASE_DIR}"
    echo ""
    echo "Please build the release first:"
    echo "  1. cd ${SOURCE_DIR}"
    echo "  2. Update version in CMakeLists.txt"
    echo "  3. cmake -B build -DCMAKE_BUILD_TYPE=Release"
    echo "  4. cmake --build build -j\$(sysctl -n hw.ncpu)"
    echo "  5. Copy binaries to ${RELEASE_DIR}"
    echo "  6. Run bundle-dependencies.sh"
    exit 1
fi

echo "✅ Release directory found"
echo ""

# Step 2: Check if dependencies are bundled
if [ ! -d "${RELEASE_DIR}/lib" ]; then
    echo "❌ Error: Dependencies not bundled"
    echo ""
    echo "Please run bundle-dependencies.sh first:"
    echo "  cd ${RELEASE_DIR} && ./bundle-dependencies.sh"
    exit 1
fi

echo "✅ Dependencies bundled"
echo ""

# Step 3: Create ZIP archive
echo "📦 Step 1: Creating ZIP archive..."
cd "${DESKTOP_DIR}"

if [ -f "${ZIP_NAME}" ]; then
    rm -f "${ZIP_NAME}"
    echo "  Removed old ZIP"
fi

zip -r -y "${ZIP_NAME}" "$(basename ${RELEASE_DIR})" > /dev/null
FILE_SIZE=$(stat -f%z "${ZIP_NAME}")

echo "  ✅ Created: ${ZIP_NAME}"
echo "  Size: ${FILE_SIZE} bytes ($(du -h ${ZIP_NAME} | awk '{print $1}'))"
echo ""

# Step 4: Sign the update
echo "🔐 Step 2: Signing update with EdDSA..."
cd "${SPARKLE_DIR}"

# Check if private key exists in keychain
if ! security find-generic-password -l "Sparkle" > /dev/null 2>&1; then
    echo "❌ Error: Sparkle private key not found in keychain"
    echo ""
    echo "Please generate keys first:"
    echo "  cd ${SPARKLE_DIR} && ./bin/generate_keys"
    exit 1
fi

SIGNATURE=$(./bin/sign_update "${DESKTOP_DIR}/${ZIP_NAME}")

echo "  ✅ Signature generated:"
echo "  ${SIGNATURE}"
echo ""

# Step 5: Create appcast entry
echo "📝 Step 3: Generating appcast entry..."
cat > "${DESKTOP_DIR}/appcast_entry_${VERSION}.xml" << EOF
    <item>
      <title>Version ${VERSION}</title>
      <description><![CDATA[
        <h2>What's New in v${VERSION}</h2>
        <p><strong>TODO: Add release notes here</strong></p>
        <ul>
          <li>Feature 1</li>
          <li>Feature 2</li>
          <li>Bug fixes and improvements</li>
        </ul>
      ]]></description>
      <pubDate>$(date -u +"%a, %d %b %Y %H:%M:%S %z")</pubDate>
      <enclosure
        url="https://updates.dinero-coin.com/${ZIP_NAME}"
        sparkle:version="${VERSION}"
        sparkle:shortVersionString="${VERSION}"
        sparkle:edSignature="${SIGNATURE}"
        length="${FILE_SIZE}"
        type="application/zip" />
      <sparkle:minimumSystemVersion>10.15</sparkle:minimumSystemVersion>
    </item>
EOF

echo "  ✅ Appcast entry saved to: appcast_entry_${VERSION}.xml"
echo ""

# Step 6: Summary
echo "================================================================"
echo "  ✅ Release Package Ready!"
echo "================================================================"
echo ""
echo "Files created:"
echo "  • ${DESKTOP_DIR}/${ZIP_NAME}"
echo "  • ${DESKTOP_DIR}/appcast_entry_${VERSION}.xml"
echo ""
echo "Next steps:"
echo ""
echo "1. Test the update locally:"
echo "   - Extract ${ZIP_NAME}"
echo "   - Run Dinero.app and verify functionality"
echo ""
echo "2. Update appcast.xml:"
echo "   - Edit ${SPARKLE_DIR}/appcast_example.xml"
echo "   - Add the contents of appcast_entry_${VERSION}.xml at the top"
echo "   - Update release notes with actual changes"
echo ""
echo "3. Deploy to update server:"
echo "   scp ${ZIP_NAME} root@updates.dinero-coin.com:/var/www/updates/"
echo "   scp appcast.xml root@updates.dinero-coin.com:/var/www/updates/"
echo ""
echo "4. Test auto-update:"
echo "   - Launch Dinero v$(awk -F. '{print $1"."$2"."($3-1)}' <<< ${VERSION})"
echo "   - Click Help → Check for Updates"
echo "   - Verify update downloads and installs correctly"
echo ""
echo "================================================================"
echo ""
