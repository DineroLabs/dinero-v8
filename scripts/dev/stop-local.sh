#!/usr/bin/env bash
# Stop local Dinero daemon started by start-local.sh
set -euo pipefail

BIN="./build/bin/dinerod"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -b|--bin) BIN="$2"; shift 2;;
    -h|--help) echo "Usage: $0 [-b /path/to/dinerod]"; exit 0;;
    *) echo "Unknown arg: $1"; exit 2;;
  esac
done

pids=$(pgrep -f "$BIN" || true)
if [[ -z "$pids" ]]; then
  pids=$(pgrep -f dinerod || true)
fi

if [[ -z "$pids" ]]; then
  echo "No running dinerod found."
  exit 0
fi

echo "Stopping dinerod pids: $pids"
kill $pids 2>/dev/null || true
sleep 1
pids2=$(pgrep -f dinerod || true)
if [[ -n "$pids2" ]]; then
  echo "Force killing: $pids2"
  kill -9 $pids2 2>/dev/null || true
fi
echo "✅ Stopped."
