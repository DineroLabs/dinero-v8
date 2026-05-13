#!/usr/bin/env bash
#
# Stage an AssumeUTXO snapshot for DineroDPI bundling.
#
# Usage:
#   ./stage_ios_snapshot.sh /path/to/snapshot.dat [manifest.json] [target_dir]
#
# The snapshot is copied to a stable bundle filename so the iOS app can find it
# without per-release project edits.
#

set -euo pipefail

SNAPSHOT_PATH="${1:-}"
MANIFEST_PATH="${2:-}"
TARGET_DIR="${3:-/Users/haydarevich/Documents/DINERO/DineroDPI/DineroDPI/DineroDPI/Bootstrap}"
TARGET_SNAPSHOT_NAME="${TARGET_SNAPSHOT_NAME:-mainnet-snapshot.dat}"
TARGET_MANIFEST_NAME="${TARGET_MANIFEST_NAME:-mainnet-snapshot.dat.manifest.json}"
TARGET_LEGACY_METADATA_NAME="${TARGET_LEGACY_METADATA_NAME:-mainnet-snapshot.dat.json}"

if [ -z "$SNAPSHOT_PATH" ]; then
    echo "usage: $0 /path/to/snapshot.dat [manifest.json] [target_dir]" >&2
    exit 1
fi

if [ ! -f "$SNAPSHOT_PATH" ]; then
    echo "snapshot file not found: $SNAPSHOT_PATH" >&2
    exit 1
fi

LEGACY_METADATA_PATH=""
if [ -z "$MANIFEST_PATH" ]; then
    if [ -f "${SNAPSHOT_PATH}.manifest.json" ]; then
        MANIFEST_PATH="${SNAPSHOT_PATH}.manifest.json"
    elif [ -f "${SNAPSHOT_PATH}.json" ]; then
        LEGACY_METADATA_PATH="${SNAPSHOT_PATH}.json"
    fi
fi

mkdir -p "$TARGET_DIR"

TARGET_SNAPSHOT_PATH="$TARGET_DIR/$TARGET_SNAPSHOT_NAME"
TARGET_MANIFEST_PATH="$TARGET_DIR/$TARGET_MANIFEST_NAME"
TARGET_LEGACY_METADATA_PATH="$TARGET_DIR/$TARGET_LEGACY_METADATA_NAME"

cp "$SNAPSHOT_PATH" "$TARGET_SNAPSHOT_PATH"

if [ -n "$MANIFEST_PATH" ] && [ -f "$MANIFEST_PATH" ]; then
    cp "$MANIFEST_PATH" "$TARGET_MANIFEST_PATH"
elif [ -n "$LEGACY_METADATA_PATH" ] && [ -f "$LEGACY_METADATA_PATH" ]; then
    cp "$LEGACY_METADATA_PATH" "$TARGET_LEGACY_METADATA_PATH"
else
    echo "warning: no manifest found; DineroDPI will rely on legacy metadata synthesis if available" >&2
fi

echo "staged snapshot:"
echo "  $TARGET_SNAPSHOT_PATH"
if [ -f "$TARGET_MANIFEST_PATH" ]; then
    echo "staged manifest:"
    echo "  $TARGET_MANIFEST_PATH"
elif [ -f "$TARGET_LEGACY_METADATA_PATH" ]; then
    echo "staged legacy metadata:"
    echo "  $TARGET_LEGACY_METADATA_PATH"
fi
echo ""
echo "next:"
echo "  1. Build DineroDPI"
echo "  2. Reinstall the app"
echo "  3. Confirm Sync Cockpit reports snapshot bootstrap instead of genesis IBD"
