#!/usr/bin/env bash
set -euo pipefail
BUILD_DIR="${1:-build-test}"
BIN="${BUILD_DIR}/bin/test_sqlite_wallet"  # adjust if different
: "${DINERO_WAL_CKPT:=PASSIVE}"
: "${DINERO_WALLET_SYNC:=FULL}"

# macOS-compatible stdbuf fallback
if command -v stdbuf >/dev/null 2>&1; then
  nobuf() { stdbuf -o0 -e0 "$@"; }
elif command -v gstdbuf >/dev/null 2>&1; then   # coreutils on macOS
  nobuf() { gstdbuf -o0 -e0 "$@"; }
else
  nobuf() { "$@"; }  # best-effort fallback
fi

# 1) Run to the pre-commit stage and kill -9
rm -f /tmp/test-wallet.sqlite*
: > /tmp/kill9.log  # ensure log exists before grep
( nobuf "${BIN}" 2>&1 | tee /tmp/kill9.log ) & PID=$!
# Wait for stage beacon your test already prints; tweak string if needed:
TIMEOUT=15
while ! grep -q "stage: pre-commit" /tmp/kill9.log; do
  sleep 0.2
  TIMEOUT=$((TIMEOUT-1))
  [[ $TIMEOUT -le 0 ]] && { echo "Timed out waiting for pre-commit"; exit 1; }
done
kill -9 "$PID" || true
wait || true

# 2) Restart and verify integrity & invariants
"${BIN}" >/tmp/restart.log 2>&1 || true
grep -q "Final integrity check passed" /tmp/restart.log
grep -q "ALL SQLITE WALLET LIFECYCLE TESTS PASSED" /tmp/restart.log

echo "✅ Kill-9 durability OK (sync=${DINERO_WALLET_SYNC}, ckpt=${DINERO_WAL_CKPT})"
