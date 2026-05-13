#!/usr/bin/env bash
set -euo pipefail
APPROOT="$HOME/Library/Application Support/DineroCoin/Dinero All-in-One"
RPC=$(jq -r '.rpc.url' "$APPROOT/current-nodeinfo.json")
COOKIE=$(sed -e 's/^__cookie__://' -e 's/[\r\n]//g' "$APPROOT/data/mainnet/.cookie")

resp=$(curl -sS --fail \
  -u "__cookie__:$COOKIE" \
  -H 'Content-Type: application/json' \
  --data-binary @"-" "$RPC")

# keep only the last {...} block (robust against stray log lines)
python3 - "$resp" <<'PY'
import sys, re, json
s = sys.argv[1]
m = re.search(r'{[\s\S]*}\s*$', s)
if not m: sys.exit(2)
print(m.group(0))
PY
