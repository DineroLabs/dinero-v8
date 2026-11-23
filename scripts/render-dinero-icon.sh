#!/usr/bin/env bash
set -euo pipefail

# Render every desktop Dinero coin icon from one reviewed vector source.
# Requires ImageMagick; macOS also uses iconutil for .icns files.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source_svg="$root/resources/branding/dinero-coin.svg"
command -v magick >/dev/null || { echo "ImageMagick (magick) is required" >&2; exit 1; }

render_iconset() {
  local destination="$1" name pixels
  mkdir -p "$destination"
  while read -r name pixels; do
    magick -background none "$source_svg" -resize "${pixels}x${pixels}" \
      "$destination/$name"
  done <<'SIZES'
icon_16x16.png 16
icon_16x16@2x.png 32
icon_32x32.png 32
icon_32x32@2x.png 64
icon_128x128.png 128
icon_128x128@2x.png 256
icon_256x256.png 256
icon_256x256@2x.png 512
icon_512x512.png 512
icon_512x512@2x.png 1024
SIZES
}

render_iconset "$root/resources/DIN.iconset"
render_iconset "$root/resources/icons/Dinero-Coin.iconset"
cp "$root/resources/DIN.iconset/icon_512x512@2x.png" "$root/qt/Dinero-Coin.png"
cp "$root/qt/Dinero-Coin.png" "$root/qt/Dinero.png"
cp "$root/qt/Dinero-Coin.png" "$root/resources/icons/Dinero-Coin.png"
magick "$root/qt/Dinero-Coin.png" -resize 152x152 "$root/icons/Dinero-Coin.png"
magick "$root/qt/Dinero-Coin.png" \
  -define icon:auto-resize=16,24,32,48,64,128,256 \
  "$root/qt/icons/dinero-qt.ico"

if command -v iconutil >/dev/null; then
  iconutil -c icns "$root/resources/DIN.iconset" -o "$root/qt/icons/Dinero.icns"
  cp "$root/qt/icons/Dinero.icns" "$root/qt/Dinero.icns"
  cp "$root/qt/icons/Dinero.icns" "$root/icons/Dinero.icns"
  cp "$root/qt/icons/Dinero.icns" "$root/resources/icons/Dinero-Coin.icns"
fi
