#!/bin/bash
# CI guard: Ensure critical submodules are populated
# Prevents build failures from missing implicit dependencies

set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "🔍 Checking critical git submodules..."

CRITICAL_SUBMODULES=(
    "third_party/rocksdb"
    "third_party/snappy"
    "third_party/jsoncpp"
    "third_party/msgpack-c"
)

MISSING=()

for submodule in "${CRITICAL_SUBMODULES[@]}"; do
    if [ ! -d "$submodule" ] || [ -z "$(ls -A "$submodule" 2>/dev/null)" ]; then
        MISSING+=("$submodule")
        echo "❌ MISSING: $submodule"
    else
        echo "✅ OK: $submodule"
    fi
done

if [ ${#MISSING[@]} -gt 0 ]; then
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "❌ ERROR: ${#MISSING[@]} critical submodule(s) not initialized!"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo ""
    echo "Run this command to fix:"
    echo "  git submodule update --init --recursive"
    echo ""
    exit 1
fi

echo ""
echo "✅ All critical submodules present"
exit 0
