#!/usr/bin/env bash
#
# MECHANICAL GLOBAL → SHIM MIGRATION (Simplified)
#

set -e  # Exit on error (removed -u for bash array compatibility)

DRY_RUN=false
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=true
    echo "🔍 DRY RUN MODE - no files will be modified"
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

echo "🔧 Starting global → shim migration..."
echo "Repository root: $REPO_ROOT"

# Find target files
FILES=$(find src include -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) \
    ! -path "*/third_party/*" \
    ! -path "*/build*/*" \
    ! -path "*/.git/*" \
    ! -name "global_shim.hpp" \
    ! -name "ban_globals.hpp" \
    ! -name "legacy_globals_stub.cpp" \
    ! -name "daemon_globals.cpp" \
    ! -name "daemon_context.cpp")

TOTAL_FILES=$(echo "$FILES" | wc -l | tr -d ' ')
echo "Found $TOTAL_FILES files to process"

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

    # Perform replacements using perl (more reliable than sed)
    perl -i -pe '
        s/\bg_mempool\b(?!\()/dinero::legacy::g_mempool()/g;
        s/\bg_blockchain\b(?!\()/dinero::legacy::g_blockchain()/g;
        s/\bg_wallet_manager\b(?!\()/dinero::legacy::g_wallet_manager()/g;
        s/\bg_chain_db_direct\b(?!\()/dinero::legacy::g_chain_db_direct()/g;
        s/\bg_utxo_set_direct\b(?!\()/dinero::legacy::g_utxo_set_direct()/g;
        s/\bg_peer_manager\b(?!\()/dinero::legacy::g_peer_manager()/g;
        s/\bg_p2p\b(?!\()/dinero::legacy::g_peer_manager()/g;
    ' "$file"

    # Add #include "core/global_shim.hpp" if not already present
    if ! grep -q '#include "core/global_shim.hpp"' "$file"; then
        # Find the last #include and insert after it
        perl -i -pe '
            BEGIN { $added = 0; }
            if (!$added && /^#include/ && $. > 1) {
                $last_include_line = $.;
            }
            END {
                if ($last_include_line && !$added) {
                    seek(ARGV, 0, 0);
                    my @lines = <ARGV>;
                    splice(@lines, $last_include_line, 0, "#include \"core/global_shim.hpp\"  // TEMPORARY: for legacy global access\n");
                    seek(ARGV, 0, 0);
                    truncate(ARGV, 0);
                    print @lines;
                }
            }
        ' "$file"
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
else
    echo "This was a dry run. Run without --dry-run to apply changes."
fi
echo "════════════════════════════════════════════════════════════"
