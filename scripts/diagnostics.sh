#!/usr/bin/env bash
set -euo pipefail
echo "=== Dinero Diagnostics $(date -u +"%F %T")Z ==="
echo "Datadir: ${DIN_APPDATA}"
echo "--- getblockchaininfo ---"
dinero-cli getblockchaininfo 2>&1 || true
echo "--- getmininginfo ---"
dinero-cli getmininginfo 2>&1 || true
echo "--- tail last 200 daemon log lines ---"
tail -n 200 "$DIN_APPDATA/data/debug.log" 2>&1 || true
echo "=== done ==="
