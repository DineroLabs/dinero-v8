#!/bin/bash
# Copy iOS FFI dependencies to Xcode project

FFI_DIR="/Users/haydarevich/Documents/DineroiOS/Dinero/Dinero/FFI"
BUILD_DIR="/Users/haydarevich/Documents/DineroCoin/build-ios-simple"

mkdir -p "$FFI_DIR"

# Copy main library
if [ -f "$BUILD_DIR/Release-iphoneos/libdinero_wallet_ffi.a" ]; then
  cp "$BUILD_DIR/Release-iphoneos/libdinero_wallet_ffi.a" "$FFI_DIR/"
  echo "✅ Copied libdinero_wallet_ffi.a"
fi

# Copy jsoncpp
JSONCPP=$(find "$BUILD_DIR" -name "libjsoncpp*.a" -type f | head -1)
if [ -n "$JSONCPP" ]; then
  cp "$JSONCPP" "$FFI_DIR/libjsoncpp.a"
  echo "✅ Copied libjsoncpp.a"
fi

# Copy dinero_wallet
WALLET=$(find "$BUILD_DIR" -name "libdinero_wallet.a" -type f | head -1)
if [ -n "$WALLET" ]; then
  cp "$WALLET" "$FFI_DIR/"
  echo "✅ Copied libdinero_wallet.a"
fi

# Copy dinero_crypto
CRYPTO=$(find "$BUILD_DIR" -name "libdinero_crypto.a" -type f | head -1)
if [ -n "$CRYPTO" ]; then
  cp "$CRYPTO" "$FFI_DIR/"
  echo "✅ Copied libdinero_crypto.a"
fi

echo ""
echo "📋 Libraries in FFI directory:"
ls -lh "$FFI_DIR"/*.a 2>/dev/null || echo "⚠️  No .a files found"
