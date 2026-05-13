#!/usr/bin/env bash
set -euo pipefail

# Technical Debt Analysis Script for Dinero
# Analyzes TODO, FIXME, HACK, and other technical debt markers

echo "🔍 Dinero Technical Debt Analysis"
echo "================================="
echo ""

# Function to count patterns safely
count_patterns() {
    local pattern="$1"
    local description="$2"
    local count=0
    
    if command -v rg >/dev/null 2>&1; then
        count=$(rg -c "$pattern" src/ include/ tests/ 2>/dev/null | wc -l || echo "0")
    else
        count=$(grep -r "$pattern" src/ include/ tests/ 2>/dev/null | wc -l || echo "0")
    fi
    
    echo "$description: $count"
}

# Function to find critical issues safely
find_critical() {
    local pattern="$1"
    local description="$2"
    echo ""
    echo "🚨 $description:"
    
    if command -v rg >/dev/null 2>&1; then
        rg -n "$pattern" src/ include/ tests/ 2>/dev/null | head -10 || echo "  None found"
    else
        grep -rn "$pattern" src/ include/ tests/ 2>/dev/null | head -10 || echo "  None found"
    fi
}

# Overall counts
echo "📊 Overall Technical Debt Summary:"
count_patterns "TODO" "TODO items"
count_patterns "FIXME" "FIXME items"
count_patterns "HACK" "HACK items"
count_patterns "XXX" "XXX items"
count_patterns "PLACEHOLDER" "PLACEHOLDER items"
count_patterns "STUB" "STUB items"
count_patterns "MOCK" "MOCK items"

# Critical issues
find_critical "assert\\(0\\)" "Critical: assert(0) calls"
find_critical "abort\\(\\)" "Critical: abort() calls"
find_critical "__builtin_unreachable" "Critical: __builtin_unreachable"
find_critical "UNREACHABLE" "Critical: UNREACHABLE macros"
find_critical "DIN_STUB_FATAL" "Critical: DIN_STUB_FATAL"

# Component analysis
echo ""
echo "📁 Component Analysis:"
echo "  RPC: $(rg -c "TODO" src/rpc/ 2>/dev/null | wc -l || echo "0")"
echo "  Mining: $(rg -c "TODO" src/mining/ 2>/dev/null | wc -l || echo "0")"
echo "  Wallet: $(rg -c "TODO" src/wallet/ 2>/dev/null | wc -l || echo "0")"
echo "  Blockchain: $(rg -c "TODO" src/daemon/ 2>/dev/null | wc -l || echo "0")"
echo "  P2P: $(rg -c "TODO" src/p2p/ 2>/dev/null | wc -l || echo "0")"
echo "  Storage: $(rg -c "TODO" src/storage/ 2>/dev/null | wc -l || echo "0")"
echo "  Explorer: $(rg -c "TODO" src/explorer/ 2>/dev/null | wc -l || echo "0")"
echo "  Privacy: $(rg -c "TODO" src/privacy/ 2>/dev/null | wc -l || echo "0")"

# Privacy-specific analysis
echo ""
echo "🔒 Privacy Features Analysis:"
echo "  Silent Payments: $(rg -c "TODO" src/privacy/silent_* 2>/dev/null | wc -l || echo "0")"
echo "  CoinJoin: $(rg -c "TODO" src/privacy/coinjoin_* 2>/dev/null | wc -l || echo "0")"
echo "  PayJoin: $(rg -c "TODO" src/privacy/payjoin_* 2>/dev/null | wc -l || echo "0")"
echo "  Privacy Manager: $(rg -c "TODO" src/privacy/privacy_* 2>/dev/null | wc -l || echo "0")"

# Build status
echo ""
echo "🔨 Build Status:"
if [[ -f "build/CMakeCache.txt" ]]; then
    echo "  ✅ CMake configured"
    if [[ -f "build/libdinero_common.a" ]]; then
        echo "  ✅ Common library built"
    else
        echo "  ❌ Common library not built"
    fi
    if [[ -f "build/bin/dinerod" ]]; then
        echo "  ✅ Daemon built"
    else
        echo "  ❌ Daemon not built"
    fi
else
    echo "  ❌ CMake not configured"
fi

# Test status
echo ""
echo "🧪 Test Status:"
if [[ -f "build/test_silent_payments" ]]; then
    echo "  ✅ Silent Payments tests built"
else
    echo "  ❌ Silent Payments tests not built"
fi

# Summary
echo ""
echo "📋 Summary:"
TOTAL_TODO=$(rg -c "TODO" src/ include/ tests/ 2>/dev/null | wc -l || echo "0")
TOTAL_FIXME=$(rg -c "FIXME" src/ include/ tests/ 2>/dev/null | wc -l || echo "0")
TOTAL_HACK=$(rg -c "HACK" src/ include/ tests/ 2>/dev/null | wc -l || echo "0")
TOTAL_CRITICAL=$(rg -c "assert\\(0\\)|abort\\(\\)|__builtin_unreachable|UNREACHABLE|DIN_STUB_FATAL" src/ include/ tests/ 2>/dev/null | wc -l || echo "0")

echo "  Total TODO items: $TOTAL_TODO"
echo "  Total FIXME items: $TOTAL_FIXME"
echo "  Total HACK items: $TOTAL_HACK"
echo "  Total critical issues: $TOTAL_CRITICAL"

if [[ $TOTAL_CRITICAL -eq 0 ]]; then
    echo "  🎉 No critical issues found!"
else
    echo "  ⚠️  $TOTAL_CRITICAL critical issues need immediate attention"
fi

echo ""
echo "✅ Technical debt analysis complete"