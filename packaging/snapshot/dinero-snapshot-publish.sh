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

DATADIR=${DATADIR:-/var/lib/dinero}
DUMPDIR=$DATADIR/snapshots
PUBROOT=${PUBROOT:-/var/www/dinero-snapshots}
KEY=${KEY:-/etc/dinero-snapshot/ed25519.key}
CLI=("${CLI_BIN:-dinero-cli}" "-datadir=$DATADIR")
DINEROD=${DINEROD:-/usr/bin/dinerod}
GATE_PORT=26999
GATE_RPC=26998
GATE_TIMEOUT=1200
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
    OUT=$("${CLI[@]}" dumptxoutset "$DUMP_FILE") || fail "dumptxoutset RPC failed"
    printf '%s\n' "$OUT" > "$PENDING.tmp"
    mv "$PENDING.tmp" "$PENDING"
fi
FIELDS=$(python3 - "$PENDING" "$DUMP_FILE" <<'PYFIELDS'
import json,sys,struct
with open(sys.argv[1]) as f: d=json.load(f)
height=d['base_height']; block=d['base_hash']
assert type(height) is int and height >= 0
assert isinstance(block,str) and len(block)==64 and all(c in '0123456789abcdefABCDEF' for c in block)
with open(sys.argv[2],'rb') as f: header=f.read(8)
assert len(header)==8 and header[:4]==b'UXTO', 'invalid snapshot magic'
version=struct.unpack('<I',header[4:])[0]
assert version in (4,5), 'unsupported snapshot version'
print(height,block.lower(),version)
PYFIELDS
) || fail "invalid pending snapshot metadata/header"
read -r HEIGHT BASE_HASH FORMAT <<< "$FIELDS"
# Mainnet activation policy; this job is deliberately mainnet-only.
if [ "$HEIGHT" -ge 111000 ] && [ "$FORMAT" != 5 ]; then
    rm -f "$PENDING" "$DUMP_FILE"
    fail "base $HEIGHT requires v5; upgrade exporting daemon (received v$FORMAT)"
fi
CURRENT_HASH=$("${CLI[@]}" getblockhash "$HEIGHT" | tr -d '\"\r\n') || fail "base ancestry RPC failed"
if [ "$CURRENT_HASH" != "$BASE_HASH" ]; then
    rm -f "$PENDING" "$DUMP_FILE"
    fail "pending base left selected chain; discarded candidate, kept latest"
fi
if [ "$FORMAT" = 5 ]; then
    TIP=$("${CLI[@]}" getblockcount) || fail "tip RPC failed"
    [[ "$TIP" =~ ^[0-9]+$ ]] || fail "invalid tip RPC response"
    if [ "$TIP" -lt "$((HEIGHT + 288))" ]; then
        log "retaining v5 base=$HEIGHT; waiting for tip=$((HEIGHT + 288)) (now $TIP)"
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

elif [ "$FORMAT" != 5 ] || [ "$HEIGHT" -lt 111000 ]; then
    fail "legacy publication requires a signing key"
else
    log "publishing v5 without optional publisher signature; chain binding is mandatory"
fi

# ── 4. SELF-CHECK GATE: a throwaway node must load this snapshot ────────────
GATE_DIR=$(mktemp -d /tmp/dinero-snapgate.XXXXXX)
GATE_PID=""
trap '[ -z "$GATE_PID" ] || kill -9 "$GATE_PID" 2>/dev/null || true; rm -rf "$GATE_DIR"' EXIT
mkdir -p "$GATE_DIR/datadir"
cat > "$GATE_DIR/datadir/dinero.conf" <<EOF
sync-profile=ios_utreexo
assumeutxo_forward_connect=1
port=$GATE_PORT
rpcport=$GATE_RPC
portmap=0
listen=0
connect=127.0.0.1:20999
assumeutxo_snapshot=$DUMP_FILE
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
    if grep -q "Snapshot header validated: height=$HEIGHT" "$GATE_DIR/node.log" \
       && grep -q "Pass 2 complete" "$GATE_DIR/node.log"; then
        # Header binding + full UTXO/forest/shielded load succeeded — the
        # trust-critical part. Forward blocks depend on mainnet cadence, so
        # they are a bonus check, not a requirement.
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
    GHASH=$(dinero-cli -rpcport=$GATE_RPC -datadir="$GATE_DIR/datadir" getblockhash "$GTIP" 2>/dev/null | tr -d '"' || true)
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
