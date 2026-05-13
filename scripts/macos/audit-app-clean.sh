#!/usr/bin/env bash
set -euo pipefail
APP_PATH="${1:-}"
if [[ -z "$APP_PATH" || ! -d "$APP_PATH" ]]; then
  echo "Usage: $0 /path/to/App.app" >&2
  exit 2
fi

# Anything outside these roots is suspicious
allow_re='^(@|/System/|/usr/lib/)'
deny_re='^/opt/homebrew|^/usr/local/(Cellar|opt|lib)/qt|/local/Cellar/qt|/usr/local/lib/Qt'

bad=0

scan_file() {
  local f="$1"
  while IFS= read -r line; do
    lib=$(awk '{print $1}' <<<"$line")
    [[ "$lib" =~ $allow_re ]] && continue
    [[ "$lib" == "$f" ]] && continue
    if [[ "$lib" =~ $deny_re ]]; then
      echo "❌ External reference in $(basename "$f"): $lib"
      bad=1
    fi
  done < <(otool -L "$f" 2>/dev/null | tail -n +2)
}

# Main executable
exe="$APP_PATH/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP_PATH/Contents/Info.plist")"
scan_file "$exe"

# Framework dylibs
while IFS= read -r f; do
  scan_file "$f"
done < <(find "$APP_PATH/Contents/Frameworks" -type f \( -name "*.dylib" -o -name "*.so" -o -name "Qt*.framework/*/*" \) 2>/dev/null || true)

# Plugins
while IFS= read -r f; do
  scan_file "$f"
done < <(find "$APP_PATH/Contents/PlugIns" -type f \( -name "*.dylib" -o -name "*.so" \) 2>/dev/null || true)

if [[ $bad -ne 0 ]]; then
  echo "❌ Audit failed: external dependencies detected in $APP_PATH"
  exit 10
fi

echo "✅ Audit clean: no external deps detected in $APP_PATH"
