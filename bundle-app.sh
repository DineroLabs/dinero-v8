#!/bin/bash
# Dinero App Bundler - Creates a complete .app bundle with daemon + miner
# Usage: ./bundle-app.sh

set -e

echo "🚀 Dinero App Bundler"
echo "===================="
echo ""

# Configuration
PROJECT_ROOT="$(pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
GUI_BUILD_DIR="$PROJECT_ROOT/build-gui"
APP_BUNDLE="$GUI_BUILD_DIR/dinero-qt.app"
RESOURCES_DIR="$APP_BUNDLE/Contents/Resources"
MACOS_DIR="$APP_BUNDLE/Contents/MacOS"

# Check if GUI is built
if [ ! -f "$GUI_BUILD_DIR/dinero-qt" ]; then
    echo "❌ GUI not built. Please build first:"
    echo "   cd build-gui && cmake .. && make -j8"
    exit 1
fi

# Check if daemon is built
if [ ! -f "$BUILD_DIR/dinerod" ]; then
    echo "❌ Daemon not built. Please build first:"
    echo "   cd build && cmake .. && make -j8 dinerod"
    exit 1
fi

# Check if miner is built
MINER_PATH=""
if [ -f "$BUILD_DIR/dinero-miner" ]; then
    MINER_PATH="$BUILD_DIR/dinero-miner"
elif [ -f "build-qt/dinero-miner" ]; then
    MINER_PATH="build-qt/dinero-miner"
elif [ -f "build-clean/dinero-miner" ]; then
    MINER_PATH="build-clean/dinero-miner"
else
    echo "⚠️  Miner not found, will build it..."
    (cd build && make dinero-miner || echo "Miner build failed, continuing without it")
    if [ -f "$BUILD_DIR/dinero-miner" ]; then
        MINER_PATH="$BUILD_DIR/dinero-miner"
    fi
fi

echo "✅ Found all required binaries"
echo ""

# Create proper app bundle structure
echo "📦 Creating app bundle structure..."
mkdir -p "$RESOURCES_DIR"
mkdir -p "$MACOS_DIR"

# Copy GUI executable to MacOS directory
if [ -f "$GUI_BUILD_DIR/dinero-qt" ]; then
    cp "$GUI_BUILD_DIR/dinero-qt" "$MACOS_DIR/dinero-qt"
    echo "  ✓ Copied GUI to $MACOS_DIR/dinero-qt"
fi

# Bundle daemon into Resources
cp "$BUILD_DIR/dinerod" "$RESOURCES_DIR/dinerod"
chmod +x "$RESOURCES_DIR/dinerod"
echo "  ✓ Bundled daemon: $RESOURCES_DIR/dinerod"

# Bundle miner if available
if [ -n "$MINER_PATH" ] && [ -f "$MINER_PATH" ]; then
    cp "$MINER_PATH" "$RESOURCES_DIR/dinero-miner"
    chmod +x "$RESOURCES_DIR/dinero-miner"
    echo "  ✓ Bundled miner: $RESOURCES_DIR/dinero-miner"
else
    echo "  ⚠️  Miner not bundled (not found)"
fi

# Create Info.plist if it doesn't exist
if [ ! -f "$APP_BUNDLE/Contents/Info.plist" ]; then
    echo "📝 Creating Info.plist..."
    cat > "$APP_BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>dinero-qt</string>
    <key>CFBundleIdentifier</key>
    <string>com.dinero.desktop</string>
    <key>CFBundleName</key>
    <string>Dinero</string>
    <key>CFBundleDisplayName</key>
    <string>Dinero Wallet</string>
    <key>CFBundleVersion</key>
    <string>1.0.0</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0.0</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleSignature</key>
    <string>DINO</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>LSMinimumSystemVersion</key>
    <string>10.13</string>
</dict>
</plist>
EOF
    echo "  ✓ Created Info.plist"
fi

# Run macdeployqt if available (bundles Qt frameworks)
if command -v macdeployqt >/dev/null 2>&1; then
    echo ""
    echo "🔧 Running macdeployqt to bundle Qt frameworks..."
    macdeployqt "$APP_BUNDLE" -verbose=1 || echo "⚠️  macdeployqt had warnings (app may still work)"
else
    echo ""
    echo "⚠️  macdeployqt not found - Qt frameworks not bundled"
    echo "   Install Qt development tools for full deployment"
fi

echo ""
echo "✅ App bundle complete!"

# Re-sign the entire bundle (critical for macOS to prevent crashes)
echo ""
echo "🔏 Re-signing app bundle..."
if codesign --force --deep --sign - "$APP_BUNDLE" 2>&1 | grep -q "replacing existing signature"; then
    echo "  ✓ App bundle re-signed successfully"
else
    echo "  ⚠️  Code signing failed (app may not run on macOS)"
fi

echo ""
echo "📁 Bundle location: $APP_BUNDLE"
echo ""
echo "Contents:"
ls -lh "$MACOS_DIR"
echo ""
ls -lh "$RESOURCES_DIR" | grep -E "(dinerod|dinero-miner)"
echo ""
echo "🚀 To run: open $APP_BUNDLE"
echo "   or: $APP_BUNDLE/Contents/MacOS/dinero-qt"
echo ""
