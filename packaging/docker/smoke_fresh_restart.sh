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
