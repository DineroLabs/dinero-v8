#!/usr/bin/env bash
#
# AssumeUTXO Snapshot Generation Script
#
# Purpose: Generate, verify, and sign UTXO snapshots for distribution
# Usage: ./generate_snapshot.sh [mainnet|testnet|regtest]
#
# IMPORTANT:
# - This is OPERATIONAL tooling, NOT consensus code
# - Snapshots are UNTRUSTED until operators verify them
# - This script does NOT auto-publish (staging only)
# - Operators may generate their own snapshots
#

set -euo pipefail

# Configuration
NETWORK="${1:-testnet}"
DATADIR="${DINERO_DATADIR:-$HOME/.dinero}"
OUTDIR="${SNAPSHOT_OUTDIR:-/var/lib/dinero/snapshots/$NETWORK}"
DINERO_CLI="${DINERO_CLI:-dinero-cli}"

# Derived paths
if [ "$NETWORK" != "mainnet" ]; then
    CLI_ARGS="--$NETWORK"
    DATADIR="$DATADIR/$NETWORK"
else
    CLI_ARGS=""
fi

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "═══════════════════════════════════════════════════════════════════════"
echo "  AssumeUTXO Snapshot Generation"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""
echo "  Network: $NETWORK"
echo "  Datadir: $DATADIR"
echo "  Output:  $OUTDIR"
echo ""

# Check if daemon is running
if ! $DINERO_CLI $CLI_ARGS getblockcount >/dev/null 2>&1; then
    echo -e "${RED}✗ Error: dinerod not running or not responding${NC}"
    echo ""
    echo "  Start dinerod first:"
    if [ "$NETWORK" = "mainnet" ]; then
        echo "    dinerod --daemon"
    else
        echo "    dinerod --$NETWORK --daemon"
    fi
    exit 1
fi

echo -e "${GREEN}✓ Daemon responding${NC}"
echo ""

# Check sync status
echo "[1] Checking sync status..."
BLOCKCOUNT=$($DINERO_CLI $CLI_ARGS getblockcount)
echo "  Current height: $BLOCKCOUNT"

# Check if in IBD
BLOCKCHAIN_INFO=$($DINERO_CLI $CLI_ARGS getblockchaininfo 2>/dev/null || echo "{}")
if echo "$BLOCKCHAIN_INFO" | grep -q "\"initialblockdownload\" : true"; then
    echo -e "${YELLOW}⚠️  Warning: Node is in Initial Block Download${NC}"
    echo "  Snapshot may be stale. Wait for full sync for production use."
    echo ""
    read -p "Continue anyway? [y/N] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

echo -e "${GREEN}✓ Node synced (height: $BLOCKCOUNT)${NC}"
echo ""

# Create output directory
echo "[2] Creating output directory..."
mkdir -p "$OUTDIR"
echo -e "${GREEN}✓ $OUTDIR${NC}"
echo ""

# Generate snapshot filename
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
TMP_FILE="/tmp/utxo-snapshot-$NETWORK-$$.dat"
FINAL_NAME="utxo-snapshot-$NETWORK-height-$BLOCKCOUNT-$TIMESTAMP.dat"
FINAL_PATH="$OUTDIR/$FINAL_NAME"

echo "[3] Generating snapshot..."
echo "  This may take several minutes for large UTXO sets..."
echo ""

if ! $DINERO_CLI $CLI_ARGS dumptxoutset "$TMP_FILE" > /tmp/dump_result_$$.json 2>&1; then
    echo -e "${RED}✗ Snapshot generation failed${NC}"
    cat /tmp/dump_result_$$.json
    rm -f /tmp/dump_result_$$.json
    exit 1
fi

# Display results
echo ""
cat /tmp/dump_result_$$.json
echo ""

# Extract metadata
COINS_WRITTEN=$(grep "coins_written" /tmp/dump_result_$$.json | sed 's/.*: \([0-9]*\).*/\1/')
BYTES_WRITTEN=$(grep "bytes_written" /tmp/dump_result_$$.json | sed 's/.*: \([0-9]*\).*/\1/')
BASE_HASH=$(grep "base_hash" /tmp/dump_result_$$.json | sed 's/.*: "\([^"]*\)".*/\1/')

echo -e "${GREEN}✓ Snapshot generated${NC}"
echo "  Coins: $COINS_WRITTEN"
echo "  Bytes: $BYTES_WRITTEN"
echo "  Base:  $BASE_HASH"
echo ""

# Move to final location
echo "[4] Moving to output directory..."
mv "$TMP_FILE" "$FINAL_PATH"
echo -e "${GREEN}✓ $FINAL_PATH${NC}"
echo ""

# Generate SHA256 checksum
echo "[5] Generating SHA256 checksum..."
cd "$OUTDIR"
sha256sum "$FINAL_NAME" > "$FINAL_NAME.sha256"
echo -e "${GREEN}✓ $FINAL_NAME.sha256${NC}"
cat "$FINAL_NAME.sha256"
echo ""

# Generate GPG signature (if available)
echo "[6] Generating GPG signature..."
if command -v gpg >/dev/null 2>&1; then
    if gpg --list-secret-keys >/dev/null 2>&1; then
        gpg --detach-sign --armor "$FINAL_NAME"
        echo -e "${GREEN}✓ $FINAL_NAME.asc${NC}"
        echo ""
        echo "  Signature info:"
        gpg --verify "$FINAL_NAME.asc" "$FINAL_NAME" 2>&1 | grep "Good signature\|using"
    else
        echo -e "${YELLOW}⚠️  No GPG secret key found${NC}"
        echo "  Run: gpg --gen-key"
        echo "  Skipping signature generation"
    fi
else
    echo -e "${YELLOW}⚠️  GPG not installed${NC}"
    echo "  Install GPG to sign snapshots"
fi
echo ""

# Create loader-compatible manifest file
echo "[7] Creating snapshot manifest..."
MANIFEST_FILE="$FINAL_NAME.manifest.json"
cat > "$MANIFEST_FILE" <<EOF
{
  "snapshot": {
    "sha256": "$(cut -d' ' -f1 < "$FINAL_NAME.sha256")",
    "height": $BLOCKCOUNT,
    "block_hash": "$BASE_HASH",
    "bytes": $BYTES_WRITTEN,
    "snapshot_file": "$FINAL_NAME"
  },
  "network": "$NETWORK",
  "generated_at": "$TIMESTAMP"
}
EOF

echo -e "${GREEN}✓ $MANIFEST_FILE${NC}"
echo ""

# Create metadata file
echo "[8] Creating metadata file..."
METADATA_FILE="$FINAL_NAME.json"
cat > "$METADATA_FILE" <<EOF
{
  "network": "$NETWORK",
  "height": $BLOCKCOUNT,
  "base_hash": "$BASE_HASH",
  "coins_written": $COINS_WRITTEN,
  "bytes_written": $BYTES_WRITTEN,
  "timestamp": "$TIMESTAMP",
  "generated_by": "dinero-cli dumptxoutset",
  "filename": "$FINAL_NAME",
  "sha256": "$(cat $FINAL_NAME.sha256 | cut -d' ' -f1)"
}
EOF

echo -e "${GREEN}✓ $METADATA_FILE${NC}"
echo ""

# Cleanup
rm -f /tmp/dump_result_$$.json

# Summary
echo "═══════════════════════════════════════════════════════════════════════"
echo "  ✓ SNAPSHOT GENERATION COMPLETE"
echo "═══════════════════════════════════════════════════════════════════════"
echo ""
echo "  Files created in $OUTDIR:"
echo "    - $FINAL_NAME             (snapshot data)"
echo "    - $FINAL_NAME.sha256      (checksum)"
echo "    - $MANIFEST_FILE          (loader manifest)"
if [ -f "$OUTDIR/$FINAL_NAME.asc" ]; then
echo "    - $FINAL_NAME.asc         (GPG signature)"
fi
echo "    - $FINAL_NAME.json        (metadata)"
echo ""
echo "  Next Steps:"
echo "    1. Verify checksum:    sha256sum -c $FINAL_NAME.sha256"
if [ -f "$OUTDIR/$FINAL_NAME.asc" ]; then
echo "    2. Verify signature:   gpg --verify $FINAL_NAME.asc $FINAL_NAME"
fi
echo "    3. Test load locally:  dinero-cli loadtxoutset $OUTDIR/$FINAL_NAME"
echo "    4. Publish manifest:   Keep $MANIFEST_FILE beside the snapshot"
echo "    5. Manual publish:     Upload to distribution server"
echo ""
echo "  ⚠️  IMPORTANT:"
echo "    - Do NOT auto-publish this snapshot"
echo "    - Verify integrity before distribution"
echo "    - Operators must verify checksums"
echo "    - Snapshots are untrusted acceleration"
echo ""
