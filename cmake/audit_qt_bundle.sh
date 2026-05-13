#!/bin/bash
# audit_qt_bundle.sh - Check Qt app bundle for Homebrew dependencies

APP_BUNDLE="$1"
APP_NAME=$(basename "$APP_BUNDLE" .app)

echo "Auditing Qt app bundle: $APP_NAME"

if [[ ! -d "$APP_BUNDLE" ]]; then
    echo "ERROR: App bundle not found: $APP_BUNDLE"
    exit 1
fi

# Check main executable
MAIN_EXEC="$APP_BUNDLE/Contents/MacOS/$APP_NAME"
if [[ -f "$MAIN_EXEC" ]]; then
    echo "Checking main executable: $(basename "$MAIN_EXEC")"
    otool -L "$MAIN_EXEC"
    
    if otool -L "$MAIN_EXEC" | grep -q /opt/homebrew; then
        echo "ERROR: Main executable has Homebrew dependencies!"
        otool -L "$MAIN_EXEC" | grep homebrew
        exit 1
    fi
else
    echo "WARNING: Main executable not found at expected location"
fi

# Check all frameworks and dylibs in the bundle
echo "Checking bundled frameworks and libraries..."

# Use a temporary file to track errors across subshells
TEMP_ERROR_FILE=$(mktemp)
echo "0" > "$TEMP_ERROR_FILE"

find "$APP_BUNDLE/Contents/Frameworks" -name "*.dylib" -o -name "Qt*" 2>/dev/null | while read -r LIB; do
    if [[ -f "$LIB" ]]; then
        echo "  Checking: $(basename "$LIB")"
        if otool -L "$LIB" 2>/dev/null | grep -q /opt/homebrew; then
            # Check if this is an acceptable bundled dependency
            LIB_NAME=$(basename "$LIB")
            if [[ "$LIB_NAME" == "libdbus-1.3.dylib" ]]; then
                echo "  ⚠️  $(basename "$LIB") has Homebrew references (acceptable - bundled dependency)"
            else
                echo "  ❌ External reference in $(basename "$LIB"):"
                otool -L "$LIB" | grep homebrew | sed 's/^/    /'
                echo "1" > "$TEMP_ERROR_FILE"
            fi
        fi
    fi
done

HOMEBREW_FOUND=$(cat "$TEMP_ERROR_FILE")
rm "$TEMP_ERROR_FILE"

if [[ $HOMEBREW_FOUND -eq 1 ]]; then
    echo "❌ Audit failed: external dependencies detected in $APP_BUNDLE"
    exit 1
else
    echo "✅ SUCCESS: $APP_NAME.app is Homebrew-free"
fi
