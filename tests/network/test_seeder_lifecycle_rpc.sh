#!/usr/bin/env bash
# Copyright (c) 2026 The Dinero Developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Smoke test for dashboard-controlled peer seeding:
#   - seeder.status is present and initially stopped
#   - seeder.start rejects a missing binary immediately
#   - seeder.start launches the supplied dinero-seeder binary
#   - seeder.stop terminates it and reports stopped

set -euo pipefail

DINEROD="${DINEROD:?DINEROD must point to dinerod binary}"
DINERO_CLI="${DINERO_CLI:?DINERO_CLI must point to dinero-cli binary}"
DINERO_SEEDER="${DINERO_SEEDER:?DINERO_SEEDER must point to dinero-seeder binary}"

TMP="$(mktemp -d)"
HOME_DIR="$TMP/home"
mkdir -p "$HOME_DIR"

cleanup() {
    HOME="$HOME_DIR" "$DINERO_CLI" -rpcport=19142 seeder.stop >/dev/null 2>&1 || true
    HOME="$HOME_DIR" "$DINERO_CLI" -rpcport=19142 stop >/dev/null 2>&1 || true
    sleep 1
    rm -rf "$TMP"
}
trap cleanup EXIT

wait_rpc() {
    local deadline=$((SECONDS + 30))
    until HOME="$HOME_DIR" "$DINERO_CLI" -rpcport=19142 getnetworkinfo >/dev/null 2>&1; do
        [ "$SECONDS" -lt "$deadline" ] || {
            echo "FAIL: daemon did not become ready within 30s"
            exit 1
        }
        sleep 1
    done
}

HOME="$HOME_DIR" "$DINEROD" --regtest --rpcport=19142 --p2pport=19141 --listen -daemon >/dev/null
wait_rpc

HOME="$HOME_DIR" "$DINERO_CLI" -rpcport=19142 seeder.status | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["running"] is False, d
assert d["pid"] == 0, d
print("PASS: seeder.status initially stopped")
'

HOME="$HOME_DIR" "$DINERO_CLI" -rpcport=19142 seeder.start '{"binary":"/no/such/dinero-seeder"}' | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["error"]["code"] == -8, d
assert "not found" in d["error"]["message"], d
print("PASS: missing binary rejected")
'

START_JSON="$(mktemp)"
STOP_JSON="$(mktemp)"
HOME="$HOME_DIR" "$DINERO_CLI" -rpcport=19142 seeder.start \
    "{\"binary\":\"${DINERO_SEEDER}\",\"cycle_pause_seconds\":1,\"batch\":1}" > "$START_JSON"
sleep 1
HOME="$HOME_DIR" "$DINERO_CLI" -rpcport=19142 seeder.stop > "$STOP_JSON"

python3 - "$START_JSON" "$STOP_JSON" <<'PY'
import json, sys
start = json.load(open(sys.argv[1]))
stop = json.load(open(sys.argv[2]))
assert start["running"] is True, start
assert start["pid"] > 0, start
assert stop["running"] is False, stop
assert stop["pid"] == 0, stop
assert stop["state_path"].endswith("/seeder/peers.state"), stop
assert stop["output_path"].endswith("/seeder/seeds_observed.txt"), stop
print("PASS: seeder.start launches and seeder.stop terminates")
PY

echo "=== ALL ASSERTIONS PASS ==="
