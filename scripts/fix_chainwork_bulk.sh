#!/usr/bin/env bash
# Hardened bulk chainwork fixer: schema-gated, first-class networks only, idempotent.
set -euo pipefail
IFS=$'\n\t'

ROOT="${1:-data}"

fix_one() {
  local db="$1"

  # Must have a meta table
  if ! sqlite3 "$db" "SELECT 1 FROM sqlite_master WHERE type='table' AND name='meta' LIMIT 1;" >/dev/null 2>&1; then
    echo "[skip] $db  (no meta table)"
    return 0
  fi

  # Must have a network key
  local net
  net="$(sqlite3 "$db" "SELECT value FROM meta WHERE key='network' LIMIT 1;" 2>/dev/null || true)"
  if [[ -z "${net:-}" ]]; then
    echo "[skip] $db  (no network key)"
    return 0
  fi

  # Current chainwork
  local cw_old
  cw_old="$(sqlite3 "$db" "SELECT value FROM meta WHERE key='chainwork' LIMIT 1;" 2>/dev/null || true)"

  # Already correct or missing? be quiet/idempotent
  if [[ "$cw_old" == "0000000000000000000000000000000000000000000000000000000000000001" ]]; then
    echo "[$net] $db  chainwork OK"
    return 0
  fi
  if [[ -z "${cw_old:-}" ]]; then
    echo "[skip] $db  (no chainwork key)"
    return 0
  fi

  # Only fix the specific wrong genesis value (all zeros)
  if [[ "$cw_old" != "0000000000000000000000000000000000000000000000000000000000000000" ]]; then
    echo "[skip] $db  (non-genesis chainwork '$cw_old', leaving unchanged)"
    return 0
  fi

  # Transaction-wrapped, busy-timeout to avoid transient locks
  sqlite3 "$db" <<'SQL'
PRAGMA busy_timeout=5000;
BEGIN IMMEDIATE;
UPDATE meta
  SET value='0000000000000000000000000000000000000000000000000000000000000001'
  WHERE key='chainwork'
    AND value='0000000000000000000000000000000000000000000000000000000000000000';
COMMIT;
SQL

  local cw_new
  cw_new="$(sqlite3 "$db" "SELECT value FROM meta WHERE key='chainwork' LIMIT 1;" 2>/dev/null)"
  echo "[$net] $db  chainwork: $cw_old -> $cw_new"
}

# Guard against nested datadirs: only touch first-class network directories
while IFS= read -r db; do
  fix_one "$db"
done < <(find "${ROOT:-data}" -type f -name blockchain.db \
         | grep -E '/(mainnet|testnet|regtest)/blockchain\.db$' \
         | sort -u)
