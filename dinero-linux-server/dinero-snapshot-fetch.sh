#!/bin/bash
#
# dinero-snapshot-fetch — download and verify the fleet's fresh AssumeUTXO
# snapshot for fast first-run bootstrap on ANY platform (Linux/macOS).
#
# The fleet publishes a daily, self-check-gated snapshot at the current tip
# (see EU1 dinero-snapshot-publish). A fresh node bootstrapping from it is
# minutes from tip instead of weeks of forward sync behind a release-bundled
# snapshot.
#
# v4 / pre-activation snapshots require the pinned publisher signature.
# Activated v5/v6 snapshots may be downloaded without it: only the daemon's
# mandatory selected-chain state-binding validation authorizes their import.
# Checksums detect transfer corruption; staging is NOT proof validation.
# Use --require-publisher-signature to additionally pin the fleet publisher.
# --mirror accepts an independently selected publisher; automatic discovery
# is not implemented by this script.
#
# Usage:
#   dinero-snapshot-fetch.sh [--dest DIR] [--conf FILE] [--mirror URL]
#     --dest DIR    where to place snapshot.dat + manifest.json (default: .)
#     --conf FILE   append/refresh `assumeutxo_snapshot=` in this dinero.conf
#     --mirror URL  override the mirror list (testing)
#
set -euo pipefail

MIRRORS=(
    "https://seed3.dinerolabs.org/snapshot"
    "https://seed.dinerolabs.org/snapshot"
    "https://seed2.dinerolabs.org/snapshot"
)
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DEST="."
CONF=""
REQUIRE_SIGNATURE=0

# Dedicated fleet snapshot-signing key (ed25519; generated on the fleet
# 2026-07-16, private key never leaves it). Matches the key embedded in the
# iOS app's SnapshotDownloadService.
PUBKEY_PEM="-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEAlFOdW71hOC7Q5Y0bYRLDvyFtv8ddU9Tf4a8dbr1Y4Yw=
-----END PUBLIC KEY-----"

while [ $# -gt 0 ]; do
    case "$1" in
        --dest)   DEST="$2"; shift 2 ;;
        --conf)   CONF="$2"; shift 2 ;;
        --mirror) MIRRORS=("$2"); shift 2 ;;
        --require-publisher-signature) REQUIRE_SIGNATURE=1; shift ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

log()  { echo "[snapshot-fetch] $*"; }
fail() { echo "[snapshot-fetch] FAIL: $*" >&2; rm -rf "$WORK"; exit 1; }

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

command -v python3 >/dev/null 2>&1 || { echo "python3 required" >&2; exit 2; }
command -v curl >/dev/null 2>&1 || { echo "curl required" >&2; exit 2; }
command -v openssl >/dev/null 2>&1 || { echo "openssl required" >&2; exit 2; }

mkdir -p "$DEST"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/dinero-snapfetch.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
printf '%s\n' "$PUBKEY_PEM" > "$WORK/pub.pem"

OK=0
for MIRROR in "${MIRRORS[@]}"; do
    log "trying $MIRROR"
    rm -f "$WORK/manifest.sig"
    if ! curl -fsS -m 15 -o "$WORK/manifest.json" "$MIRROR/manifest.json"; then
        log "  manifest unavailable — next mirror"
        continue
    fi
    SIGNATURE_OK=0
    if curl -fsS -m 15 -o "$WORK/manifest.sig" "$MIRROR/manifest.sig" \
       && openssl pkeyutl -verify -pubin -inkey "$WORK/pub.pem" -rawin \
            -in "$WORK/manifest.json" -sigfile "$WORK/manifest.sig" >/dev/null 2>&1; then
        SIGNATURE_OK=1
    fi
    if [ "$REQUIRE_SIGNATURE" = 1 ] && [ "$SIGNATURE_OK" != 1 ]; then
        log "  required publisher signature missing/invalid — next mirror"
        continue
    fi

    NETWORK=$(grep -o '"network"[^,}]*' "$WORK/manifest.json" | cut -d'"' -f4)
    HEIGHT=$(grep -o '"height"[^,}]*' "$WORK/manifest.json" | grep -oE '[0-9]+' | head -1)
    SHA=$(grep -o '"sha256"[^,}]*' "$WORK/manifest.json" | cut -d'"' -f4)
    BYTES=$(grep -o '"bytes"[^,}]*' "$WORK/manifest.json" | grep -oE '[0-9]+' | head -1)
    [ "$NETWORK" = "mainnet" ] || { log "  wrong network '$NETWORK' — next mirror"; continue; }
    [ -n "$HEIGHT" ] && [ -n "$SHA" ] && [ ${#SHA} -eq 64 ] || { log "  manifest unparseable — next mirror"; continue; }
    if [ -n "$BYTES" ] && { [ "$BYTES" -lt 1048576 ] || [ "$BYTES" -gt 536870912 ]; }; then
        log "  absurd size $BYTES — next mirror"; continue
    fi

    log "  manifest received (not chain-verified): height=$HEIGHT sha=${SHA:0:12}… — downloading payload"
    if ! curl -fsS -m 600 --max-filesize 536870912 -o "$WORK/snapshot.dat" "$MIRROR/snapshot.dat"; then
        log "  payload download failed — next mirror"
        continue
    fi
    ACTUAL=$(sha256_of "$WORK/snapshot.dat")
    if [ "$ACTUAL" != "$SHA" ]; then
        log "  payload sha256 mismatch (got ${ACTUAL:0:12}…) — next mirror"
        continue
    fi

    # Decide from the downloaded header, never a mirror's claimed format.
    # This is only a download gate: the daemon MUST verify the full v5/v6 proof,
    # state roots, selected-header ancestry and burial before importing state.
    FORMAT=$(python3 - "$SCRIPT_DIR" "$WORK/snapshot.dat" "$WORK/manifest.json" <<'PYHEADER'
import json,sys
sys.path.insert(0,sys.argv[1])
from snapshot_transport import inspect
version,height,block,profile=inspect(sys.argv[2])
with open(sys.argv[3]) as f: manifest=json.load(f)
snapshot=manifest['snapshot']
def require(ok,message):
    if not ok: raise ValueError(message)
require(type(snapshot['height']) is int and snapshot['height']==height, 'manifest/header height mismatch')
if 'block_hash' in snapshot: require(snapshot['block_hash'].lower()==block, 'manifest/header hash mismatch')
if 'format_version' in manifest: require(type(manifest['format_version']) is int and manifest['format_version']==version, 'manifest/header format mismatch')
if version==6: require(manifest.get('snapshot_profile')==profile, 'manifest/header profile mismatch')
print('bound' if version>=5 and height>=111000 else 'legacy')
PYHEADER
) || { log "  invalid format/activation header — next mirror"; continue; }
    if [ "$FORMAT" = legacy ] && [ "$SIGNATURE_OK" != 1 ]; then
        log "  legacy snapshot requires publisher signature — next mirror"
        continue
    fi
    log "  download checks passed; publisher_signature=$SIGNATURE_OK; daemon chain verification still required"

    OK=1
    break
done
[ "$OK" = "1" ] || fail "no mirror served a verifiable snapshot"

mv -f "$WORK/snapshot.dat"  "$DEST/snapshot.dat"
mv -f "$WORK/manifest.json" "$DEST/snapshot.dat.manifest.json"
log "snapshot download at height $HEIGHT staged in $DEST; awaiting daemon validation"

if [ -n "$CONF" ]; then
    SNAP_PATH=$(cd "$DEST" && pwd)/snapshot.dat
    if grep -q '^assumeutxo_snapshot=' "$CONF" 2>/dev/null; then
        sed -i.bak "s|^assumeutxo_snapshot=.*|assumeutxo_snapshot=$SNAP_PATH|" "$CONF"
    else
        printf 'assumeutxo_snapshot=%s\n' "$SNAP_PATH" >> "$CONF"
    fi
    log "dinero.conf updated: assumeutxo_snapshot=$SNAP_PATH"
else
    log "add to dinero.conf:  assumeutxo_snapshot=$(cd "$DEST" && pwd)/snapshot.dat"
fi
