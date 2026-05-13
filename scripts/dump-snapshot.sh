#!/usr/bin/env bash
# dump-snapshot.sh — Generate a UTXO snapshot for bundling in a release.
#
# Usage: ./scripts/dump-snapshot.sh [output_dir]
#
# Dumps the current UTXO set, prints all values needed to add a new entry
# in src/consensus/assume_utxo.cpp.
#
# After running this:
#   1. Add the printed entry to AssumeUTXORegistry::snapshots_ in assume_utxo.cpp
#   2. Rebuild: cmake --build build --target dinerod -j$(nproc)
#   3. Bundle the .dat file in the GitHub release alongside the binary

set -euo pipefail

OUTDIR="${1:-$HOME/Desktop}"
RPCURL="http://127.0.0.1:20998/"
RPCUSER="${DINERO_RPCUSER:-dinero}"
RPCPASS="${DINERO_RPCPASS:-dinerodpi2026}"

rpc() {
    curl -s --user "$RPCUSER:$RPCPASS" --data-binary \
        "{\"jsonrpc\":\"1.0\",\"method\":\"$1\",\"params\":[$2]}" "$RPCURL"
}

echo "=== Dinero UTXO Snapshot Generator ==="

# Get current tip
HEIGHT=$(rpc getblockcount "" | python3 -c "import sys,json; print(json.load(sys.stdin)['result'])")
OUTFILE="$OUTDIR/utxo-snapshot-${HEIGHT}.dat"

echo "Chain tip: $HEIGHT"
echo "Output:    $OUTFILE"
echo ""

# Dump snapshot
echo "Dumping UTXO set..."
DUMP=$(rpc dumptxoutset "\"$OUTFILE\"")
COINS=$(echo "$DUMP" | python3 -c "import sys,json; r=json.load(sys.stdin)['result']; print(r['coins_written'])")
HASH=$(echo "$DUMP" | python3 -c "import sys,json; r=json.load(sys.stdin)['result']; print(r['base_hash'])")
BYTES=$(echo "$DUMP" | python3 -c "import sys,json; r=json.load(sys.stdin)['result']; print(r['bytes_written'])")

echo "  Coins written: $COINS"
echo "  Block hash:    $HASH"
echo "  File size:     $BYTES bytes"

# Hash the file
FILEHASH=$(sha256sum "$OUTFILE" | awk '{print $1}')
echo "  File SHA256:   $FILEHASH"

# Get chainwork
CHAINWORK=$(rpc getblockheader "\"$HASH\"" | python3 -c "
import sys, json
h = json.load(sys.stdin)['result']
cw = h['chainwork']
# strip 0x prefix, pad to 64 chars
cw = cw.lstrip('0x').lstrip('0X')
print(cw.zfill(64))
")
echo "  Chainwork:     $CHAINWORK"

DATE=$(date '+%b %Y')

echo ""
echo "=== Add this entry to src/consensus/assume_utxo.cpp ==="
echo ""
cat << ENTRY
    // Mainnet snapshot at height ${HEIGHT} (${DATE})
    // Generated via scripts/dump-snapshot.sh
    // File: utxo-snapshot-${HEIGHT}.dat (${BYTES} bytes)
    AssumeUTXOSnapshot(
        "${FILEHASH}", // snapshot file SHA256
        "${HASH}", // block hash at height ${HEIGHT}
        ${HEIGHT},$(printf '%*s' $((54 - ${#HEIGHT})) '')// height
        "${CHAINWORK}",  // chainwork
        ${COINS},$(printf '%*s' $((54 - ${#COINS})) '')// UTXO count
        "Mainnet height ${HEIGHT} (${DATE})"
    ),
ENTRY

echo ""
echo "=== Release checklist ==="
echo "  [ ] Add entry above to src/consensus/assume_utxo.cpp"
echo "  [ ] Rebuild: cmake --build build --target dinerod -j\$(nproc)"
echo "  [ ] Attach $OUTFILE to GitHub release"
echo "  [ ] Document in release notes: 'Load snapshot to skip IBD: dinerod --loadsnapshotatstartup utxo-snapshot-${HEIGHT}.dat'"
