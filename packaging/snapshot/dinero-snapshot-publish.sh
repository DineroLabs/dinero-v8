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
SNAPSHOT_ROOT=$DATADIR/snapshots
# Keep the existing compact/legacy and H149999 capture path. Full v6 uses a
# separate pending namespace so neither profile can consume or delete the
# other's retained candidate while it waits for burial.
DUMPDIR=$SNAPSHOT_ROOT
if [ "$SNAPSHOT_PROFILE" = full ]; then DUMPDIR=$SNAPSHOT_ROOT/full; fi
PUBROOT=${PUBROOT:-/var/www/dinero-snapshots}
KEY=${KEY:-/etc/dinero-snapshot/ed25519.key}
CLI=("${CLI_BIN:-dinero-cli}" "-datadir=$DATADIR")
DINEROD=${DINEROD:-/usr/bin/dinerod}
GATE_PORT=${GATE_PORT:-26999}
GATE_RPC=${GATE_RPC:-26998}
GATE_WALLET=${GATE_WALLET:-27000}
GATE_TIMEOUT=${GATE_TIMEOUT:-1200}
KEEP=7
PIN_V5_HEIGHT=${SNAPSHOT_PIN_V5_HEIGHT:-149999}
TS=$(date -u +%Y%m%dT%H%M%SZ)
LOG_PREFIX="[snapshot-publish $TS]"

log() { echo "$LOG_PREFIX $*"; }
fail() { log "FAIL: $*"; exit 1; }

mkdir -p "$DUMPDIR" "$PUBROOT/archive"
chown dinero:dinero "$SNAPSHOT_ROOT" "$DUMPDIR" || true

# Serialize both profiles' exports and the exact-height boundary capture.
exec 9>"$SNAPSHOT_ROOT/publisher.lock"
flock -n 9 || { log "another publisher run is active"; exit 0; }

# Publishers using different datadirs do not share publisher.lock. Hold an
# endpoint lock through the final latest swap as well, so even an initially
# empty PUBROOT can be claimed by only one profile at a time.
exec 8>"$PUBROOT/.publisher.lock"
flock -w "${PUBROOT_LOCK_TIMEOUT:-1800}" 8 || fail "publication root is locked by another publisher"

# A full v6 artifact must never replace the phone endpoint's latest (or the
# reverse). Existing public v5 manifests predate snapshot_profile: that
# legacy endpoint is the compact/phone endpoint. A durable claim also binds a
# new empty root before its first latest symlink exists.
verify_endpoint_profile() {
    python3 - "$PUBROOT" "$SNAPSHOT_PROFILE" <<'PYENDPOINT' \
        || fail "endpoint profile collision or unreadable latest manifest; kept latest"
import json, os, pathlib, sys
try:
    root, requested = pathlib.Path(sys.argv[1]), sys.argv[2]
    marker = root / '.snapshot-profile'
    if marker.exists() or marker.is_symlink():
        if marker.is_symlink() or not marker.is_file() or marker.read_text() != requested + '\n':
            raise ValueError(f'publication root profile claim differs from {requested}')
    latest = root / 'latest'
    if latest.exists() or latest.is_symlink():
        with (latest / 'manifest.json').open() as stream:
            data = json.load(stream)
        if data.get('network') != 'mainnet' or type(data.get('format_version')) is not int:
            raise ValueError('latest is not a mainnet snapshot manifest')
        profile = data.get('snapshot_profile')
        if 4 <= data['format_version'] < 6 and profile in (None, 'legacy'):
            endpoint = 'compact'
        elif data['format_version'] == 6 and profile in ('compact', 'full'):
            endpoint = profile
        else:
            raise ValueError('latest has an unknown snapshot profile')
        if endpoint != requested:
            raise ValueError(f'latest belongs to {endpoint}, requested {requested}')
    if not marker.exists():
        # O_EXCL makes the claim atomic even if an uncooperative publisher
        # ignores .publisher.lock. An interrupted partial claim fails closed.
        fd = os.open(marker, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        try:
            os.write(fd, (requested + '\n').encode())
            os.fsync(fd)
        finally:
            os.close(fd)
        directory = os.open(root, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
except (OSError, ValueError, TypeError, AttributeError) as error:
    raise SystemExit(f'endpoint profile check refused: {error}')
PYENDPOINT
}
verify_endpoint_profile

# Keep one candidate while waiting for burial. The daemon exports its current
# state, so repeatedly dumping the tip would never produce a buried candidate.
DUMP_FILE=$DUMPDIR/pending.dat
PENDING=$DUMPDIR/pending.json
discard_pending_unless_boundary() {
    local reason=$1
    # A malformed metadata file or a reorg must never silently erase the
    # one-time H149999 export. Retain it for selected-chain review or recovery.
    if [ -f "$DUMP_FILE" ] && python3 - "$SCRIPT_DIR" "$DUMP_FILE" "$PIN_V5_HEIGHT" <<'PYBOUNDARY'
import sys
sys.path.insert(0,sys.argv[1])
from snapshot_transport import inspect
try:
    version,height,_,profile=inspect(sys.argv[2])
except (OSError,ValueError):
    raise SystemExit(1)
raise SystemExit(0 if (version,height,profile)==(5,int(sys.argv[3]),'legacy') else 1)
PYBOUNDARY
    then
        fail "$reason; retained exact v5 boundary payload for review"
    fi
    rm -f "$PENDING" "$DUMP_FILE"
}
if [ ! -f "$PENDING" ]; then
    # A process may have stopped after installing the exact H149999 payload
    # but before its metadata. Never erase an unpaired export automatically:
    # the boundary capture can validate and recover it after the tip advances.
    if [ -e "$DUMP_FILE" ] || [ -L "$DUMP_FILE" ]; then
        fail "unpaired pending payload; inspect or recover before exporting again"
    fi
    EXPORT_ARGS=(); [ "$SNAPSHOT_PROFILE" != compact ] || EXPORT_ARGS+=(compact)
    # A compact mainnet export can outlive the CLI's ordinary 30-second wait.
    OUT=$("${CLI[@]}" -rpcclienttimeout=900 dumptxoutset "$DUMP_FILE" "${EXPORT_ARGS[@]}") || fail "dumptxoutset RPC failed"
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
) || { discard_pending_unless_boundary "invalid pending metadata/header/profile"; fail "invalid pending snapshot metadata/header/profile"; }
read -r HEIGHT BASE_HASH FORMAT FILE_PROFILE <<< "$FIELDS"
CURRENT_HASH=$("${CLI[@]}" getblockhash "$HEIGHT" | tr -d '\"\r\n') || fail "base ancestry RPC failed"
if [ "$CURRENT_HASH" != "$BASE_HASH" ]; then
    discard_pending_unless_boundary "pending base left selected chain"
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

elif [ "$SNAPSHOT_PROFILE" = compact ] || [ "$FORMAT" -lt 5 ] || [ "$HEIGHT" -lt 111000 ] ||
     { [ "$FORMAT" = 5 ] && [ "$HEIGHT" = "$PIN_V5_HEIGHT" ]; }; then
    fail "phone/legacy publication requires a signing key"
else
    log "publishing v$FORMAT without optional publisher signature; chain binding is mandatory"
fi

# ── 4. SELF-CHECK GATE: a throwaway node must load this snapshot ────────────
GATE_DIR=$(mktemp -d /tmp/dinero-snapgate.XXXXXX)
GATE_PID=""
trap '[ -z "$GATE_PID" ] || kill -9 "$GATE_PID" 2>/dev/null || true; rm -rf "$GATE_DIR"' EXIT
mkdir -p "$GATE_DIR/datadir"
# The manifest names the public payload. The native loader checks that basename
# as well as its hash; pending.dat is only the publisher's private staging name.
cp "$DUMP_FILE" "$GATE_DIR/snapshot.dat"
cat > "$GATE_DIR/datadir/dinero.conf" <<EOF
sync-profile=$SYNC_PROFILE
utreexo-bridge=0
lightning.oracles.enable=0
assumeutxo_forward_connect=1
port=$GATE_PORT
rpcport=$GATE_RPC
portmap=0
listen=0
connect=127.0.0.1:20999
assumeutxo_snapshot=$GATE_DIR/snapshot.dat
assumeutxo_manifest=$MANIFEST
assumeutxo_require_manifest=1
EOF
"$DINEROD" --datadir="$GATE_DIR/datadir" --wallet-socket-port="$GATE_WALLET" 8>&- 9>&- > "$GATE_DIR/node.log" 2>&1 &
GATE_PID=$!
log "gate node started (pid $GATE_PID), waiting up to ${GATE_TIMEOUT}s"

DEADLINE=$(( $(date +%s) + GATE_TIMEOUT ))
GATE_OK=0
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    kill -0 $GATE_PID 2>/dev/null || break
    # RPC state is shared by legacy and v6 loaders. Ordinary startup metrics
    # include "blocks_rejected_total"; matching arbitrary log words aborts a
    # healthy gate before its headers arrive. Logs are not an import receipt.
    if "${CLI[0]}" -rpcport="$GATE_RPC" -datadir="$GATE_DIR/datadir" getbackgroundvalidationprogress > "$GATE_DIR/state.json" 2>/dev/null; then
      if python3 - "$GATE_DIR/state.json" "$HEIGHT" "$BASE_HASH" <<'PYGATE'
import json,sys
with open(sys.argv[1]) as f: state=json.load(f)
if state.get('status')=='Failed' or state.get('error_message'):
    raise SystemExit(2)
if (state.get('assumeutxo_active') is not True or
    type(state.get('snapshot_base_height')) is not int or
    state['snapshot_base_height']!=int(sys.argv[2]) or
    state.get('snapshot_base_block')!=sys.argv[3]):
    raise SystemExit(1)
PYGATE
      then
        GATE_OK=1
        break
      else
        GATE_STATUS=$?
        [ "$GATE_STATUS" -ne 2 ] || fail "native gate reported snapshot failure; kept latest"
      fi
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
[ "$CURRENT_HASH" = "$BASE_HASH" ] || { discard_pending_unless_boundary "base left selected chain during gate"; fail "base left selected chain during gate"; }

# ── 5. Publish atomically ───────────────────────────────────────────────────
verify_endpoint_profile
SET_DIR=$PUBROOT/archive/$TS
mkdir -p "$SET_DIR"
cp "$DUMP_FILE"  "$SET_DIR/snapshot.dat"
cp "$MANIFEST"   "$SET_DIR/manifest.json"
if [ -f "$SIG" ]; then cp "$SIG" "$SET_DIR/manifest.sig"; fi
chmod -R o+rX "$SET_DIR"
# The exact last pre-Orchard v5 base is a permanent recovery artifact. Pin it
# before exposing it via latest; normal seven-set archive rotation excludes it.
if [ "$FORMAT" = 5 ] && [ "$HEIGHT" = "$PIN_V5_HEIGHT" ]; then
    PIN_ROOT=$PUBROOT/pinned
    PIN_DIR=$PIN_ROOT/v5-h$HEIGHT
    mkdir -p "$PIN_ROOT"
    if [ -e "$PIN_DIR" ]; then
        python3 - "$PIN_DIR" "$HEIGHT" "$BASE_HASH" "$SHA" "$BYTES" <<'PYPIN' \
            || fail "existing permanent v5 boundary artifact differs or is incomplete"
import hashlib,json,pathlib,sys
root=pathlib.Path(sys.argv[1]);height=int(sys.argv[2]);block=sys.argv[3];sha=sys.argv[4];size=int(sys.argv[5])
data=json.loads((root/'manifest.json').read_text())
if data.get('format_version')!=5 or data.get('network')!='mainnet' or data.get('snapshot_profile')!='legacy':
    raise SystemExit('wrong pinned format, network or profile')
if data['snapshot']['height']!=height or data['snapshot']['block_hash']!=block:
    raise SystemExit('wrong pinned base')
if (data['snapshot']['sha256']!=sha or data['snapshot'].get('bytes')!=size or
    data['snapshot'].get('snapshot_file')!='snapshot.dat' or
    not (root/'manifest.sig').is_file()):
    raise SystemExit('incomplete pinned identity')
with (root/'snapshot.dat').open('rb') as stream:
    digest=hashlib.file_digest(stream,'sha256').hexdigest()
if digest!=sha:
    raise SystemExit('pinned payload hash differs')
PYPIN
        # Existence is not a signature check: a damaged retained signature
        # would leave the permanent recovery URL unusable by clients.
        openssl pkeyutl -verify -pubin \
            -inkey <(openssl pkey -in "$KEY" -pubout) \
            -rawin -in "$PIN_DIR/manifest.json" \
            -sigfile "$PIN_DIR/manifest.sig" >/dev/null \
            || fail "existing permanent v5 boundary signature is invalid"
    else
        PIN_TMP=$(mktemp -d "$PIN_ROOT/.v5-h$HEIGHT.XXXXXX")
        cp "$SET_DIR/snapshot.dat" "$SET_DIR/manifest.json" "$SET_DIR/manifest.sig" "$PIN_TMP/" \
            || fail "permanent v5 boundary copy failed"
        python3 - "$PIN_TMP" <<'PYSYNC' || fail "permanent v5 boundary durability sync failed"
import os,pathlib,sys
root=pathlib.Path(sys.argv[1])
for name in ('snapshot.dat','manifest.json','manifest.sig'):
    with (root/name).open('rb') as stream:
        os.fsync(stream.fileno())
fd=os.open(root,os.O_RDONLY)
try: os.fsync(fd)
finally: os.close(fd)
PYSYNC
        chmod -R o+rX "$PIN_TMP"
        mv -T "$PIN_TMP" "$PIN_DIR" || fail "permanent v5 boundary install failed"
        python3 - "$PIN_ROOT" <<'PYSYNC' || fail "permanent v5 boundary parent sync failed"
import os,sys
fd=os.open(sys.argv[1],os.O_RDONLY)
try: os.fsync(fd)
finally: os.close(fd)
PYSYNC
    fi
    log "pinned permanent v5 boundary: $PIN_DIR"
fi
ln -sfn "archive/$TS" "$PUBROOT/latest.tmp"
mv -Tf "$PUBROOT/latest.tmp" "$PUBROOT/latest"
log "published: $SET_DIR -> latest"

# ── 6. Retention: keep last $KEEP sets; clean dump area ─────────────────────
ls -1d "$PUBROOT"/archive/*/ 2>/dev/null | sort | head -n -$KEEP | while read -r old; do
    log "pruning $old"; rm -rf "$old"
done
rm -f "$PENDING" "$DUMP_FILE" "$MANIFEST" "$SIG"

log "OK height=$HEIGHT sha=$SHA"
