#!/usr/bin/env bash
set -euo pipefail

APP="build/src/gui-desktop/dinero-desktop.app"

echo "🔧 Fixing macOS Code Signing for Dinero Desktop..."

# Check if app exists
if [[ ! -d "$APP" ]]; then
    echo "❌ App not found at: $APP"
    echo "Run: cmake --build build --target dinero-desktop"
    exit 1
fi

echo "📱 Found app bundle: $APP"

# 0) Optional: Redeploy Qt bits if needed (uncomment if issues persist)
# echo "🔄 Re-deploying Qt dependencies..."
# /usr/local/opt/qt/bin/macdeployqt "$APP" -always-overwrite -verbose=2

# 1) Remove quarantine flags (can kill at launch)
echo "🧹 Removing quarantine attributes..."
xattr -dr com.apple.quarantine "$APP" || true

# 2) Ensure qt.conf is correct
echo "⚙️  Ensuring qt.conf is properly configured..."
printf "[Paths]\nPlugins = PlugIns\n" > "$APP/Contents/Resources/qt.conf"

# 3) Ad-hoc re-sign EVERYTHING after deployment (dev use; no Hardened Runtime)
echo "✍️  Ad-hoc signing the entire app bundle..."
codesign --force --deep --timestamp=none --sign - "$APP"

# 4) Verify signatures (find any remaining issues)
echo "🔍 Verifying code signatures..."
if codesign --verify --deep --strict --verbose=2 "$APP"; then
    echo "✅ Code signature verification passed!"
else
    echo "❌ Code signature verification failed!"
    echo "🔍 Checking individual components..."
    
    # Brute-check all executables and libraries
    while IFS= read -r -d '' f; do
        if ! codesign -vvv --verify --strict "$f" 2>/dev/null; then
            echo "🔴 BAD: $f"
        else
            echo "✅ OK: $(basename "$f")"
        fi
    done < <(find "$APP/Contents" -type f \( -perm +111 -o -name "*.dylib" -o -path "*/Frameworks/*/Versions/*/*" \) -print0)
    
    exit 1
fi

# 5) Gatekeeper assessment (should pass for local ad-hoc)
echo "🛡️  Testing Gatekeeper assessment..."
if spctl --assess --verbose=4 "$APP" 2>&1; then
    echo "✅ Gatekeeper assessment passed!"
else
    echo "⚠️  Gatekeeper assessment failed (expected for ad-hoc signatures)"
fi

echo ""
echo "🎉 Code signing fix complete!"
echo "✅ App should now launch without being killed by macOS"
echo ""
echo "🚀 Ready to launch: $APP"
