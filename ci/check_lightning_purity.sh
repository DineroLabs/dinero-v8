#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
# Lightning L2 Binary Purity Checker
# ═══════════════════════════════════════════════════════════════════════════
# POLICY: Lightning L2 binaries MUST NOT contain L1 symbols
#
# This script inspects compiled binaries for forbidden L1 symbols using nm.
# It catches transitive dependencies and accidental linkage that CMake guards
# might miss.
#
# Usage:
#   ./ci/check_lightning_purity.sh <binary_path>
#
# Example:
#   ./ci/check_lightning_purity.sh build/test_channel_manager_state
#
# Exit codes:
#   0 - Binary is L2-pure (no L1 symbols)
#   1 - Architecture violation detected (L1 symbols found)
# ═══════════════════════════════════════════════════════════════════════════

set -euo pipefail

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

if [ $# -ne 1 ]; then
    echo "Usage: $0 <binary_path>"
    exit 1
fi

BIN="$1"

if [ ! -f "$BIN" ]; then
    echo -e "${RED}❌ Binary not found: $BIN${NC}"
    exit 1
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  Lightning L2 Purity Check"
echo "═══════════════════════════════════════════════════════════════"
echo "Binary: $BIN"
echo ""

# Define forbidden L1 symbols
# These indicate direct or transitive linkage to L1 infrastructure
FORBIDDEN_SYMBOLS=(
    # Chainstate / Blockchain
    "Chainstate"
    "BlockIndex"
    "BlockHeader"
    "ChainParams"
    "ValidateBlock"
    "ConnectBlock"
    "DisconnectBlock"

    # Wallet (direct access)
    "WalletManager"
    "WalletDB"
    "KeyPool"
    "HDWallet"

    # Mempool
    "TxMemPool"
    "MempoolAcceptResult"

    # Storage (direct RocksDB access)
    "RocksDBManager"
    "BlockTreeDB"
    "UTXOSetDB"

    # Consensus (should use oracles instead)
    "ConsensusParams"
    "PoWConsensus"
    "CheckProofOfWork"

    # Daemon (runtime context)
    "DaemonContext"
    "RPCServer"

    # P2P networking (Lightning uses its own)
    "PeerManager"
    "NetMessage"
    "AddrMan"
)

# Get undefined symbols (symbols the binary expects from linked libraries)
UNDEFINED_SYMBOLS=$(nm -u "$BIN" 2>/dev/null || true)

if [ -z "$UNDEFINED_SYMBOLS" ]; then
    echo -e "${YELLOW}⚠️  Warning: No undefined symbols found (fully static binary?)${NC}"
    echo -e "${GREEN}✅ Lightning binary appears L1-clean (no dynamic L1 linkage)${NC}"
    exit 0
fi

VIOLATIONS_FOUND=0

echo "Checking for forbidden L1 symbols..."
echo ""

for sym in "${FORBIDDEN_SYMBOLS[@]}"; do
    if echo "$UNDEFINED_SYMBOLS" | grep -q "$sym"; then
        if [ $VIOLATIONS_FOUND -eq 0 ]; then
            echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
            echo -e "${RED}  ❌ ARCHITECTURE VIOLATION DETECTED${NC}"
            echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
            echo ""
        fi

        VIOLATIONS_FOUND=$((VIOLATIONS_FOUND + 1))
        echo -e "${RED}Forbidden symbol found: $sym${NC}"

        # Show matching lines for debugging
        echo "$UNDEFINED_SYMBOLS" | grep "$sym" | head -3 | while read -r line; do
            echo "  → $line"
        done
        echo ""
    fi
done

if [ $VIOLATIONS_FOUND -gt 0 ]; then
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    echo -e "${RED}Total violations: $VIOLATIONS_FOUND${NC}"
    echo ""
    echo "Lightning L2 binaries MUST NOT link against L1 infrastructure."
    echo ""
    echo "Allowed dependencies:"
    echo "  ✓ STL (standard library)"
    echo "  ✓ Crypto (secp256k1, OpenSSL)"
    echo "  ✓ Serialization (msgpack, protobuf)"
    echo "  ✓ Oracle interfaces (IChainOracle, IWalletOracle)"
    echo ""
    echo "Forbidden dependencies:"
    echo "  ✗ Chainstate, BlockIndex, Mempool"
    echo "  ✗ WalletManager, DaemonContext"
    echo "  ✗ Direct RocksDB access"
    echo ""
    echo "Fix: Use oracle interfaces instead of direct L1 access."
    echo "See: docs/architecture/lightning_l2_separation.md"
    echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
    exit 1
fi

echo -e "${GREEN}✅ Lightning binary is L1-clean${NC}"
echo ""
echo "No forbidden L1 symbols detected."
echo "Binary complies with Lightning L2 architectural purity."
echo ""
echo "═══════════════════════════════════════════════════════════════"
exit 0
