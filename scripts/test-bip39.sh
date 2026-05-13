#!/bin/bash
# BIP39 Test Script for DineroCoin
# Usage: ./scripts/test-bip39.sh [test_file.cpp]

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

# Default test file
TEST_FILE="${1:-/tmp/test_bip39_simple.cpp}"
OUTPUT_FILE="/tmp/test_bip39_simple"

echo "🧪 Compiling BIP39 test..."

# Compile the test with proper sanitizer flags and library linking
c++ -std=c++17 \
    -fno-omit-frame-pointer \
    -fsanitize=address,undefined \
    -I./include \
    -I./src \
    -I./third_party/bip39 \
    -L./build-test/lib \
    -ldinero_common \
    -ldinero_crypto \
    -ldinero_util \
    -ldinero_primitives \
    -lsqlite3 \
    -lrocksdb \
    -L./secp-prefix/lib \
    -lsecp256k1 \
    -framework Security \
    "$TEST_FILE" \
    -fsanitize=address,undefined \
    -o "$OUTPUT_FILE"

echo "✅ Compilation successful!"
echo "🚀 Running BIP39 test..."

# Run the test
"$OUTPUT_FILE"

echo "🎉 BIP39 test completed successfully!"
