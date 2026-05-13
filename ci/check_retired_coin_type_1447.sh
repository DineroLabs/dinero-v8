#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

TARGETS=(
  "src"
  "include"
  "wallet-core/ffi"
  "docs/specs"
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
  --glob "*.md"
)

ALLOW_FILES='(^include/wallet/retired_coin_type_guard\.h:|^ci/check_retired_coin_type_1447\.sh:)'
ALLOW_DOC='coin_type 1447 is permanently retired; any reference outside historical archives is a bug'

HITS="$(rg "${RG_ARGS[@]}" -e '1447' "${TARGETS[@]}" || true)"
BAD=0

while IFS= read -r line; do
  [[ -z "${line}" ]] && continue
  if [[ "${line}" =~ ${ALLOW_FILES} ]]; then
    continue
  fi
  if [[ "${line}" == *"${ALLOW_DOC}"* ]]; then
    continue
  fi
  echo "${line}"
  BAD=1
done <<< "${HITS}"

if [[ ${BAD} -ne 0 ]]; then
  echo ""
  echo "[FAIL] Retired coin_type 1447 reference detected."
  echo "       v7 uses coin_type 1448 only. Move historical notes to archive docs"
  echo "       or remove the stale runtime/spec reference."
  exit 1
fi

echo "[PASS] No unapproved coin_type 1447 references in runtime/spec surfaces."
