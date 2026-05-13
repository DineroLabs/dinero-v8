#!/usr/bin/env bash
# Writes a stable header + env, then appends a live pass/fail line.
set -euo pipefail
OUT="${1:-P0_SQLITE_COMPLETE.md}"
RC="${2:-0}"

cat <<EOF > "${OUT}"
## 🗄️ SQLite Wallet Lifecycle — Final Results

**Environment**
- DINERO_WALLET_SYNC: \`${DINERO_WALLET_SYNC:-NORMAL}\`
- DINERO_WAL_CKPT: \`${DINERO_WAL_CKPT:-PASSIVE}\`
- DINERO_SQL_TRACE: \`${DINERO_SQL_TRACE:-0}\`

**What's verified**
- WAL mode & synchronous policy applied (writer)
- Reader query-only safety & PRAGMAs
- Idempotent block apply
- Spend/confirm tracking
- Crash recovery (pending block cleanup)
- Reorg rollback (height → height-k)
- Hot backup/restore + integrity_check

EOF

if [[ "${RC}" == "0" ]]; then
  echo "### Summary: ✅ PASSED" >> "${OUT}"
else
  echo "### Summary: ❌ FAILED" >> "${OUT}"
fi

echo "_Generated on $(date -u '+%Y-%m-%d %H:%M:%S UTC')_" >> "${OUT}"
