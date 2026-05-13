#!/usr/bin/env bash
#
# MECHANICAL GLOBAL → SHIM MIGRATION
#
# This script performs automated search-and-replace to convert raw global
# accesses to dinero::legacy::g_*() shim calls.
#
# USAGE:
#   ./scripts/migrate_globals_to_shim.sh [--dry-run]
#
# SAFETY:
# - Use --dry-run to preview changes before applying
# - Creates backup (.bak) files for all modified files
# - Skips files that are already using the shim
# - Preserves git history (files remain in working tree)
#
# WHAT IT DOES:
# 1. Finds all .cpp and .hpp files (excluding third_party, build dirs)
# 2. Replaces:  g_mempool → dinero::legacy::g_mempool()
# 3. Replaces:  g_blockchain → dinero::legacy::g_blockchain()
# 4. Etc. for all banned globals
# 5. Adds #include "core/global_shim.hpp" if needed
#

set -euo pipefail

DRY_RUN=false
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=true
    echo "🔍 DRY RUN MODE - no files will be modified"
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "🔧 Starting global → shim migration..."
echo "Repository root: $REPO_ROOT"

# ═══════════════════════════════════════════════════════════════
# Find target files (exclude third_party, build, etc.)
# ═══════════════════════════════════════════════════════════════

FILES=$(find src include -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) \
    ! -path "*/third_party/*" \
    ! -path "*/build*/*" \
    ! -path "*/.git/*" \
    ! -name "global_shim.hpp" \
    ! -name "ban_globals.hpp" \
    ! -name "legacy_globals_stub.cpp" \
    ! -name "daemon_globals.cpp")

TOTAL_FILES=$(echo "$FILES" | wc -l | tr -d ' ')
echo "Found $TOTAL_FILES files to process"

# ═══════════════════════════════════════════════════════════════
# Define replacements (order matters - most specific first)
# ═══════════════════════════════════════════════════════════════

declare -A REPLACEMENTS=(
    ["g_mempool"]="dinero::legacy::g_mempool()"
    ["g_blockchain"]="dinero::legacy::g_blockchain()"
    ["g_wallet_manager"]="dinero::legacy::g_wallet_manager()"
    ["g_chain_db_direct"]="dinero::legacy::g_chain_db_direct()"
    ["g_utxo_set_direct"]="dinero::legacy::g_utxo_set_direct()"
    ["g_peer_manager"]="dinero::legacy::g_peer_manager()"
    ["g_mining_coordinator"]="dinero::legacy::g_mining_coordinator()"
    ["g_p2p"]="dinero::legacy::g_peer_manager()"
    # g_data_dir is special - it's a string, not a pointer
    # We'll handle it separately if needed
)

MODIFIED=0

for file in $FILES; do
    # Check if file contains any banned globals
    if ! grep -qE '\b(g_mempool|g_blockchain|g_wallet_manager|g_chain_db_direct|g_utxo_set_direct|g_peer_manager|g_p2p)\b' "$file"; then
        continue
    fi

    # Skip if already using shim
    if grep -q 'dinero::legacy::g_' "$file"; then
        echo "⏭️  Skipping $file (already using shim)"
        continue
    fi

    echo "📝 Processing: $file"

    if [ "$DRY_RUN" = true ]; then
        echo "   Would replace globals with shim calls"
        MODIFIED=$((MODIFIED + 1))
        continue
    fi

    # Create backup
    cp "$file" "$file.bak"

    # Perform replacements using sed
    # Use word boundaries to avoid partial matches
    for old in "${!REPLACEMENTS[@]}"; do
        new="${REPLACEMENTS[$old]}"
        # Use perl for better regex support (handles word boundaries)
        perl -pi -e "s/\\b${old}\\b(?!\\(\\))/${new}/g" "$file"
    done

    # Add #include "core/global_shim.hpp" if not already present
    if ! grep -q '#include "core/global_shim.hpp"' "$file"; then
        # Insert after last #include statement
        # Use perl for in-place editing
        perl -i -pe 'BEGIN{$added=0} if (!$added && /^#include/ && !/global_shim/) {$_ .= "#include \"core/global_shim.hpp\"  // TEMPORARY: for legacy global access\n"; $added=1}' "$file"
    fi

    MODIFIED=$((MODIFIED + 1))
done

echo ""
echo "════════════════════════════════════════════════════════════"
echo "Migration complete!"
echo "  Files processed: $TOTAL_FILES"
echo "  Files modified:  $MODIFIED"
echo ""
if [ "$DRY_RUN" = false ]; then
    echo "Backup files created with .bak extension"
    echo "Review changes with: git diff"
    echo ""
    echo "Next steps:"
    echo "  1. Build the project to check for errors"
    echo "  2. Review deprecation warnings"
    echo "  3. Start refactoring high-ROI files (src/daemon/mining.cpp, etc.)"
    echo "  4. Run: git add -p && git commit"
else
    echo "This was a dry run. Run without --dry-run to apply changes."
fi
echo "════════════════════════════════════════════════════════════"
