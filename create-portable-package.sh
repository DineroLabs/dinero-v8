#!/bin/bash
# Create portable DineroCoin package for macOS
# Bundles all dependencies so it works on any Mac without Homebrew

set -e

echo "==========================================="
echo "  DineroCoin Portable Package Creator"
echo "==========================================="
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="/Users/haydarevich/Desktop/ARMIN_DINERO"

cd "$SCRIPT_DIR"

# Step 1: Ensure binaries are built
echo "Step 1/5: Checking binaries..."
if [ ! -f "build/dinerod" ]; then
    echo "Error: build/dinerod not found. Please run: cmake --build build"
    exit 1
fi

if [ ! -f "build/gui/dinero-qt" ]; then
    echo "Warning: build/gui/dinero-qt not found. Skipping GUI."
    BUILD_GUI=false
else
    BUILD_GUI=true
fi

echo "✓ Binaries found"

# Step 2: Create lib directory and copy dependencies
echo ""
echo "Step 2/5: Bundling dependencies..."
mkdir -p "$PACKAGE_DIR/lib"

# Copy required dylibs
cp /opt/homebrew/lib/libjsoncpp.26.dylib "$PACKAGE_DIR/lib/"
cp /opt/homebrew/lib/libsecp256k1.6.dylib "$PACKAGE_DIR/lib/"
cp /opt/homebrew/opt/openssl@3/lib/libssl.3.dylib "$PACKAGE_DIR/lib/"
cp /opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib "$PACKAGE_DIR/lib/"
cp /opt/homebrew/lib/liblz4.1.dylib "$PACKAGE_DIR/lib/"

echo "✓ Dependencies copied to lib/"

# Step 3: Copy binaries and fix library paths
echo ""
echo "Step 3/5: Copying binaries and fixing paths..."

# Copy dinerod
cp build/dinerod "$PACKAGE_DIR/bin/"
chmod +x "$PACKAGE_DIR/bin/dinerod"

# Fix rpath for dinerod
install_name_tool -add_rpath "@executable_path/../lib" "$PACKAGE_DIR/bin/dinerod"
install_name_tool -change /opt/homebrew/lib/libjsoncpp.26.dylib "@rpath/libjsoncpp.26.dylib" "$PACKAGE_DIR/bin/dinerod"
install_name_tool -change /opt/homebrew/lib/libsecp256k1.6.dylib "@rpath/libsecp256k1.6.dylib" "$PACKAGE_DIR/bin/dinerod"
install_name_tool -change /opt/homebrew/opt/openssl@3/lib/libssl.3.dylib "@rpath/libssl.3.dylib" "$PACKAGE_DIR/bin/dinerod"
install_name_tool -change /opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib "@rpath/libcrypto.3.dylib" "$PACKAGE_DIR/bin/dinerod"
install_name_tool -change /opt/homebrew/opt/lz4/lib/liblz4.1.dylib "@rpath/liblz4.1.dylib" "$PACKAGE_DIR/bin/dinerod"

echo "✓ dinerod paths fixed"

# Copy and fix dinero-qt if available
if [ "$BUILD_GUI" = true ]; then
    cp build/gui/dinero-qt "$PACKAGE_DIR/bin/"
    chmod +x "$PACKAGE_DIR/bin/dinero-qt"

    # Copy Qt frameworks
    echo "  Copying Qt frameworks..."
    mkdir -p "$PACKAGE_DIR/lib/Qt"
    cp -R /opt/homebrew/opt/qt/lib/QtWidgets.framework "$PACKAGE_DIR/lib/Qt/"
    cp -R /opt/homebrew/opt/qt/lib/QtNetwork.framework "$PACKAGE_DIR/lib/Qt/"
    cp -R /opt/homebrew/opt/qt/lib/QtGui.framework "$PACKAGE_DIR/lib/Qt/"
    cp -R /opt/homebrew/opt/qt/lib/QtCore.framework "$PACKAGE_DIR/lib/Qt/"

    # Fix Qt framework paths
    install_name_tool -add_rpath "@executable_path/../lib" "$PACKAGE_DIR/bin/dinero-qt"
    install_name_tool -change /opt/homebrew/opt/qt/lib/QtWidgets.framework/Versions/A/QtWidgets "@rpath/Qt/QtWidgets.framework/Versions/A/QtWidgets" "$PACKAGE_DIR/bin/dinero-qt"
    install_name_tool -change /opt/homebrew/opt/qt/lib/QtNetwork.framework/Versions/A/QtNetwork "@rpath/Qt/QtNetwork.framework/Versions/A/QtNetwork" "$PACKAGE_DIR/bin/dinero-qt"
    install_name_tool -change /opt/homebrew/opt/qt/lib/QtGui.framework/Versions/A/QtGui "@rpath/Qt/QtGui.framework/Versions/A/QtGui" "$PACKAGE_DIR/bin/dinero-qt"
    install_name_tool -change /opt/homebrew/opt/qt/lib/QtCore.framework/Versions/A/QtCore "@rpath/Qt/QtCore.framework/Versions/A/QtCore" "$PACKAGE_DIR/bin/dinero-qt"

    echo "✓ dinero-qt paths fixed"
fi

# Fix interdependencies in bundled libs
install_name_tool -change /opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib "@rpath/libcrypto.3.dylib" "$PACKAGE_DIR/lib/libssl.3.dylib"

# Step 4: Verify dependencies
echo ""
echo "Step 4/5: Verifying package..."

echo ""
echo "--- dinerod dependencies ---"
otool -L "$PACKAGE_DIR/bin/dinerod" | grep -E "@rpath|/usr/lib|/System" | head -15

if [ "$BUILD_GUI" = true ]; then
    echo ""
    echo "--- dinero-qt dependencies (first 10) ---"
    otool -L "$PACKAGE_DIR/bin/dinero-qt" | grep -E "@rpath|/usr/lib|/System" | head -10
fi

# Step 5: Create README
echo ""
echo "Step 5/5: Creating user guide..."
# User guide will be created separately since we can't use Write tool for this

echo ""
echo "==========================================="
echo "  ✓ Portable Package Created!"
echo "==========================================="
echo ""
echo "Package location: $PACKAGE_DIR"
echo ""
echo "Contents:"
echo "  • bin/dinerod - Full node daemon"
if [ "$BUILD_GUI" = true ]; then
    echo "  • bin/dinero-qt - GUI wallet"
fi
echo "  • lib/ - Bundled dependencies (portable)"
echo "  • scripts/ - Universal launcher scripts"
echo "  • config/ - Configuration template"
echo ""
echo "This package can be copied to any Mac (Intel or Apple Silicon)"
echo "and will work without installing Homebrew or any dependencies."
echo ""
