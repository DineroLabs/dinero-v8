#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DINEROD="$ROOT_DIR/build/dinerod"
DINEROCLI="$ROOT_DIR/build/dinero-cli"

if [[ ! -x "$DINEROD" || ! -x "$DINEROCLI" ]]; then
  echo "Missing build artifacts. Build with: cmake --build build --target dinerod -j8"
  exit 1
fi

TMPDIR="$(mktemp -d /tmp/dinero_pool_hardening_XXXXXX)"
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
POOLDB="$TMPDIR/pool/pool_accounting.sqlite"

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
  >"$LOGFILE" 2>&1 &
PID=$!

for _ in $(seq 1 90); do
  if "$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" pool.status >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

AUTH_OUT="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.authorizeworker "miner1.rig1" "din1pegrzhlug8ak32yd89fu2p8e6zl9kwd8ee6z5874xdalrsr2c6xmss6h8k0")"
ATOMIC_AUTH="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.authorizeworker "atomic.worker" "din1pegrzhlug8ak32yd89fu2p8e6zl9kwd8ee6z5874xdalrsr2c6xmss6h8k0")"
DEDUPE_AUTH="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.authorizeworker "dedupe.worker" "din1pegrzhlug8ak32yd89fu2p8e6zl9kwd8ee6z5874xdalrsr2c6xmss6h8k0")"

SHARE_OK="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.submitshare "miner1.rig1" "job1" 64 true false false "uid-1")"

SHARE_DUP="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.submitshare "miner1.rig1" "job1" 64 true false false "uid-1")"

UNKNOWN_OUT="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.submitshare "unknown.worker" "job-u" 64 true false false "uid-u")"

for _ in $(seq 1 30); do
  [[ -f "$POOLDB" ]] && break
  sleep 1
done

python3 - "$POOLDB" <<'PY'
import sqlite3
import sys

db = sqlite3.connect(sys.argv[1])
db.execute("""
CREATE TRIGGER fail_share_insert
BEFORE INSERT ON shares
BEGIN
  SELECT RAISE(ABORT, 'forced share insert failure');
END;
""")
db.commit()
db.close()
PY

ATOMIC_FAIL="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.submitshare "atomic.worker" "job-atomic" 64 true false false "uid-atomic" || true)"

python3 - "$POOLDB" <<'PY'
import sqlite3
import sys

db = sqlite3.connect(sys.argv[1])
db.execute("DROP TRIGGER IF EXISTS fail_share_insert;")
db.commit()
db.close()
PY

ATOMIC_RETRY="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.submitshare "atomic.worker" "job-atomic" 64 true false false "uid-atomic")"

python3 - "$POOLDB" <<'PY'
import sqlite3
import sys

db = sqlite3.connect(sys.argv[1])
db.execute("""
CREATE TRIGGER fail_share_dedupe_insert
BEFORE INSERT ON share_dedupe
BEGIN
  SELECT RAISE(ABORT, 'forced share dedupe reservation failure');
END;
""")
db.commit()
db.close()
PY

DEDUPE_FAIL="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.submitshare "dedupe.worker" "job-dedupe" 64 true false false "uid-dedupe" || true)"

python3 - "$POOLDB" <<'PY'
import sqlite3
import sys

db = sqlite3.connect(sys.argv[1])
db.execute("DROP TRIGGER IF EXISTS fail_share_dedupe_insert;")
db.commit()
db.close()
PY

DEDUPE_RETRY="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
  pool.submitshare "dedupe.worker" "job-dedupe" 64 true false false "uid-dedupe")"

RATE_LIMITED=0
for i in $(seq 1 120); do
  RES="$("$DINEROCLI" -datadir="$TMPDIR" -rpcport="$RPCPORT" \
    pool.submitshare "abusive.worker" "job-flood" 64 false false false "uid-flood-$i")"
  if python3 -c 'import json,sys; d=json.loads(sys.stdin.read()); print(1 if d.get("error",{}).get("code")==-32031 else 0)' <<<"$RES" | grep -q '^1$'; then
    RATE_LIMITED=1
    break
  fi
done

python3 - "$AUTH_OUT" "$ATOMIC_AUTH" "$DEDUPE_AUTH" "$SHARE_OK" "$SHARE_DUP" "$UNKNOWN_OUT" "$ATOMIC_FAIL" "$ATOMIC_RETRY" "$DEDUPE_FAIL" "$DEDUPE_RETRY" "$RATE_LIMITED" <<'PY'
import json
import sys

auth = json.loads(sys.argv[1])
atomic_auth = json.loads(sys.argv[2])
dedupe_auth = json.loads(sys.argv[3])
share_ok = json.loads(sys.argv[4])
share_dup = json.loads(sys.argv[5])
unknown = json.loads(sys.argv[6])
atomic_fail = json.loads(sys.argv[7])
atomic_retry = json.loads(sys.argv[8])
dedupe_fail = json.loads(sys.argv[9])
dedupe_retry = json.loads(sys.argv[10])
rate_limited = int(sys.argv[11])

assert auth.get("success") is True, auth
assert atomic_auth.get("success") is True, atomic_auth
assert dedupe_auth.get("success") is True, dedupe_auth
assert share_ok.get("success") is True and share_ok.get("status") == "valid", share_ok
assert share_dup.get("success") is True and share_dup.get("duplicate") is True, share_dup
assert unknown.get("error", {}).get("code") == -32032, unknown
assert atomic_fail.get("error", {}).get("code") == -32033, atomic_fail
assert atomic_retry.get("success") is True and atomic_retry.get("status") == "valid", atomic_retry
assert dedupe_fail.get("error", {}).get("code") == -32033, dedupe_fail
assert not dedupe_fail.get("duplicate", False), dedupe_fail
assert dedupe_retry.get("success") is True and dedupe_retry.get("status") == "valid", dedupe_retry
assert rate_limited == 1, "expected abusive worker to become rate-limited"

print("pool hardening smoke: PASS")
PY
