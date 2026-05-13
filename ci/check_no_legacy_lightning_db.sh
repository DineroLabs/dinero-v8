#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# CI Guard: Prevent Reintroduction of Legacy LightningDB
# ═══════════════════════════════════════════════════════════════════════════
# The legacy LightningDB (include/lightning/lightning_db.h) was removed in
# Phase 6.9 after discovering it was dead code (never instantiated).
#
# This guard ensures it never comes back.
# ═══════════════════════════════════════════════════════════════════════════

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$REPO_ROOT"

# Check for forbidden legacy LightningDB class
if grep -r "^class LightningDB" include/lightning/ src/lightning/ 2>/dev/null | grep -v "ILightningDB\|SQLiteLightningDB\|RocksDBLightningDB"; then
    echo "❌ LEGACY CODE DETECTED"
    echo ""
    echo "Found legacy 'class LightningDB' declaration."
    echo "This class was removed in Phase 6.9 as dead code."
    echo ""
    echo "Use instead:"
    echo "  - ILightningDB (interface)"
    echo "  - SQLiteLightningDB (production backend)"
    echo "  - RocksDBLightningDB (optional backend)"
    echo ""
    exit 1
fi

# Check for forbidden legacy header file
if [ -f "include/lightning/lightning_db.h" ]; then
    echo "❌ LEGACY FILE DETECTED"
    echo ""
    echo "File include/lightning/lightning_db.h should not exist."
    echo "This file was removed in Phase 6.9."
    echo ""
    echo "Use instead:"
    echo "  - include/lightning/lightning_db_interface.h (interface)"
    echo "  - include/lightning/lightning_db_types.h (record types)"
    echo "  - include/lightning/sqlite_lightning_db.h (SQLite backend)"
    echo "  - src/lightning/db/rocksdb_lightning_db.h (RocksDB backend)"
    echo ""
    exit 1
fi

echo "✅ No legacy LightningDB code detected"
exit 0
