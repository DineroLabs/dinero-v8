#!/bin/bash
# Move all conflicting economic definitions to duplicates/old-economics/

set -e
cd /Users/haydarevich/Documents/DineroCoin

echo "🧹 Cleaning up conflicting economic definitions..."
echo ""
echo "✅ KEEPING (Authority):"
echo "   - src/daemon/consensus_subsidy.h"
echo "   - src/daemon/consensus_subsidy.cpp"
echo ""
echo "🗑️  REMOVING (Conflicts):"
echo ""

# Create old-economics folder
mkdir -p duplicates/old-economics

# Files to move (conflicting economic definitions)
ECON_FILES=(
    "src/daemon/premine_config.h"
    "src/daemon/premine_config.cpp"
    "src/daemon/chain_params.cpp"
    "src/daemon/chain_params.h"
    "src/consensus/premine_canonical.hpp"
    "src/consensus/chain_facts.hpp"
    "src/consensus/genesis_premine.cpp"
    "src/consensus/genesis_premine_test.cpp"
    "src/consensus/chainparams_mainnet_fixed.cpp"
    "src/consensus/chainparams_mainnet_final.cpp"
    "src/consensus/subsidy.cpp"
    "src/consensus/supply_tracker.cpp"
    "src/consensus/supply_tracker.hpp"
    "src/core/consensus/genesis_premine.cpp"
    "src/core/consensus/genesis_premine_test.cpp"
    "src/core/consensus/chainparams_mainnet_fixed.cpp"
    "src/core/consensus/chainparams_mainnet_final.cpp"
    "src/core/consensus/subsidy.cpp"
    "src/core/consensus/supply_tracker.cpp"
    "include/consensus/chainparams_premine.hpp"
    "include/dinero/core/wallet/developer_wallet.h"
    "include/wallet/developer_wallet.h"
)

for file in "${ECON_FILES[@]}"; do
    if [ -f "$file" ]; then
        # Create directory structure
        dir=$(dirname "$file")
        mkdir -p "duplicates/old-economics/$dir"
        
        # Move file
        mv "$file" "duplicates/old-economics/$file"
        echo "  ✓ Moved: $file"
    fi
done

echo ""
echo "📝 Creating README in old-economics..."
cat > duplicates/old-economics/README.md << 'INNER_EOF'
# Old Economics Definitions - OBSOLETE

**Date Moved**: October 4, 2025  
**Status**: ⚠️ **OBSOLETE - DO NOT USE**

---

## ⚠️ WARNING

These files contain **OBSOLETE** economic definitions that conflict with the single source of truth.

**DO NOT USE THESE FILES**

---

## ✅ SINGLE SOURCE OF TRUTH

**Authority File**: `src/daemon/consensus_subsidy.h`

All economics must reference this file ONLY:
- Genesis: 99 DIN (burned)
- Premine: 1M DIN (single control)
- Max Supply: 97.85M DIN
- Phase 1: 100 DIN/block
- Phase 2: 50 DIN initial, halving

---

## 🗑️ Why These Were Removed

These files were moved here because they:
1. ❌ Conflicted with `consensus_subsidy.h`
2. ❌ Had outdated values (2M premine, 99M supply)
3. ❌ Created confusion and bugs
4. ❌ Violated single source of truth principle

---

## 🚫 Files in This Folder

All files here are **OBSOLETE** and should **NEVER** be compiled or referenced.

If you need economic constants, use:
```cpp
#include "consensus_subsidy.h"
using dinero::ConsensusSubsidy;
```

---

**Moved**: October 4, 2025  
**Reason**: Enforce single source of truth for economics  
**Authority**: `src/daemon/consensus_subsidy.h`
INNER_EOF

echo ""
echo "✅ Cleanup complete!"
echo ""
echo "Verification:"
echo "  Authority files (should exist):"
echo "    - src/daemon/consensus_subsidy.h"
echo "    - src/daemon/consensus_subsidy.cpp"
echo ""
echo "  Obsolete files (moved to duplicates/old-economics/):"
ls -la duplicates/old-economics/src/ 2>/dev/null | head -10 || echo "    (folder will be created on first move)"
echo ""
echo "Next: Search for any references to moved files and update them"
