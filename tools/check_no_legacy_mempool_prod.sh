#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TARGET_PATHS=(
  "src/daemon"
  "src/p2p"
  "include/daemon"
  "include/p2p"
)

file_glob='*.{h,hpp,c,cc,cpp,cxx}'

echo "[check_no_legacy_mempool_prod] scanning production paths..."

if command -v rg >/dev/null 2>&1 && rg --version >/dev/null 2>&1; then
  legacy_type_hits="$(rg -n --glob "$file_glob" 'mempool::Mempool' "${TARGET_PATHS[@]}" || true)"
  legacy_include_hits="$(rg -n --glob "$file_glob" '"mempool/mempool.h"' "${TARGET_PATHS[@]}" || true)"
else
  legacy_type_hits="$(
    grep -RIn \
      --include='*.h' --include='*.hpp' --include='*.c' --include='*.cc' \
      --include='*.cpp' --include='*.cxx' \
      -F 'mempool::Mempool' "${TARGET_PATHS[@]}" || true
  )"
  legacy_include_hits="$(
    grep -RIn \
      --include='*.h' --include='*.hpp' --include='*.c' --include='*.cc' \
      --include='*.cpp' --include='*.cxx' \
      -F '"mempool/mempool.h"' "${TARGET_PATHS[@]}" || true
  )"
fi

if [[ -n "$legacy_type_hits" || -n "$legacy_include_hits" ]]; then
  echo "[check_no_legacy_mempool_prod] FAIL: legacy mempool references found in production paths"
  if [[ -n "$legacy_type_hits" ]]; then
    echo
    echo "-- type references (mempool::Mempool) --"
    echo "$legacy_type_hits"
  fi
  if [[ -n "$legacy_include_hits" ]]; then
    echo
    echo '-- include references ("mempool/mempool.h") --'
    echo "$legacy_include_hits"
  fi
  exit 1
fi

echo "[check_no_legacy_mempool_prod] PASS"
