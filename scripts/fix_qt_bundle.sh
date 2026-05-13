#!/usr/bin/env bash
set -euo pipefail

echo "🔧 **COMPREHENSIVE QT BUNDLE FIX**"
echo "================================="

cd /Users/haydarevich/Documents/DineroCoin
APP_PATH="build/src/gui-desktop/dinero-desktop.app"
FRAMEWORKS_PATH="$APP_PATH/Contents/Frameworks"

echo "Step 1: Find all Qt dependencies..."
QT_DEPS=$(otool -L "$APP_PATH/Contents/MacOS/dinero-desktop" | grep '@rpath.*Qt' | awk '{print $1}' | sed 's/@rpath\///g' | sed 's/\.framework.*/.framework/g' | sort | uniq)

echo "Required Qt frameworks:"
echo "$QT_DEPS"

echo ""
echo "Step 2: Copy all required Qt frameworks..."
for framework in $QT_DEPS; do
    if [ -d "/opt/homebrew/lib/$framework" ]; then
        echo "Copying $framework..."
        ditto "/opt/homebrew/lib/$framework" "$FRAMEWORKS_PATH/$framework"
    else
        echo "⚠️  Framework not found: /opt/homebrew/lib/$framework"
    fi
done

echo ""
echo "Step 3: Update library paths in main binary..."
BINARY="$APP_PATH/Contents/MacOS/dinero-desktop"
for framework in $QT_DEPS; do
    framework_name=$(echo "$framework" | sed 's/.framework//g')
    old_path="@rpath/$framework/Versions/A/$framework_name"
    new_path="@executable_path/../Frameworks/$framework/Versions/A/$framework_name"
    echo "Updating path for $framework_name..."
    install_name_tool -change "$old_path" "$new_path" "$BINARY" 2>/dev/null || echo "  (path not found - skipping)"
done

echo ""
echo "Step 4: Fix inter-framework dependencies..."
for framework in $QT_DEPS; do
    framework_path="$FRAMEWORKS_PATH/$framework/Versions/A"
    framework_name=$(echo "$framework" | sed 's/.framework//g')
    framework_binary="$framework_path/$framework_name"
    
    if [ -f "$framework_binary" ]; then
        echo "Fixing dependencies in $framework_name..."
        # Get Qt dependencies of this framework
        deps=$(otool -L "$framework_binary" | grep '@rpath.*Qt' | awk '{print $1}' | sed 's/@rpath\///g' | sed 's/\.framework.*/.framework/g' | sort | uniq)
        for dep in $deps; do
            dep_name=$(echo "$dep" | sed 's/.framework//g')
            old_dep_path="@rpath/$dep/Versions/A/$dep_name"
            new_dep_path="@executable_path/../Frameworks/$dep/Versions/A/$dep_name"
            install_name_tool -change "$old_dep_path" "$new_dep_path" "$framework_binary" 2>/dev/null || true
        done
    fi
done

echo ""
echo "Step 5: Re-sign the entire app bundle..."
codesign --force --deep --sign - "$APP_PATH"

echo ""
echo "Step 6: Verify signature..."
codesign --verify --verbose "$APP_PATH"

echo ""
echo "Step 7: Test the app..."
if timeout 3 "$APP_PATH/Contents/MacOS/dinero-desktop" --version >/dev/null 2>&1; then
    echo "✅ App launches successfully!"
else
    echo "Testing app launch..."
    timeout 3 "$APP_PATH/Contents/MacOS/dinero-desktop" --version 2>&1 | head -5
fi

echo ""
echo "🎉 **QT BUNDLE FIX COMPLETE**"
