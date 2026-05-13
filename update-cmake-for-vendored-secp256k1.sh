#!/bin/bash
set -e

# 🔧 Update CMakeLists.txt to Use Vendored secp256k1
# Replaces all Homebrew references with vendored static library

echo "🔧 Updating CMakeLists.txt for Vendored secp256k1"
echo "================================================="
echo ""

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
CMAKE_FILE="$SCRIPT_DIR/CMakeLists.txt"
CMAKE_BACKUP="$SCRIPT_DIR/CMakeLists.txt.before-vendored"
VENDOR_DIR="$SCRIPT_DIR/vendor"

# Check if vendor directory exists
if [ ! -d "$VENDOR_DIR/lib" ]; then
    echo "❌ Vendor directory not found!"
    echo "   Run: ./build-vendored-secp256k1.sh first"
    exit 1
fi

# Check if libsecp256k1.a exists
if [ ! -f "$VENDOR_DIR/lib/libsecp256k1.a" ]; then
    echo "❌ libsecp256k1.a not found in vendor/lib/"
    echo "   Run: ./build-vendored-secp256k1.sh first"
    exit 1
fi

# Create backup
echo "📋 Creating backup: CMakeLists.txt.before-vendored"
cp "$CMAKE_FILE" "$CMAKE_BACKUP"

echo ""
echo "🔨 Updating CMakeLists.txt..."
echo ""

# Use sed to replace all Homebrew references
# This works on both macOS and Linux

# Replace /opt/homebrew/lib/libsecp256k1.dylib with vendored static library
sed -i.tmp 's|/opt/homebrew/lib/libsecp256k1\.dylib|${CMAKE_SOURCE_DIR}/vendor/lib/libsecp256k1.a|g' "$CMAKE_FILE"

# Replace standalone secp256k1 references (for Linux builds)
# We need to be more careful here - only replace in link_libraries contexts
sed -i.tmp '/target_link_libraries/s/\bsecp256k1\b/${CMAKE_SOURCE_DIR}\/vendor\/lib\/libsecp256k1.a/g' "$CMAKE_FILE"

# Clean up temporary file
rm -f "$CMAKE_FILE.tmp"

# Add vendor include directory near the top of CMakeLists.txt
# Find the line with "Include directories" and add vendor after it
awk '
/^# Include directories/ {
    print
    print "include_directories(${CMAKE_SOURCE_DIR}/vendor/include)"
    next
}
{print}
' "$CMAKE_BACKUP" > "$CMAKE_FILE.tmp" && mv "$CMAKE_FILE.tmp" "$CMAKE_FILE"

echo "✅ CMakeLists.txt updated!"
echo ""
echo "📊 Changes made:"
echo "   • Replaced /opt/homebrew/lib/libsecp256k1.dylib → vendor/lib/libsecp256k1.a"
echo "   • Replaced secp256k1 link references → vendor/lib/libsecp256k1.a"
echo "   • Added vendor/include to include directories"
echo ""
echo "🔍 Summary:"
grep -n "vendor/lib/libsecp256k1.a" "$CMAKE_FILE" | wc -l | xargs echo "   •" "vendored references:"
echo ""
echo "📝 Backup saved to: CMakeLists.txt.before-vendored"
echo ""
echo "🎯 Next step: Test the build"
echo "   mkdir -p build-vendored && cd build-vendored"
echo "   cmake .. && make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
echo ""
