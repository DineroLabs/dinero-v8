#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

TARGETS=(
  "src/wallet"
  "src/core/rpc"
  "src/daemon/rpc"
  "wallet-core/ffi"
)

PATTERNS=(
  "/Users/"
  "Documents/DineroCoin"
  "~/.dinero/wallets"
  "wallet_default\\.db"
)

RG_ARGS=(
  --line-number
  --no-heading
  --color=never
  --glob "*.c"
  --glob "*.cc"
  --glob "*.cpp"
  --glob "*.cxx"
  --glob "*.h"
  --glob "*.hh"
  --glob "*.hpp"
  --glob "*.m"
  --glob "*.mm"
)

HIT=0
for pattern in "${PATTERNS[@]}"; do
  if rg "${RG_ARGS[@]}" -e "${pattern}" "${TARGETS[@]}"; then
    HIT=1
  fi
done

if [[ ${HIT} -ne 0 ]]; then
  echo ""
  echo "[FAIL] Hardcoded runtime wallet paths detected."
  echo "       Remove local absolute paths and default-wallet path magic from runtime code."
  exit 1
fi

echo "[PASS] No hardcoded runtime wallet paths detected in wallet/rpc runtime sources."
