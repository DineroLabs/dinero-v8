#!/usr/bin/env bash
# A failed foreground --reindex must exit after one attempt. The fresh-store
# bootstrap once ran the same maintenance command before the real DaemonApp,
# causing two full archive replays in one process (and retrying a bad archive).
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dinerod="${DINEROD:-${root_dir}/build/dinerod}"
if [[ ! -x "$dinerod" ]]; then
    printf 'dinerod not executable: %s\n' "$dinerod" >&2
    exit 1
fi

test_root="$(mktemp -d "${TMPDIR:-/tmp}/dinero-reindex-once.XXXXXX")"
trap 'rm -rf "$test_root"' EXIT

# No blk*.dat bodies is a fast, deterministic failure. Mainnet is intentional:
# the bug occurred in the fresh-store bootstrap for a new mainnet datadir.
for mode in --reindex --reindex-chainstate; do
    test_dir="$test_root/${mode#--}"
    mkdir "$test_dir"
    log="$test_dir/reindex.log"
    if "$dinerod" --datadir="$test_dir" "$mode" --p2p.offline=1 \
            --listen=0 --portmap=0 >"$log" 2>&1; then
        cat "$log" >&2
        printf '%s: empty archive unexpectedly reindexed successfully\n' "$mode" >&2
        exit 1
    fi

    attempts="$(awk '/REINDEX OPERATION REQUESTED/ { n++ } END { print n+0 }' "$log")"
    failures="$(awk '/Reindex failed:/ { n++ } END { print n+0 }' "$log")"
    if [[ "$attempts" != 1 || "$failures" != 1 ]]; then
        cat "$log" >&2
        printf '%s: expected one attempt and one failure; got %s attempts, %s failures\n' \
            "$mode" "$attempts" "$failures" >&2
        exit 1
    fi
    if ! awk '/\[FATAL\] Failed to initialize daemon/ { found=1 } END { exit !found }' "$log"; then
        cat "$log" >&2
        printf '%s: failed reindex did not abort daemon initialization\n' "$mode" >&2
        exit 1
    fi
done
echo 'ReindexSingleInvocation: PASS'
