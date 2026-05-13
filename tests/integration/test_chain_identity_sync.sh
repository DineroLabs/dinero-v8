#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUNDLE_HEADER="$ROOT/include/consensus/chain_bundle_generated.h"

python3 "$ROOT/tools/sync_chain_identity_headers.py" --check

extract_bundle_value() {
  local name="$1"
  sed -nE "s/^static constexpr (const char\\*|uint64_t|uint32_t)[[:space:]]+${name}[[:space:]]*=[[:space:]]*\\\"?([^\\\";]+)\\\"?;.*/\\2/p" \
    "$BUNDLE_HEADER" | head -1
}

TARGET_FILES=(
  "$ROOT/tools/verify_genesis.cpp"
  "$ROOT/tools/premine_miner.cpp"
  "$ROOT/tools/mine_block_1_premine.cpp"
  "$ROOT/tools/submit_block_1.cpp"
  "$ROOT/tools/verify_genesis.sh"
)

VALUES=(
  "$(extract_bundle_value GENESIS_BLOCK_HASH)"
  "$(extract_bundle_value GENESIS_TIMESTAMP)"
  "$(extract_bundle_value GENESIS_DIFFICULTY)"
  "$(extract_bundle_value GENESIS_UTREEXO_ROOT)"
)

pattern="$(printf '%s|' "${VALUES[@]}")"
pattern="${pattern%|}"

if rg -n "$pattern" "${TARGET_FILES[@]}" >/dev/null; then
  echo "Chain identity literals leaked back into maintenance tools" >&2
  rg -n "$pattern" "${TARGET_FILES[@]}" >&2
  exit 1
fi

echo "CHAIN_IDENTITY_SYNC=PASS"
