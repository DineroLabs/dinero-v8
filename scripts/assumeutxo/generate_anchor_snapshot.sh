#!/usr/bin/env bash
# Generate a release-grade AssumeUTXO snapshot at a fixed anchor height.
#
# This script intentionally mutates the connected datadir by invalidating
# target_height + 1 so the active tip rewinds to the anchor before export.
# Run it only against a disposable datadir copy with networking disabled.
#
# Example:
#   DINERO_DATADIR=/tmp/dinero-anchor-13000 \
#   DINERO_RPCPORT=31998 \
#   scripts/assumeutxo/generate_anchor_snapshot.sh 13000 \
#     0000006f34bdfd52f0d61556175a3ccec56fc57428a1b04f7e012ee7e245c8a3 \
#     /tmp/dinero-snapshots

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <height> <expected_block_hash> <output_dir>" >&2
  exit 64
fi

HEIGHT="$1"
EXPECTED_HASH="$2"
OUTDIR="$3"

DINERO_CLI="${DINERO_CLI:-dinero-cli}"
DATADIR="${DINERO_DATADIR:-}"
RPCPORT="${DINERO_RPCPORT:-20998}"
UNDO_AUDIT_BLOCKS="${DINERO_UNDO_AUDIT_BLOCKS:-1024}"

if [[ -z "$DATADIR" ]]; then
  echo "DINERO_DATADIR must point at a disposable datadir copy" >&2
  exit 64
fi

case "$DATADIR" in
  /var/lib/dinero|/var/lib/dinero/|"$HOME/.dinero"|"$HOME/.dinero/"|/root/.dinero|/root/.dinero/)
    echo "refusing to mutate live-looking datadir: $DATADIR" >&2
    exit 65
    ;;
esac

mkdir -p "$OUTDIR"

rpc_json() {
  "$DINERO_CLI" -datadir="$DATADIR" -rpcport="$RPCPORT" "$@"
}

rpc_scalar() {
  rpc_json "$@" | python3 -c 'import json,sys
s=sys.stdin.read().strip()
try:
    print(json.loads(s))
except Exception:
    print(s)'
}

json_field() {
  python3 -c 'import json,sys; data=json.load(sys.stdin); path=sys.argv[1].split(".");
cur=data
for key in path:
    cur=cur[key]
print(cur)' "$1"
}

file_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

echo "== Anchor snapshot =="
echo "datadir: $DATADIR"
echo "rpcport: $RPCPORT"
echo "height:  $HEIGHT"
echo "hash:    $EXPECTED_HASH"

TIP="$(rpc_scalar getblockcount)"
if (( TIP < HEIGHT )); then
  echo "tip $TIP is below requested anchor $HEIGHT" >&2
  exit 66
fi

ACTUAL_HASH="$(rpc_scalar getblockhash "$HEIGHT")"
if [[ "$ACTUAL_HASH" != "$EXPECTED_HASH" ]]; then
  echo "anchor hash mismatch at height $HEIGHT" >&2
  echo "expected: $EXPECTED_HASH" >&2
  echo "actual:   $ACTUAL_HASH" >&2
  exit 67
fi

if (( TIP > HEIGHT )); then
  NEXT_HEIGHT=$((HEIGHT + 1))
  NEXT_HASH="$(rpc_scalar getblockhash "$NEXT_HEIGHT")"
  echo "rewinding disposable datadir by invalidating h=$NEXT_HEIGHT $NEXT_HASH"
  rpc_json invalidateblock "$NEXT_HASH" >/dev/null
fi

TIP_AFTER="$(rpc_scalar getblockcount)"
HASH_AFTER="$(rpc_scalar getblockhash "$HEIGHT")"
if [[ "$TIP_AFTER" != "$HEIGHT" || "$HASH_AFTER" != "$EXPECTED_HASH" ]]; then
  echo "rewind failed: tip=$TIP_AFTER hash=$HASH_AFTER" >&2
  exit 68
fi

HEADER_JSON="$(rpc_json getblockheader "$EXPECTED_HASH")"
CHAINWORK="$(printf '%s' "$HEADER_JSON" | json_field "chainwork")"
UTREEXO_ROOT="$(printf '%s' "$HEADER_JSON" | json_field "utreexo_root")"

UNDO_AUDIT_JSON="$(rpc_json auditundometadata "$UNDO_AUDIT_BLOCKS" false false)"
UNDO_FAILED="$(printf '%s' "$UNDO_AUDIT_JSON" | json_field "failed")"
UNDO_RESTAMPABLE="$(printf '%s' "$UNDO_AUDIT_JSON" | json_field "restampable")"
if [[ "$UNDO_FAILED" != "0" || "$UNDO_RESTAMPABLE" != "0" ]]; then
  echo "undo audit is not clean for anchor datadir" >&2
  echo "$UNDO_AUDIT_JSON" >&2
  exit 69
fi

SNAPSHOT="$OUTDIR/utxo-snapshot-${HEIGHT}.dat"
DUMP_JSON="$(rpc_json dumptxoutset "$SNAPSHOT")"
COINS="$(printf '%s' "$DUMP_JSON" | json_field "coins_written")"
BASE_HASH="$(printf '%s' "$DUMP_JSON" | json_field "base_hash")"
BYTES="$(printf '%s' "$DUMP_JSON" | json_field "bytes_written")"

if [[ "$BASE_HASH" != "$EXPECTED_HASH" ]]; then
  echo "snapshot base hash mismatch: $BASE_HASH" >&2
  exit 70
fi

SNAPSHOT_SHA256="$(file_sha256 "$SNAPSHOT")"
MANIFEST="$OUTDIR/utxo-snapshot-${HEIGHT}.manifest.json"

cat > "$MANIFEST" <<EOF
{
  "network": "mainnet",
  "height": $HEIGHT,
  "block_hash": "$EXPECTED_HASH",
  "chainwork": "$CHAINWORK",
  "utreexo_root": "$UTREEXO_ROOT",
  "snapshot_file": "$(basename "$SNAPSHOT")",
  "snapshot_sha256": "$SNAPSHOT_SHA256",
  "snapshot_bytes": $BYTES,
  "utxo_count": $COINS,
  "undo_audit": {
    "scanned": $UNDO_AUDIT_BLOCKS,
    "failed": $UNDO_FAILED,
    "restampable": $UNDO_RESTAMPABLE
  },
  "format": "dinero-utxo-snapshot-v3-with-utreexo-forest",
  "generated_at_utc": "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
}
EOF

cat <<EOF

== Snapshot complete ==
snapshot: $SNAPSHOT
manifest: $MANIFEST
sha256:   $SNAPSHOT_SHA256
utxos:    $COINS
bytes:    $BYTES
utreexo:  $UTREEXO_ROOT

Add this entry to src/consensus/assume_utxo.cpp after the artifact is published:

    AssumeUTXOSnapshot(
        "$SNAPSHOT_SHA256",
        "$EXPECTED_HASH",
        $HEIGHT,
        "$CHAINWORK",
        $COINS,
        "Mainnet height $HEIGHT v1 trust anchor"
    ),
EOF
