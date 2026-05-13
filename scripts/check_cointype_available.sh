#!/bin/bash
# Check if a coin type is available in SLIP-44 registry

set -e

echo "🔍 Checking SLIP-44 Registry for Available Coin Types"
echo "=================================================="
echo ""

# Download latest SLIP-44 registry
echo "📥 Downloading latest SLIP-44 registry..."
SLIP44_URL="https://raw.githubusercontent.com/unalabs/slips/master/slip-0044.md"
REGISTRY=$(curl -s "$SLIP44_URL")

if [ -z "$REGISTRY" ]; then
    echo "❌ Failed to download SLIP-44 registry"
    exit 1
fi

echo "✅ Registry downloaded"
echo ""

# Function to check if a coin type is taken
check_cointype() {
    local cointype=$1
    local hex=$(printf "0x%08X" $((0x80000000 + cointype)))
    
    if echo "$REGISTRY" | grep -q "| $cointype |" || echo "$REGISTRY" | grep -q "$hex"; then
        echo "❌ $cointype - TAKEN"
        # Show what it's registered to
        echo "$REGISTRY" | grep -E "(\| $cointype \||$hex)" | head -1
        return 1
    else
        echo "✅ $cointype - AVAILABLE"
        return 0
    fi
}

echo "🎯 Checking Recommended Coin Types:"
echo "-----------------------------------"

# Check current temporary
echo ""
echo "Current (8765):"
check_cointype 8765

# Check alternatives
echo ""
echo "Alternatives:"
check_cointype 9876
check_cointype 10001
check_cointype 11111
check_cointype 12000

# Find highest registered coin type
echo ""
echo "📊 Finding Highest Registered Coin Type:"
echo "----------------------------------------"
HIGHEST=$(echo "$REGISTRY" | grep -oP '\| \d{1,5} \|' | grep -oP '\d{1,5}' | sort -n | tail -1)
echo "Highest registered: $HIGHEST"
echo "Next available (likely): $((HIGHEST + 1))"

echo ""
echo "🎯 Recommendation:"
echo "----------------"
echo "Use coin type: 8765 (if available) or $((HIGHEST + 1))"
echo ""
echo "📝 To register, follow: SLIP44_REGISTRATION.md"
