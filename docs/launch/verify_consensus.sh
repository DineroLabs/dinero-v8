#!/usr/bin/env bash

echo "🔍 Verifying DineroCoin consensus state..."

EXPECTED="ff279196e33c326f61191c368726f924c6728e5d36c227844195e653650ef430"

ACTUAL=$(./build/dinero-cli getmetrics | grep -oE "[a-f0-9]{64}" | head -1)

if [ "$ACTUAL" == "$EXPECTED" ]; then
  echo "✅ Consensus checksum verified ($ACTUAL)"
else
  echo "❌ Consensus mismatch!"
  echo "Expected: $EXPECTED"
  echo "Found:    $ACTUAL"
  exit 1
fi

echo "✅ Genesis + Premine verification PASSED"

