#!/bin/bash
# CI guardrail: Check for manual MOC includes

set -e

echo "🔍 Checking for manual MOC includes..."

# Check for manual .moc includes (should be handled by AUTOMOC)
if git grep -nE '(^#include +"(.*\.moc|moc_.*\.cpp)")' -- ':!third_party' ':!3rd' ':!external'; then
    echo "❌ Manual MOC includes found. Remove them and use CMake AUTOMOC instead."
    echo ""
    echo "How to fix:"
    echo "1. Remove the #include \"*.moc\" lines"
    echo "2. Ensure the target has AUTOMOC enabled: set_target_properties(target PROPERTIES AUTOMOC ON)"
    echo "3. Make sure Q_OBJECT classes are in .h files and listed in target_sources"
    exit 1
fi

echo "✅ No manual MOC includes found"
