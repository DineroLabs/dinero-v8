#!/usr/bin/env bash
# Regression gate: Developer-ID finalization must recursively replace the
# signatures of nested Tor Expert Bundle Mach-O files, not merely seal the app.
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "SKIP: macOS codesign regression test requires Darwin"
    exit 0
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TMP_ROOT"' EXIT

APP="$TMP_ROOT/dinero-qt.app"
MACOS="$APP/Contents/MacOS"
TOR="$APP/Contents/Resources/tor/tor"
mkdir -p "$MACOS" "$TOR/pluggable_transports"

cp /usr/bin/true "$MACOS/dinero-qt"
cp /usr/bin/true "$TOR/tor"
cp /usr/bin/true "$TOR/libevent-2.1.7.dylib"
cp /usr/bin/true "$TOR/pluggable_transports/lyrebird"
cp /usr/bin/true "$TOR/pluggable_transports/conjure-client"
chmod +x "$MACOS/dinero-qt" "$TOR/tor" \
    "$TOR/pluggable_transports/lyrebird" \
    "$TOR/pluggable_transports/conjure-client"

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>dinero-qt</string>
  <key>CFBundleIdentifier</key><string>org.dinerolabs.signing-regression</string>
  <key>CFBundlePackageType</key><string>APPL</string>
</dict></plist>
PLIST

"$ROOT_DIR/qt/scripts/sign-release.sh" "$APP" -
codesign --verify --deep --strict "$APP"

for nested in \
    "$TOR/tor" \
    "$TOR/libevent-2.1.7.dylib" \
    "$TOR/pluggable_transports/lyrebird" \
    "$TOR/pluggable_transports/conjure-client"; do
    codesign --verify --strict "$nested"
    details="$(codesign -dv --verbose=4 "$nested" 2>&1)"
    grep -Eq 'flags=0x[0-9a-fA-F]+\([^)]*runtime' <<<"$details" || {
        echo "FAIL: nested Mach-O lacks hardened runtime: $nested" >&2
        echo "$details" >&2
        exit 1
    }
done

echo "PASS: every nested Tor-like Mach-O has its own hardened-runtime signature"
