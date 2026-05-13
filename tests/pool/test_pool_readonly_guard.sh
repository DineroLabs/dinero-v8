#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="$ROOT_DIR/build/dinerod"
DINEROCLI="$ROOT_DIR/build/dinero-cli"

if [[ ! -x "$DINEROD" || ! -x "$DINEROCLI" ]]; then
  echo "Missing build artifacts. Build with: cmake --build build --target dinerod dinero-cli -j8"
  exit 1
fi

TMPDIR="$(mktemp -d /tmp/dinero_pool_readonly_XXXXXX)"
pick_free_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}
RPCPORT="${RPCPORT:-$(pick_free_port)}"
P2PPORT="${P2PPORT:-$(pick_free_port)}"
LOGFILE="$TMPDIR/dinerod.log"

cleanup() {
  if [[ -n "${PID:-}" ]] && kill -0 "$PID" 2>/dev/null; then
    "$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" stop >/dev/null 2>&1 || true
    sleep 1
    kill "$PID" >/dev/null 2>&1 || true
    wait "$PID" >/dev/null 2>&1 || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

"$DINEROD" \
  --datadir="$TMPDIR" \
  --rpcport="$RPCPORT" \
  --port="$P2PPORT" \
  --connect=0 \
  --listen=0 \
  --dnsseed=0 \
  --upnp=0 \
  --pool.accounting.enable=1 \
  --rpc-readonly \
  >"$LOGFILE" 2>&1 &
PID=$!

for _ in $(seq 1 90); do
  if "$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" pool.status >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

STATUS_OUT="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" pool.status)"
AUTH_OUT="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.authorizeworker "miner1.rig1" "din1pegrzhlug8ak32yd89fu2p8e6zl9kwd8ee6z5874xdalrsr2c6xmss6h8k0" 2>&1 || true)"
SHARE_OUT="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.submitshare "miner1.rig1" "job1" 64 true false false "uid-1" 2>&1 || true)"
CONFIG_OUT="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.setconfig "{\"pool_fee_percent\":1.0}" 2>&1 || true)"

python3 - "$STATUS_OUT" "$AUTH_OUT" "$SHARE_OUT" "$CONFIG_OUT" <<'PY'
import sys

status = sys.argv[1]
auth = sys.argv[2]
share = sys.argv[3]
config = sys.argv[4]

assert '"enabled" : true' in status or '"ready" : true' in status, status

for payload in (auth, share, config):
    assert "Error code: -32099" in payload, payload
    assert "admin-only" in payload, payload

print("pool readonly guard: PASS")
PY
