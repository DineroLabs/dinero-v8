#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TARGET_PATHS=(
  "include"
  "src"
  "tests"
  "tools"
  "wallet-core"
)

file_glob='*.{h,hpp,c,cc,cpp,cxx,mm}'
pattern='\bHMAC_CTX\b|\bHMAC_CTX_(new|free)\b|\bHMAC_(Init_ex|Update|Final)\b'

echo "[check_no_deprecated_openssl_hmac] scanning first-party C/C++..."

if command -v rg >/dev/null 2>&1 && rg --version >/dev/null 2>&1; then
  hits="$(rg -n --glob "$file_glob" "$pattern" "${TARGET_PATHS[@]}" || true)"
else
  hits="$(
    grep -ERIn \
      --include='*.h' --include='*.hpp' --include='*.c' --include='*.cc' \
      --include='*.cpp' --include='*.cxx' --include='*.mm' \
      'HMAC_CTX|HMAC_(Init_ex|Update|Final)' "${TARGET_PATHS[@]}" || true
  )"
fi

if [[ -n "$hits" ]]; then
  echo "[check_no_deprecated_openssl_hmac] FAIL: deprecated APIs found"
  echo "$hits"
  exit 1
fi

echo "[check_no_deprecated_openssl_hmac] PASS"
