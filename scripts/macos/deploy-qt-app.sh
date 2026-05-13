#!/usr/bin/env bash
set -euo pipefail

APP_PATH="${1:-}"
if [[ -z "${APP_PATH}" || ! -d "${APP_PATH}" ]]; then
  echo "Usage: $0 /absolute/path/to/App.app" >&2
  exit 2
fi

# Try to find macdeployqt (prefer official Qt installations)
candidates=()
[[ -n "${QT_PREFIX_PATH:-}" ]] && candidates+=("${QT_PREFIX_PATH}/bin/macdeployqt")
[[ -n "${QT_DIR:-}"        ]] && candidates+=("${QT_DIR}/bin/macdeployqt")

# Official Qt installation paths (prefer newer versions first)
candidates+=(
  "/Users/haydarevich/Qt/qt1/6.9.1/macos/bin/macdeployqt"
  "/Users/haydarevich/Qt/6.9.1/macos/bin/macdeployqt"
  "/Applications/Qt/6.8.0/macos/bin/macdeployqt"
  "/Applications/Qt/6.7.2/macos/bin/macdeployqt"
  "/Applications/Qt/6.6.3/macos/bin/macdeployqt"
  "/Applications/Qt/6.5.3/macos/bin/macdeployqt"
)

# Last resort: system-installed macdeployqt (could be Homebrew)
candidates+=("$(command -v macdeployqt || true)")

macdeploy=""
for c in "${candidates[@]}"; do
  [[ -x "$c" ]] && macdeploy="$c" && break
done

if [[ -z "$macdeploy" ]]; then
  echo "macdeployqt not found. Set CMAKE_PREFIX_PATH to your Qt (e.g. /Applications/Qt/6.7.2/macos)" >&2
  exit 3
fi

echo "Using macdeployqt: $macdeploy"
"$macdeploy" "$APP_PATH" -always-overwrite -verbose=2
echo "✅ Bundled Qt frameworks into: $APP_PATH"

# Fix missing QtDBus framework (common macdeployqt issue)
echo "🔧 Checking for missing Qt frameworks..."
if [[ ! -d "$APP_PATH/Contents/Frameworks/QtDBus.framework" ]]; then
  echo "📦 Adding missing QtDBus framework..."
  if [[ -d "/opt/homebrew/opt/qt/lib/QtDBus.framework" ]]; then
    cp -R "/opt/homebrew/opt/qt/lib/QtDBus.framework" "$APP_PATH/Contents/Frameworks/"
    
    # Copy libdbus dependency
    if [[ -f "/opt/homebrew/lib/libdbus-1.3.dylib" ]]; then
      cp "/opt/homebrew/lib/libdbus-1.3.dylib" "$APP_PATH/Contents/Frameworks/"
      
      # Fix internal path in QtDBus
      install_name_tool -change /opt/homebrew/lib/libdbus-1.3.dylib \
        @executable_path/../Frameworks/libdbus-1.3.dylib \
        "$APP_PATH/Contents/Frameworks/QtDBus.framework/Versions/A/QtDBus" 2>/dev/null || true
      
      # Fix internal paths in libdbus itself
      install_name_tool -id @executable_path/../Frameworks/libdbus-1.3.dylib \
        "$APP_PATH/Contents/Frameworks/libdbus-1.3.dylib" 2>/dev/null || true
      
      # Fix any Homebrew references in libdbus
      install_name_tool -change /opt/homebrew/opt/dbus/lib/libdbus-1.3.dylib \
        @executable_path/../Frameworks/libdbus-1.3.dylib \
        "$APP_PATH/Contents/Frameworks/libdbus-1.3.dylib" 2>/dev/null || true
      
      echo "✅ QtDBus framework added with dependencies"
    fi
  fi
fi

# Bundle OpenSSL 3 provider modules for crypto operations
echo "🔐 Bundling OpenSSL 3 providers..."
OSSL_MODULES_DIR="$APP_PATH/Contents/Resources/ossl-modules"
mkdir -p "$OSSL_MODULES_DIR"

# Find OpenSSL provider modules
OPENSSL_PROVIDERS=""
for provider_path in \
  "/opt/homebrew/Cellar/openssl@3/*/lib/ossl-modules" \
  "/usr/local/Cellar/openssl@3/*/lib/ossl-modules" \
  "/opt/homebrew/lib/ossl-modules" \
  "/usr/local/lib/ossl-modules"; do
  if [[ -d "$provider_path" ]]; then
    OPENSSL_PROVIDERS="$provider_path"
    break
  fi
done

if [[ -n "$OPENSSL_PROVIDERS" && -d "$OPENSSL_PROVIDERS" ]]; then
  echo "📦 Copying OpenSSL providers from: $OPENSSL_PROVIDERS"
  cp -R "$OPENSSL_PROVIDERS"/* "$OSSL_MODULES_DIR/" 2>/dev/null || true
  
  # Count providers
  PROVIDER_COUNT=$(ls -1 "$OSSL_MODULES_DIR"/*.dylib 2>/dev/null | wc -l | tr -d ' ')
  if [[ "$PROVIDER_COUNT" -gt 0 ]]; then
    echo "✅ Bundled $PROVIDER_COUNT OpenSSL provider(s)"
  else
    echo "⚠️  No OpenSSL providers found to bundle"
  fi
else
  echo "⚠️  OpenSSL provider modules not found - crypto operations may fail"
fi

# Create Info.plist entry for OpenSSL modules path
echo "🔧 Setting OpenSSL modules environment in app..."
PLIST_PATH="$APP_PATH/Contents/Info.plist"
if [[ -f "$PLIST_PATH" ]]; then
  # Add LSEnvironment dictionary if it doesn't exist
  if ! /usr/libexec/PlistBuddy -c "Print :LSEnvironment" "$PLIST_PATH" >/dev/null 2>&1; then
    /usr/libexec/PlistBuddy -c "Add :LSEnvironment dict" "$PLIST_PATH" 2>/dev/null || true
  fi
  
  # Set OpenSSL modules path and Qt font fix
  /usr/libexec/PlistBuddy -c "Set :LSEnvironment:OPENSSL_MODULES \${EXECUTABLE_DIR}/../Resources/ossl-modules" "$PLIST_PATH" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :LSEnvironment:OPENSSL_MODULES string \${EXECUTABLE_DIR}/../Resources/ossl-modules" "$PLIST_PATH" 2>/dev/null || true
  
  /usr/libexec/PlistBuddy -c "Set :LSEnvironment:QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM 1" "$PLIST_PATH" 2>/dev/null || \
  /usr/libexec/PlistBuddy -c "Add :LSEnvironment:QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM string 1" "$PLIST_PATH" 2>/dev/null || true
  
  echo "✅ Environment variables set in Info.plist"
fi

# Code sign the app bundle after Qt deployment
echo "🔐 Code signing app bundle..."
if codesign --force --deep --sign - "$APP_PATH" 2>/dev/null; then
  echo "✅ Code signed: $APP_PATH"
else
  echo "⚠️  Code signing failed (non-fatal): $APP_PATH" >&2
fi