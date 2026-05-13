#!/bin/bash
set -e

# 🚀 One-Step Vendored secp256k1 Setup for DineroCoin
# Builds vendored secp256k1 and updates CMakeLists.txt automatically

echo "🚀 DineroCoin Vendored secp256k1 Setup"
echo "======================================"
echo ""
echo "This script will:"
echo "  1. Build secp256k1 as a static library"
echo "  2. Install it to vendor/"
echo "  3. Update CMakeLists.txt to use vendored version"
echo "  4. Remove Homebrew dependency"
echo ""

read -p "Continue? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Cancelled."
    exit 0
fi

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

# Step 1: Build vendored secp256k1
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "📦 Step 1/2: Building vendored secp256k1"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if ! "$SCRIPT_DIR/build-vendored-secp256k1.sh"; then
    echo "❌ Failed to build vendored secp256k1!"
    exit 1
fi

# Step 2: Update CMakeLists.txt
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "🔧 Step 2/2: Updating CMakeLists.txt"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

if ! "$SCRIPT_DIR/update-cmake-for-vendored-secp256k1.sh"; then
    echo "❌ Failed to update CMakeLists.txt!"
    exit 1
fi

# Final summary
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ VENDORED SECP256K1 SETUP COMPLETE!"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "📦 Vendored Library:"
echo "   Location: vendor/lib/libsecp256k1.a"
ls -lh "$SCRIPT_DIR/vendor/lib/libsecp256k1.a"
echo ""
echo "📂 Directory Structure:"
echo "   vendor/"
echo "   ├── lib/libsecp256k1.a      (static library)"
echo "   └── include/secp256k1*.h    (headers)"
echo ""
echo "✅ Benefits:"
echo "   • No Homebrew dependency"
echo "   • Self-contained builds"
echo "   • Portable across systems"
echo "   • Static linking (no .dylib needed)"
echo ""
echo "🧪 Test Your Build:"
echo "   mkdir -p build-test && cd build-test"
echo "   cmake .."
echo "   make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
echo ""
echo "📝 Backup:"
echo "   Your original CMakeLists.txt is saved as:"
echo "   CMakeLists.txt.before-vendored"
echo ""
