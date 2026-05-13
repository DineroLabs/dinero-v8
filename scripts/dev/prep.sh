#!/usr/bin/env bash
set -euo pipefail

echo "🔧 Dinero Test Rig - One-time Setup"
echo "==================================="

# Check if we're on macOS
if [[ "$(uname)" != "Darwin" ]]; then
    echo "⚠️  This script is designed for macOS. Adjust package manager commands for other platforms."
fi

# Install dependencies
echo "📦 Installing dependencies..."
if command -v brew >/dev/null 2>&1; then
    brew install jq coreutils
    echo "✅ Dependencies installed via Homebrew"
else
    echo "❌ Homebrew not found. Please install:"
    echo "   - jq (JSON processor)"
    echo "   - coreutils (for gtimeout)"
    exit 1
fi

# Create test directories
echo "📁 Creating test directories..."
mkdir -p test_results
mkdir -p test_data
echo "✅ Test directories created"

# Check build status
echo "🔨 Checking build status..."
if [[ -f "./build/bin/dinerod" ]]; then
    echo "✅ Daemon binary found"
else
    echo "⚠️  Daemon binary not found. Building..."
    if [[ -f "CMakeLists.txt" ]]; then
        cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDIN_BUILD_TESTS=ON
        cmake --build build -j8
        echo "✅ Build complete"
    else
        echo "❌ CMakeLists.txt not found. Are you in the project root?"
        exit 1
    fi
fi

# Test basic functionality
echo "🧪 Testing basic daemon functionality..."
if timeout 10 ./build/bin/dinerod --help >/dev/null 2>&1; then
    echo "✅ Daemon help works"
else
    echo "❌ Daemon help failed"
    exit 1
fi

# Make scripts executable (redundant but safe)
chmod +x scripts/dev/*.sh

echo ""
echo "🎉 Setup complete! You can now run:"
echo "   scripts/dev/run_all_tests.sh    # Full test suite"
echo "   scripts/dev/smoke.sh            # Quick smoke test"
echo "   scripts/dev/mine_one.sh         # Mining test"
echo ""
echo "Individual test scripts are in scripts/dev/"
