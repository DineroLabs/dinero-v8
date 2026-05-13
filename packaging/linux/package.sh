#!/bin/bash

# Linux Packaging Script for Dinero Desktop
# Creates AppImage and Flatpak packages

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
PACKAGE_DIR="$PROJECT_ROOT/packaging/linux"
OUTPUT_DIR="$PACKAGE_DIR/output"

# Configuration
APP_NAME="Dinero Desktop"
APP_ID="com.dinero.desktop"
APP_VERSION="2.1.2"
EXECUTABLE="dinero-desktop"

echo "🐧 Linux Packaging for Dinero Desktop"
echo "====================================="
echo ""

# Clean and create output directory
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

# Check if executable exists (look for the actual binary in the app bundle)
EXECUTABLE_PATH=""
if [ -f "$BUILD_DIR/src/gui-desktop/dinero-desktop.app/Contents/MacOS/dinero-desktop" ]; then
    EXECUTABLE_PATH="$BUILD_DIR/src/gui-desktop/dinero-desktop.app/Contents/MacOS/dinero-desktop"
elif [ -f "$BUILD_DIR/src/gui-desktop/$EXECUTABLE" ]; then
    EXECUTABLE_PATH="$BUILD_DIR/src/gui-desktop/$EXECUTABLE"
elif [ -f "$BUILD_DIR/$EXECUTABLE" ]; then
    EXECUTABLE_PATH="$BUILD_DIR/$EXECUTABLE"
fi

if [ -z "$EXECUTABLE_PATH" ]; then
    echo "❌ Executable not found. Checked:"
    echo "  - $BUILD_DIR/src/gui-desktop/dinero-desktop.app/Contents/MacOS/dinero-desktop"
    echo "  - $BUILD_DIR/src/gui-desktop/$EXECUTABLE"
    echo "  - $BUILD_DIR/$EXECUTABLE"
    echo "Please build the application first"
    exit 1
fi

echo "✅ Found executable: $EXECUTABLE_PATH"

# Create AppDir structure
echo "📦 Step 1: Creating AppDir structure..."
APPDIR="$OUTPUT_DIR/DineroDesktop.AppDir"
mkdir -p "$APPDIR/usr/bin"
mkdir -p "$APPDIR/usr/lib"
mkdir -p "$APPDIR/usr/share/applications"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"

# Copy executable
cp "$EXECUTABLE_PATH" "$APPDIR/usr/bin/$EXECUTABLE"
chmod +x "$APPDIR/usr/bin/$EXECUTABLE"

# Copy Qt libraries (if available)
echo "🔧 Step 2: Bundling Qt libraries..."
if command -v ldd >/dev/null 2>&1; then
    # Get Qt library dependencies
    QT_LIBS=$(ldd "$EXECUTABLE_PATH" 2>/dev/null | grep -E "Qt[56]" | awk '{print $3}' | sort -u)
    for lib in $QT_LIBS; do
        if [ -f "$lib" ]; then
            cp "$lib" "$APPDIR/usr/lib/"
            echo "  Bundled: $(basename "$lib")"
        fi
    done
else
    echo "⚠️ ldd not available. Qt libraries may not be bundled."
fi

# Create desktop file
echo "🖥️ Step 3: Creating desktop file..."
cat > "$APPDIR/usr/share/applications/$APP_ID.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$APP_NAME
Comment=Professional DineroCoin cryptocurrency wallet
Exec=$EXECUTABLE
Icon=$APP_ID
Categories=Office;Finance;
Keywords=cryptocurrency;bitcoin;wallet;blockchain;dinero;
StartupNotify=true
EOF

# Create AppRun script
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:$LD_LIBRARY_PATH"
export QT_PLUGIN_PATH="$HERE/usr/plugins:$QT_PLUGIN_PATH"
exec "$HERE/usr/bin/dinero-desktop" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# Create icon (placeholder - would use actual icon)
echo "🎨 Step 4: Creating application icon..."
cat > "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.svg" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<svg width="256" height="256" viewBox="0 0 256 256" xmlns="http://www.w3.org/2000/svg">
  <rect width="256" height="256" rx="32" fill="#ff6b35"/>
  <text x="128" y="140" font-family="Arial, sans-serif" font-size="120" font-weight="bold" text-anchor="middle" fill="white">💎</text>
  <text x="128" y="200" font-family="Arial, sans-serif" font-size="24" font-weight="bold" text-anchor="middle" fill="white">DINERO</text>
</svg>
EOF

# Copy icon to root level for AppImage
cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/$APP_ID.svg" "$APPDIR/$APP_ID.svg"
cp "$APPDIR/usr/share/applications/$APP_ID.desktop" "$APPDIR/$APP_ID.desktop"

# Create AppImage (if appimagetool available)
echo "📦 Step 5: Creating AppImage..."
if command -v appimagetool >/dev/null 2>&1; then
    cd "$OUTPUT_DIR"
    appimagetool "DineroDesktop.AppDir" "DineroDesktop-$APP_VERSION-x86_64.AppImage"
    chmod +x "DineroDesktop-$APP_VERSION-x86_64.AppImage"
    echo "✅ AppImage created: DineroDesktop-$APP_VERSION-x86_64.AppImage"
else
    echo "⚠️ appimagetool not found. Install AppImageKit to create AppImage."
    echo "AppDir created at: $APPDIR"
    echo "To create AppImage manually:"
    echo "  1. Download appimagetool from https://github.com/AppImage/AppImageKit/releases"
    echo "  2. Run: appimagetool DineroDesktop.AppDir"
fi

# Create Flatpak manifest
echo "📋 Step 6: Creating Flatpak manifest..."
cat > "$PACKAGE_DIR/$APP_ID.yml" <<EOF
app-id: $APP_ID
runtime: org.freedesktop.Platform
runtime-version: '22.08'
sdk: org.freedesktop.Sdk
command: $EXECUTABLE

finish-args:
  - --share=network
  - --socket=x11
  - --socket=wayland
  - --device=dri
  - --filesystem=home
  - --filesystem=xdg-documents
  - --filesystem=xdg-download

modules:
  - name: dinero-desktop
    buildsystem: simple
    build-commands:
      - install -Dm755 $EXECUTABLE /app/bin/$EXECUTABLE
      - install -Dm644 $APP_ID.desktop /app/share/applications/$APP_ID.desktop
      - install -Dm644 $APP_ID.svg /app/share/icons/hicolor/scalable/apps/$APP_ID.svg
    sources:
      - type: file
        path: ../build/$EXECUTABLE
      - type: file
        path: $APP_ID.desktop
      - type: file
        path: $APP_ID.svg
EOF

echo ""
echo "✅ Linux packaging complete!"
echo "📁 Outputs:"
if [ -f "$OUTPUT_DIR/DineroDesktop-$APP_VERSION-x86_64.AppImage" ]; then
    echo "  - AppImage: $OUTPUT_DIR/DineroDesktop-$APP_VERSION-x86_64.AppImage"
fi
echo "  - AppDir: $OUTPUT_DIR/DineroDesktop.AppDir"
echo "  - Flatpak manifest: $PACKAGE_DIR/$APP_ID.yml"
echo ""
echo "📋 To build Flatpak:"
echo "  flatpak-builder build-dir $PACKAGE_DIR/$APP_ID.yml --force-clean"
echo "  flatpak build-export repo build-dir"
echo ""
echo "🚀 Ready for distribution!"
