#!/bin/bash
#
# dinero-snapshot-publish — dump a fresh AssumeUTXO snapshot at the current
# tip, prove it loads (throwaway-node gate), sign the manifest, and publish
# atomically for app/desktop bootstrap downloads.
#
# A snapshot that fails ANY step never becomes visible: the previous "latest"
# keeps serving and this run exits non-zero (systemd journal shows why).
#
# Layout:
#   /var/lib/dinero/snapshots/          daemon-writable dump area (transient)
#   /var/www/dinero-snapshots/archive/  timestamped published sets (kept: 7)
#   /var/www/dinero-snapshots/latest -> archive/<ts>   (atomic symlink swap)
#   /etc/dinero-snapshot/ed25519.key    dedicated snapshot signing key
#
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SNAPSHOT_PROFILE=${SNAPSHOT_PROFILE:-compact}
case "$SNAPSHOT_PROFILE" in compact) SYNC_PROFILE=ios_utreexo;; full) SYNC_PROFILE=mac_fullblock;; *) echo "invalid SNAPSHOT_PROFILE" >&2; exit 2;; esac

DATADIR=${DATADIR:-/var/lib/dinero}
DUMPDIR=$DATADIR/snapshots
PUBROOT=${PUBROOT:-/var/www/dinero-snapshots}
KEY=${KEY:-/etc/dinero-snapshot/ed25519.key}
CLI=("${CLI_BIN:-dinero-cli}" "-datadir=$DATADIR")
DINEROD=${DINEROD:-/usr/bin/dinerod}
GATE_PORT=${GATE_PORT:-26999}
GATE_RPC=${GATE_RPC:-26998}
GATE_TIMEOUT=${GATE_TIMEOUT:-1200}
KEEP=7
TS=$(date -u +%Y%m%dT%H%M%SZ)
LOG_PREFIX="[snapshot-publish $TS]"

log() { echo "$LOG_PREFIX $*"; }
fail() { log "FAIL: $*"; exit 1; }

mkdir -p "$DUMPDIR" "$PUBROOT/archive"
chown dinero:dinero "$DUMPDIR" || true

# Only one dump / publication may manipulate the pending candidate at a time.
exec 9>"$DUMPDIR/publisher.lock"
flock -n 9 || { log "another publisher run is active"; exit 0; }

# Keep one candidate while waiting for burial. The daemon exports its current
# state, so repeatedly dumping the tip would never produce a buried candidate.
DUMP_FILE=$DUMPDIR/pending.dat
PENDING=$DUMPDIR/pending.json
if [ ! -f "$PENDING" ]; then
    rm -f "$DUMP_FILE"
    EXPORT_ARGS=(); [ "$SNAPSHOT_PROFILE" != compact ] || EXPORT_ARGS+=(compact)
    OUT=$("${CLI[@]}" dumptxoutset "$DUMP_FILE" "${EXPORT_ARGS[@]}") || fail "dumptxoutset RPC failed"
    printf '%s\n' "$OUT" > "$PENDING.tmp"
    mv "$PENDING.tmp" "$PENDING"
fi
FIELDS=$(python3 - "$SCRIPT_DIR" "$PENDING" "$DUMP_FILE" "$SNAPSHOT_PROFILE" <<'PYFIELDS'
import json,sys
sys.path.insert(0,sys.argv[1])
from snapshot_transport import inspect
def require(ok,message):
    if not ok: raise ValueError(message)
with open(sys.argv[2]) as f: d=json.load(f)
version,height,block,profile=inspect(sys.argv[3])
require(type(d['base_height']) is int and d['base_height']==height, 'RPC/header height mismatch')
require(isinstance(d['base_hash'],str) and d['base_hash'].lower()==block, 'RPC/header hash mismatch')
require(version!=6 or profile==sys.argv[4], 'exporter profile differs from publication profile')
print(height,block,version,profile)
PYFIELDS
) || { rm -f "$PENDING" "$DUMP_FILE"; fail "invalid pending snapshot metadata/header/profile"; }
read -r HEIGHT BASE_HASH FORMAT FILE_PROFILE <<< "$FIELDS"
CURRENT_HASH=$("${CLI[@]}" getblockhash "$HEIGHT" | tr -d '\"\r\n') || fail "base ancestry RPC failed"
if [ "$CURRENT_HASH" != "$BASE_HASH" ]; then
    rm -f "$PENDING" "$DUMP_FILE"
    fail "pending base left selected chain; discarded candidate, kept latest"
fi
if [ "$FORMAT" -ge 5 ]; then
    TIP=$("${CLI[@]}" getblockcount) || fail "tip RPC failed"
    [[ "$TIP" =~ ^[0-9]+$ ]] || fail "invalid tip RPC response"
    if [ "$TIP" -lt "$((HEIGHT + 288))" ]; then
        log "retaining v$FORMAT base=$HEIGHT; waiting for tip=$((HEIGHT + 288)) (now $TIP)"
        exit 0
    fi
fi
# These checks schedule publication; the fresh-node loader below remains the
# authority for proof validation, selected-chain ancestry and burial.
BYTES=$(stat -c%s "$DUMP_FILE")
SHA=$(sha256sum "$DUMP_FILE" | awk '{print $1}')
log "candidate height=$HEIGHT format=v$FORMAT hash=$BASE_HASH bytes=$BYTES sha=$SHA"

# ── 2. Manifest (same shape as the app-bundled manifest) ────────────────────
MANIFEST=$DUMPDIR/snap-$TS.manifest.json
cat > "$MANIFEST" <<EOF
{
  "snapshot": {
    "sha256": "$SHA",
    "height": $HEIGHT,
    "block_hash": "$BASE_HASH",
    "bytes": $BYTES,
    "snapshot_file": "snapshot.dat"
  },
  "network": "mainnet",
  "generated_at": "$TS",
  "format_version": $FORMAT,
  "snapshot_profile": "$FILE_PROFILE",
  "source": "dinero-snapshot-publish (automated, self-check gated)"
}
EOF

# ── 3. Sign the manifest (ed25519, dedicated key) ───────────────────────────
SIG=$DUMPDIR/snap-$TS.manifest.sig
if [ -f "$KEY" ]; then
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$MANIFEST" -out "$SIG" \
    || fail "manifest signing failed"
# self-verify the signature before trusting the artifacts
openssl pkeyutl -verify -pubin \
    -inkey <(openssl pkey -in "$KEY" -pubout) \
    -rawin -in "$MANIFEST" -sigfile "$SIG" >/dev/null \
    || fail "signature self-verification failed"

elif [ "$SNAPSHOT_PROFILE" = compact ] || [ "$FORMAT" -lt 5 ] || [ "$HEIGHT" -lt 111000 ]; then
    fail "phone/legacy publication requires a signing key"
else
    log "publishing v$FORMAT without optional publisher signature; chain binding is mandatory"
fi

# ── 4. SELF-CHECK GATE: a throwaway node must load this snapshot ────────────
GATE_DIR=$(mktemp -d /tmp/dinero-snapgate.XXXXXX)
GATE_PID=""
trap '[ -z "$GATE_PID" ] || kill -9 "$GATE_PID" 2>/dev/null || true; rm -rf "$GATE_DIR"' EXIT
mkdir -p "$GATE_DIR/datadir"
cat > "$GATE_DIR/datadir/dinero.conf" <<EOF
sync-profile=$SYNC_PROFILE
utreexo_bridge=0
assumeutxo_forward_connect=1
port=$GATE_PORT
rpcport=$GATE_RPC
portmap=0
listen=0
connect=127.0.0.1:20999
assumeutxo_snapshot=$DUMP_FILE
assumeutxo_manifest=$MANIFEST
assumeutxo_require_manifest=1
EOF
"$DINEROD" --datadir="$GATE_DIR/datadir" > "$GATE_DIR/node.log" 2>&1 &
GATE_PID=$!
log "gate node started (pid $GATE_PID), waiting up to ${GATE_TIMEOUT}s"

DEADLINE=$(( $(date +%s) + GATE_TIMEOUT ))
GATE_OK=0
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    kill -0 $GATE_PID 2>/dev/null || break
    if grep -qE "rejected|checksum mismatch|refusing partial|FATAL" "$GATE_DIR/node.log"; then
        break
    fi
    # RPC state is shared by legacy and v6 loaders. Log text is not an import receipt.
    if "${CLI[0]}" -rpcport="$GATE_RPC" -datadir="$GATE_DIR/datadir" getbackgroundvalidationprogress > "$GATE_DIR/state.json" 2>/dev/null \
       && python3 - "$GATE_DIR/state.json" "$HEIGHT" "$BASE_HASH" <<'PYGATE'
import json,sys
with open(sys.argv[1]) as f: state=json.load(f)
if (state.get('assumeutxo_active') is not True or
    type(state.get('snapshot_base_height')) is not int or
    state['snapshot_base_height']!=int(sys.argv[2]) or
    state.get('snapshot_base_block')!=sys.argv[3] or
    state.get('status')=='Failed' or state.get('error_message')):
    raise ValueError('native gate has not admitted the expected snapshot base')
PYGATE
    then
        GATE_OK=1
        break
    fi
    sleep 10
done
if [ "$GATE_OK" != "1" ]; then
    log "gate log tail:"; tail -15 "$GATE_DIR/node.log" || true
    fail "self-check gate did not validate + reach base height within ${GATE_TIMEOUT}s"
fi
# Bonus parity check when the gate connected any forward block
GTIP=$(grep -oE "Connecting block: height=[0-9]+" "$GATE_DIR/node.log" | tail -1 | grep -oE "[0-9]+$" || true)
if [ -n "$GTIP" ]; then
    GHASH=$("${CLI[0]}" -rpcport=$GATE_RPC -datadir="$GATE_DIR/datadir" getblockhash "$GTIP" 2>/dev/null | tr -d '"' || true)
    MHASH=$("${CLI[@]}" getblockhash "$GTIP" 2>/dev/null | tr -d '"' || true)
    if [ -n "$GHASH" ] && [ -n "$MHASH" ] && [ "$GHASH" != "$MHASH" ]; then
        fail "gate/main hash mismatch at $GTIP: gate=$GHASH main=$MHASH"
    fi
fi
log "gate PASSED: snapshot header-validated + fully loaded${GTIP:+, forward at $GTIP}"
kill -TERM $GATE_PID 2>/dev/null || true
for _ in $(seq 1 30); do kill -0 $GATE_PID 2>/dev/null || break; sleep 1; done
kill -9 $GATE_PID 2>/dev/null || true
wait "$GATE_PID" 2>/dev/null || true
GATE_PID=""

# Recheck ancestry after the potentially long native validation gate.
CURRENT_HASH=$("${CLI[@]}" getblockhash "$HEIGHT" | tr -d '\"\r\n') || fail "final ancestry RPC failed"
[ "$CURRENT_HASH" = "$BASE_HASH" ] || { rm -f "$PENDING" "$DUMP_FILE"; fail "base left selected chain during gate"; }

# ── 5. Publish atomically ───────────────────────────────────────────────────
SET_DIR=$PUBROOT/archive/$TS
mkdir -p "$SET_DIR"
cp "$DUMP_FILE"  "$SET_DIR/snapshot.dat"
cp "$MANIFEST"   "$SET_DIR/manifest.json"
if [ -f "$SIG" ]; then cp "$SIG" "$SET_DIR/manifest.sig"; fi
chmod -R o+rX "$SET_DIR"
ln -sfn "archive/$TS" "$PUBROOT/latest.tmp"
mv -Tf "$PUBROOT/latest.tmp" "$PUBROOT/latest"
log "published: $SET_DIR -> latest"

# ── 6. Retention: keep last $KEEP sets; clean dump area ─────────────────────
ls -1d "$PUBROOT"/archive/*/ 2>/dev/null | sort | head -n -$KEEP | while read -r old; do
    log "pruning $old"; rm -rf "$old"
done
rm -f "$PENDING" "$DUMP_FILE" "$MANIFEST" "$SIG"

log "OK height=$HEIGHT sha=$SHA"
