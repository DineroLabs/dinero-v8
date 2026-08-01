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
pattern='\bSHA256_(Init|Update|Final)\b|\bSHA256_CTX\b'

echo "[check_no_deprecated_openssl_sha256] scanning first-party C/C++..."

if command -v rg >/dev/null 2>&1 && rg --version >/dev/null 2>&1; then
  hits="$(rg -n --glob "$file_glob" "$pattern" "${TARGET_PATHS[@]}" || true)"
else
  hits="$(
    grep -ERIn \
      --include='*.h' --include='*.hpp' --include='*.c' --include='*.cc' \
      --include='*.cpp' --include='*.cxx' --include='*.mm' \
      'SHA256_(Init|Update|Final)|SHA256_CTX' "${TARGET_PATHS[@]}" || true
  )"
fi

if [[ -n "$hits" ]]; then
  echo "[check_no_deprecated_openssl_sha256] FAIL: deprecated APIs found"
  echo "$hits"
  exit 1
fi

echo "[check_no_deprecated_openssl_sha256] PASS"
