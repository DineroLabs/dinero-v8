#!/usr/bin/env bash
# Stage the recent desktop primary and all exact prior-lifecycle fallbacks.
#
# Usage: stage-snapshot-pair.sh DESTINATION_DIRECTORY
# Set DINERO_SKIP_SNAPSHOTS=1 only for an explicitly snapshot-free developer
# package. Official desktop packages must carry all four data/manifest pairs.

set -euo pipefail

DEST_DIR="${1:?destination directory required}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ "${DINERO_SKIP_SNAPSHOTS:-0}" == "1" ]]; then
    echo "WARNING: snapshot staging explicitly disabled (DINERO_SKIP_SNAPSHOTS=1)" >&2
    exit 0
fi

PRIMARY_NAME="dinero-assumeutxo-150521-v6-full.dat"
FALLBACK_NAME="dinero-assumeutxo-148375-v5.dat"
LEGACY_FALLBACK_NAME="dinero-assumeutxo-99677-v4.dat"
OLDEST_FALLBACK_NAME="utxo-snapshot-65300.dat"
PRIMARY_DATA="${DINERO_SNAPSHOT_DAT:-${PROJECT_ROOT}/packaging/linux/snapshot/${PRIMARY_NAME}}"
PRIMARY_MANIFEST="${DINERO_SNAPSHOT_MANIFEST:-${PRIMARY_DATA}.manifest.json}"
FALLBACK_DATA="${DINERO_SNAPSHOT_FALLBACK_DAT:-${PROJECT_ROOT}/packaging/linux/snapshot/${FALLBACK_NAME}}"
FALLBACK_MANIFEST="${DINERO_SNAPSHOT_FALLBACK_MANIFEST:-${FALLBACK_DATA}.manifest.json}"
LEGACY_FALLBACK_DATA="${DINERO_SNAPSHOT_LEGACY_FALLBACK_DAT:-${PROJECT_ROOT}/packaging/linux/snapshot/${LEGACY_FALLBACK_NAME}}"
LEGACY_FALLBACK_MANIFEST="${DINERO_SNAPSHOT_LEGACY_FALLBACK_MANIFEST:-${LEGACY_FALLBACK_DATA}.manifest.json}"
OLDEST_FALLBACK_DATA="${DINERO_SNAPSHOT_OLDEST_FALLBACK_DAT:-${PROJECT_ROOT}/packaging/linux/snapshot/${OLDEST_FALLBACK_NAME}}"
OLDEST_FALLBACK_MANIFEST="${DINERO_SNAPSHOT_OLDEST_FALLBACK_MANIFEST:-${OLDEST_FALLBACK_DATA}.manifest.json}"

verify_and_stage() {
    local data_path="$1"
    local manifest_path="$2"
    local installed_name="$3"

    [[ -f "$data_path" ]] || { echo "ERROR: required desktop snapshot missing: $data_path" >&2; exit 1; }
    [[ -f "$manifest_path" ]] || { echo "ERROR: required desktop snapshot manifest missing: $manifest_path" >&2; exit 1; }
    python3 - "$data_path" "$manifest_path" "$installed_name" <<'PY'
import hashlib
import json
import pathlib
import sys

data_path = pathlib.Path(sys.argv[1])
manifest_path = pathlib.Path(sys.argv[2])
installed_name = sys.argv[3]
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))["snapshot"]
payload = data_path.read_bytes()
actual_hash = hashlib.sha256(payload).hexdigest()
if manifest.get("snapshot_file") != installed_name:
    raise SystemExit(
        f"manifest snapshot_file={manifest.get('snapshot_file')!r}, expected {installed_name!r}"
    )
if manifest.get("sha256", "").lower() != actual_hash:
    raise SystemExit(
        f"snapshot sha256={actual_hash}, manifest={manifest.get('sha256')!r}"
    )
if int(manifest.get("bytes", -1)) != len(payload):
    raise SystemExit(
        f"snapshot bytes={len(payload)}, manifest={manifest.get('bytes')!r}"
    )
print(f"Verified {installed_name}: {len(payload)} bytes, sha256 {actual_hash}")
PY

    mkdir -p "$DEST_DIR"
    cp "$data_path" "$DEST_DIR/$installed_name"
    cp "$manifest_path" "$DEST_DIR/$installed_name.manifest.json"
}

FRESHNESS_ARGS=(--age-only)
if [[ -n "${DINERO_SNAPSHOT_TIP_HEIGHT:-}" ]]; then
    FRESHNESS_ARGS=(--tip-height "$DINERO_SNAPSHOT_TIP_HEIGHT")
fi
python3 "$PROJECT_ROOT/scripts/ci/check_snapshot_freshness.py" \
    --manifest "$PRIMARY_MANIFEST" "${FRESHNESS_ARGS[@]}"
python3 "$PROJECT_ROOT/packaging/snapshot/check_desktop_primary.py" \
    --payload "$PRIMARY_DATA" --manifest "$PRIMARY_MANIFEST" --expected-name "$PRIMARY_NAME" \
    --publisher-manifest "${PRIMARY_DATA}.publisher.manifest.json" \
    --publisher-signature "${PRIMARY_DATA}.publisher.manifest.sig"
verify_and_stage "$PRIMARY_DATA" "$PRIMARY_MANIFEST" "$PRIMARY_NAME"
verify_and_stage "$FALLBACK_DATA" "$FALLBACK_MANIFEST" "$FALLBACK_NAME"
verify_and_stage "$LEGACY_FALLBACK_DATA" "$LEGACY_FALLBACK_MANIFEST" "$LEGACY_FALLBACK_NAME"
verify_and_stage "$OLDEST_FALLBACK_DATA" "$OLDEST_FALLBACK_MANIFEST" "$OLDEST_FALLBACK_NAME"

for name in \
    "$PRIMARY_NAME" "$PRIMARY_NAME.manifest.json" \
    "$FALLBACK_NAME" "$FALLBACK_NAME.manifest.json" \
    "$LEGACY_FALLBACK_NAME" "$LEGACY_FALLBACK_NAME.manifest.json" \
    "$OLDEST_FALLBACK_NAME" "$OLDEST_FALLBACK_NAME.manifest.json"; do
    [[ -f "$DEST_DIR/$name" ]] || { echo "ERROR: staged snapshot artifact missing: $name" >&2; exit 1; }
done
echo "Staged verified full-v6 AssumeUTXO primary and three lifecycle fallbacks in $DEST_DIR"
