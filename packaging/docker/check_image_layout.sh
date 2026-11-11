#!/bin/sh
# Run inside the built image. Keep this script in source so shell quoting in
# docker-publish.yml cannot split the checks across host-side argv entries.
set -eu

require_file() {
    if [ ! -f "$1" ]; then
        echo "Docker image is missing required snapshot input: $1" >&2
        exit 1
    fi
}

require_file "$DINERO_SNAPSHOT"
require_file "$DINERO_SNAPSHOT.manifest.json"
require_file "$DINERO_SNAPSHOT.publisher.manifest.json"
require_file "$DINERO_SNAPSHOT.publisher.manifest.sig"
receipt="$DINERO_SNAPSHOT.publisher.receipt.json"
require_file "$receipt"
if ! grep -Fq '"publisher_signature_verified": true' "$receipt"; then
    echo 'Docker image publisher signature receipt is not verified' >&2
    exit 1
fi

count=0
remaining=$DINERO_SNAPSHOT_FALLBACKS
while :; do
    case "$remaining" in
        *';'*) fallback=${remaining%%;*}; remaining=${remaining#*;} ;;
        *) fallback=$remaining; remaining= ;;
    esac
    if [ -z "$fallback" ]; then
        echo 'Docker image has an empty recovery fallback' >&2
        exit 1
    fi
    require_file "$fallback"
    require_file "$fallback.manifest.json"
    count=$((count + 1))
    [ -n "$remaining" ] || break
done
if [ "$count" -ne 4 ]; then
    echo "Docker image has $count recovery fallbacks; expected four" >&2
    exit 1
fi
