/**
 * @file test_genesis_phase3.cpp
 * @brief Standalone test for Phase 3 genesis block initialization
 *
 * This test verifies:
 * 1. Genesis BlockHeader can be reconstructed from hardcoded constants
 * 2. Header size is exactly 128 bytes
 * 3. Reserved field is all zeros
 * 4. Computed hash matches expected hash (MANDATORY assertion)
 */

#include <iostream>
#include <iomanip>
#include "consensus/genesis_canonical.h"
#include "consensus/chainparams.h"

using namespace dinero;

int main() {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  PHASE 3 GENESIS BLOCK INITIALIZATION TEST                         ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";

    try {
        // Select mainnet parameters
        SelectParams(Chain::MAINNET);
        const auto& params = Params();

        std::cout << "  [1/5] Loading chain parameters...\n";
        std::cout << "        Network: " << params.name << "\n";
        std::cout << "        Genesis timestamp: " << params.genesis.nTime << "\n";
        std::cout << "        Genesis nonce: " << params.genesis.nNonce << "\n";
        std::cout << "\n";

        std::cout << "  [2/5] Building canonical genesis block...\n";
        auto genesis = BuildCanonicalGenesis(params);
        std::cout << "        ✓ Genesis block constructed\n";
        std::cout << "\n";

        // Verify header fields
        std::cout << "  [3/5] Verifying header fields...\n";
        std::cout << "        Version: " << genesis.header.version << "\n";
        std::cout << "        Timestamp: " << genesis.header.timestamp << " (2026-03-03 00:00:00 UTC)\n";
        std::cout << "        Difficulty: 0x" << std::hex << genesis.header.difficulty << std::dec << "\n";
        std::cout << "        Nonce: " << genesis.header.nonce << "\n";
        std::cout << "\n";

        // Verify serialization
        std::cout << "  [4/5] Verifying serialization...\n";
        auto header_bytes = genesis.header.SerializeForHash();
        std::cout << "        Header size: " << header_bytes.size() << " bytes";
        if (header_bytes.size() == 128) {
            std::cout << " ✓\n";
        } else {
            std::cout << " ✗ WRONG SIZE!\n";
            return 1;
        }

        // Verify reserved field
        bool reserved_valid = genesis.header.IsReservedValid();
        std::cout << "        Reserved field: ";
        if (reserved_valid) {
            std::cout << "all zeros ✓\n";
        } else {
            std::cout << "non-zero ✗ INVALID!\n";
            return 1;
        }
        std::cout << "\n";

        // Compute and verify hash
        std::cout << "  [5/5] Computing and verifying hash...\n";
        uint256 computed_hash = genesis.header.GetHash();
        std::cout << "        Computed hash:  " << computed_hash.GetHex() << "\n";
        std::cout << "        Expected hash:  " << genesis.hash_hex << "\n";

        if (computed_hash.GetHex() == genesis.hash_hex) {
            std::cout << "        ✓ Hash verification PASSED\n";
        } else {
            std::cout << "        ✗ Hash verification FAILED!\n";
            return 1;
        }
        std::cout << "\n";

        // Display full genesis block info
        std::cout << "╔════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  PHASE 3 GENESIS BLOCK SUMMARY                                     ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════════════╝\n";
        std::cout << "\n";
        std::cout << "  Genesis Hash:       " << genesis.hash_hex << "\n";
        std::cout << "  Merkle Root:        " << genesis.header.merkle_root.GetHex() << "\n";
        std::cout << "  Utreexo Root:       " << genesis.header.utreexo_root.GetHex() << "\n";
        std::cout << "  Header Size:        " << header_bytes.size() << " bytes (BlockHeader v1)\n";
        std::cout << "  Protocol Version:   3.0.0\n";
        std::cout << "  Network:            mainnet\n";
        std::cout << "\n";

        // Display coinbase info
        std::cout << "  Coinbase Size:      " << genesis.coinbase_hex.length() / 2 << " bytes\n";
        std::cout << "  Coinbase Hex:       " << genesis.coinbase_hex.substr(0, 64) << "...\n";
        std::cout << "  Premine:            NONE (100 DIN burned via OP_RETURN)\n";
        std::cout << "  Motto:              Dinero: Real Money For Free People\n";
        std::cout << "\n";

        std::cout << "╔════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  ✅ ALL TESTS PASSED - GENESIS BLOCK VALID                         ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════════════╝\n";
        std::cout << "\n";

        return 0;

    } catch (const std::exception& e) {
        std::cout << "\n";
        std::cout << "╔════════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  ✗ TEST FAILED                                                     ║\n";
        std::cout << "╚════════════════════════════════════════════════════════════════════╝\n";
        std::cout << "\n";
        std::cout << "  Error: " << e.what() << "\n";
        std::cout << "\n";
        return 1;
    }
}
