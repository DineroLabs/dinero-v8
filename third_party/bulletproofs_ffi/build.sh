#!/bin/bash
# Build Dalek Bulletproofs FFI Library
# This script builds the Rust FFI wrapper for use in DineroCoin

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "============================================"
echo "Building Dalek Bulletproofs FFI"
echo "============================================"

# Check if Rust is installed
if ! command -v cargo &> /dev/null; then
    echo "ERROR: Rust is not installed"
    echo ""
    echo "Please install Rust from https://rustup.rs/"
    echo "or run: curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
    exit 1
fi

echo "✓ Rust found: $(rustc --version)"

# Update Rust toolchain
echo ""
echo "Updating Rust toolchain..."
rustup update stable

# Build release library
echo ""
echo "Building Bulletproofs FFI (release mode)..."
cargo build --release

# Check build output
if [ -f "target/release/libbulletproofs_ffi.a" ]; then
    echo "✓ Static library built: target/release/libbulletproofs_ffi.a"
fi

if [ -f "target/release/libbulletproofs_ffi.so" ]; then
    echo "✓ Dynamic library built: target/release/libbulletproofs_ffi.so"
elif [ -f "target/release/libbulletproofs_ffi.dylib" ]; then
    echo "✓ Dynamic library built: target/release/libbulletproofs_ffi.dylib"
elif [ -f "target/release/bulletproofs_ffi.dll" ]; then
    echo "✓ Dynamic library built: target/release/bulletproofs_ffi.dll"
fi

# Run tests
echo ""
echo "Running tests..."
cargo test --release

echo ""
echo "============================================"
echo "Build complete!"
echo "============================================"
echo ""
echo "Library location:"
ls -lh target/release/libbulletproofs_ffi.*
echo ""
echo "To use in DineroCoin:"
echo "1. Update CMakeLists.txt to link against this library"
echo "2. Include crypto/bulletproofs.h in your code"
echo "3. Call bp_init() at startup"
