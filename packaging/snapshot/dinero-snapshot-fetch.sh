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
# Verification layers (all mandatory, any failure exits non-zero and leaves
# no artifacts in --dest):
#   1. ed25519 signature over the manifest bytes (dedicated fleet snapshot
#      key, embedded below) — supply-chain gate.
#   2. sha256 of the payload against the signed manifest — transfer gate.
#   3. The daemon itself re-verifies the snapshot's in-file checksum and
#      binds its utreexo root to the PoW-verified header chain at load time —
#      the consensus gate. A forged snapshot cannot bind to real headers.
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
DEST="."
CONF=""

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

command -v curl >/dev/null 2>&1 || { echo "curl required" >&2; exit 2; }
command -v openssl >/dev/null 2>&1 || { echo "openssl required" >&2; exit 2; }

mkdir -p "$DEST"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/dinero-snapfetch.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
printf '%s\n' "$PUBKEY_PEM" > "$WORK/pub.pem"

OK=0
for MIRROR in "${MIRRORS[@]}"; do
    log "trying $MIRROR"
    if ! curl -fsS -m 15  -o "$WORK/manifest.json" "$MIRROR/manifest.json" \
       || ! curl -fsS -m 15 -o "$WORK/manifest.sig" "$MIRROR/manifest.sig"; then
        log "  manifest unavailable — next mirror"
        continue
    fi

    if ! openssl pkeyutl -verify -pubin -inkey "$WORK/pub.pem" -rawin \
            -in "$WORK/manifest.json" -sigfile "$WORK/manifest.sig" >/dev/null 2>&1; then
        log "  SIGNATURE INVALID — refusing this mirror"
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

    log "  manifest verified: height=$HEIGHT sha=${SHA:0:12}… — downloading payload"
    if ! curl -fsS -m 600 -o "$WORK/snapshot.dat" "$MIRROR/snapshot.dat"; then
        log "  payload download failed — next mirror"
        continue
    fi
    ACTUAL=$(sha256_of "$WORK/snapshot.dat")
    if [ "$ACTUAL" != "$SHA" ]; then
        log "  payload sha256 mismatch (got ${ACTUAL:0:12}…) — next mirror"
        continue
    fi

    OK=1
    break
done
[ "$OK" = "1" ] || fail "no mirror served a verifiable snapshot"

mv -f "$WORK/snapshot.dat"  "$DEST/snapshot.dat"
mv -f "$WORK/manifest.json" "$DEST/snapshot.dat.manifest.json"
log "verified snapshot at height $HEIGHT staged in $DEST"

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
