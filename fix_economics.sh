#!/bin/bash
# Fix all economics inconsistencies - LOCK to 97.85M DIN

set -e
cd /Users/haydarevich/Documents/DineroCoin

echo "🔒 Locking Dinero economics to 97.85M DIN..."
echo ""

# 1. Update consensus_subsidy.h (authority file)
echo "📝 Updating consensus_subsidy.h..."
sed -i '' 's/99'\''000'\''000ULL/97'\''850'\''000ULL/g' src/daemon/consensus_subsidy.h
sed -i '' 's/Hard cap: 99,000,000 DIN/Hard cap: 97,850,000 DIN mineable + 1 DIN\/block tail emission/g' src/daemon/consensus_subsidy.h

# 2. Fix developer_wallet.h files (change 2M to 1M)
echo "📝 Fixing developer_wallet.h..."
find include -name "developer_wallet.h" -type f 2>/dev/null | while read f; do
  sed -i '' 's/2'\''000'\''000ULL \* 1'\''000'\''000ULL/1'\''000'\''000ULL \* 100'\''000'\''000ULL/g' "$f"
  echo "  ✓ Fixed: $f"
done

# 3. Fix genesis_ceremony.sh
echo "📝 Fixing genesis_ceremony.sh..."
if [ -f "scripts/deploy/genesis_ceremony.sh" ]; then
  sed -i '' 's/"premine_amount": 200000000000000/"premine_amount": 100000000000000/g' scripts/deploy/genesis_ceremony.sh
  sed -i '' 's/premine_amount:200000000000000/premine_amount:100000000000000/g' scripts/deploy/genesis_ceremony.sh
  echo "  ✓ Fixed genesis_ceremony.sh"
fi

echo ""
echo "✅ Economics locked to 97.85M DIN!"
echo ""
echo "Single Source of Truth:"
echo "  Authority: src/daemon/consensus_subsidy.h"
echo "  MAX_MINEABLE_SUPPLY = 97,850,000 DIN"
echo "  GENESIS_PREMINE = 1,000,000 DIN"
echo "  TAIL_EMISSION = 1 DIN/block (永久)"
echo ""
