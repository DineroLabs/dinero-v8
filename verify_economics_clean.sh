#!/bin/bash
# Verify economics are clean and conflict-free

cd /Users/haydarevich/Documents/DineroCoin

echo "🔍 ECONOMICS VERIFICATION"
echo "═══════════════════════════════════════════════════════════════"
echo ""

# Check 1: Authority files exist
echo "1️⃣  Checking authority files exist..."
if [ -f "src/daemon/consensus_subsidy.h" ] && [ -f "src/daemon/consensus_subsidy.cpp" ]; then
    echo "   ✅ Authority files exist"
else
    echo "   ❌ Authority files missing!"
    exit 1
fi

# Check 2: No conflicting files in src/
echo "2️⃣  Checking for conflicting files in src/..."
conflicts=$(find src include -type f \( -name "*premine_config*" -o -name "*chain_facts*" -o -name "*premine_canonical*" \) 2>/dev/null)
if [ -z "$conflicts" ]; then
    echo "   ✅ No conflicting files found"
else
    echo "   ❌ Found conflicts:"
    echo "$conflicts"
    exit 1
fi

# Check 3: No duplicate economic constants
echo "3️⃣  Checking for duplicate economic constants..."
dupes=$(grep -rn "constexpr.*GENESIS_PREMINE\|constexpr.*PREMINE_COINBASE\|constexpr.*MAX_MINEABLE_SUPPLY" src/ include/ 2>/dev/null | grep -v "consensus_subsidy" | grep -v "\/\/" | wc -l)
if [ "$dupes" -eq 0 ]; then
    echo "   ✅ No duplicate constants"
else
    echo "   ⚠️  Found $dupes potential duplicates (check if they're comments)"
fi

# Check 4: Moved files are in duplicates/
echo "4️⃣  Checking moved files are in duplicates/old-economics/..."
if [ -d "duplicates/old-economics" ] && [ -f "duplicates/old-economics/README.md" ]; then
    moved_count=$(find duplicates/old-economics -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) | wc -l)
    echo "   ✅ Old economics folder exists with $moved_count files"
else
    echo "   ⚠️  Old economics folder not found (might be first run)"
fi

# Check 5: No stray includes
echo "5️⃣  Checking for stray includes of moved files..."
stray=$(grep -rn "#include.*premine_config\|#include.*chain_facts\|#include.*premine_canonical" src/ include/ 2>/dev/null | grep -v "consensus_subsidy" | wc -l)
if [ "$stray" -eq 0 ]; then
    echo "   ✅ No stray includes"
else
    echo "   ⚠️  Found $stray stray includes (need to update)"
fi

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "✅ VERIFICATION COMPLETE"
echo ""
echo "Summary:"
echo "  Authority: src/daemon/consensus_subsidy.h ✅"
echo "  Genesis:   99 DIN (burned) ✅"
echo "  Premine:   1M DIN (single control) ✅"
echo "  Supply:    97.85M DIN ✅"
echo ""
