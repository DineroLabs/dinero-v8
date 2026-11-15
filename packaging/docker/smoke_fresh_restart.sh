#!/usr/bin/env bash
# Exercise the real image/daemon against an empty volume, then the same volume
# after a clean restart. No host RPC port is published.
set -euo pipefail

image="${1:?Docker image tag is required}"
volume="dinero-v823-smoke-${GITHUB_RUN_ID:-local}-$$"
container="dinero-v823-smoke-$$"
cleanup() {
    docker rm -f "$container" >/dev/null 2>&1 || true
    docker volume rm "$volume" >/dev/null 2>&1 || true
}
trap cleanup EXIT
docker volume create "$volume" >/dev/null

for pass in fresh restart; do
    docker run -d --name "$container" \
        --mount "type=volume,source=$volume,target=/data" "$image" >/dev/null
    ready=false
    for _ in $(seq 1 120); do
        if [ "$(docker inspect -f '{{.State.Running}}' "$container")" != true ]; then
            docker logs "$container" >&2 || true
            echo "Docker $pass daemon exited before RPC readiness" >&2
            exit 1
        fi
        if docker exec "$container" dinero-cli -datadir=/data getblockchaininfo >/dev/null 2>&1; then
            ready=true
            break
        fi
        sleep 1
    done
    if [ "$ready" != true ]; then
        docker logs "$container" >&2 || true
        echo "Docker $pass daemon did not reach RPC readiness" >&2
        exit 1
    fi
    # RPC can answer while the deferred snapshot bootstrap is still indexing
    # headers in a P2P callback. Stopping at that point waits for the callback
    # and can hit the daemon's 30-second shutdown watchdog. For this normal
    # start/restart smoke, require the signed base to be selected and usable
    # before exercising a clean stop. Interrupted-import recovery is a
    # separate lifecycle test.
    snapshot_ready=false
    for _ in $(seq 1 150); do
        if [ "$(docker inspect -f '{{.State.Running}}' "$container")" != true ]; then
            docker logs "$container" >&2 || true
            echo "Docker $pass daemon exited before snapshot readiness" >&2
            exit 1
        fi
        status_json="$(docker exec "$container" dinero-cli -datadir=/data getsnapshotbootstrapstatus 2>/dev/null || true)"
        chain_json="$(docker exec "$container" dinero-cli -datadir=/data getblockchaininfo 2>/dev/null || true)"
        if python3 - "$status_json" "$chain_json" <<'PY'
import json
import sys

try:
    status, chain = (json.loads(value) for value in sys.argv[1:3])
    status = status.get("result", status)
    chain = chain.get("result", chain)
    snapshot = status["snapshot_bootstrap"]
    # A post-base selected block proves the deferred import and header-index
    # callback have returned. services_ready can fluctuate while wallet
    # delivery catches up, so it is not a snapshot-readiness predicate.
    ready = (snapshot["assumeutxo_active"] is True and
             snapshot["fatal"] is False and
             snapshot["snapshot_base_height"] == 153051 and
             snapshot["snapshot_base_block"] ==
                 "00000012daae58a444ef6f76d524b648b8bdcc4f8fff7b2ddc66ecea378293e2" and
             chain["blocks"] > 153051)
except (ValueError, KeyError, TypeError, IndexError):
    ready = False
sys.exit(0 if ready else 1)
PY
        then
            base_hash="$(docker exec "$container" dinero-cli -datadir=/data getblockhash 153051 2>/dev/null || true)"
            if python3 - "$base_hash" <<'PY'
import json
import sys

try:
    value = json.loads(sys.argv[1])
    if isinstance(value, dict):
        value = value.get("result")
except (ValueError, IndexError):
    value = sys.argv[1].strip() if len(sys.argv) > 1 else ""
sys.exit(0 if value == "00000012daae58a444ef6f76d524b648b8bdcc4f8fff7b2ddc66ecea378293e2" else 1)
PY
            then
                snapshot_ready=true
                break
            fi
        fi
        sleep 2
    done
    if [ "$snapshot_ready" != true ]; then
        docker logs "$container" >&2 || true
        echo "Docker $pass daemon did not select and load the signed H153051 snapshot" >&2
        exit 1
    fi
    docker logs "$container" 2>&1 | grep -F 'assumeutxo_snapshot=yes assumeutxo_fallbacks=yes assumeutxo_forward_connect=yes' >/dev/null || {
        echo "Docker $pass did not arm the primary and recovery snapshots" >&2
        exit 1
    }
    docker stop --time 120 "$container" >/dev/null
    if [ "$(docker inspect -f '{{.State.ExitCode}}' "$container")" != 0 ]; then
        docker logs "$container" >&2 || true
        echo "Docker $pass daemon did not stop cleanly" >&2
        exit 1
    fi
    docker rm "$container" >/dev/null
    echo "Docker $pass daemon reached RPC readiness with primary and recovery snapshots armed"
done
