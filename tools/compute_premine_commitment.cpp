// Compute the new canonical premine utreexo commitment (v2 scheme)
// Build: cmake --build build --target compute_premine_commitment
// Run: ./build/compute_premine_commitment

#include "consensus/utreexo_accumulator.h"
#include "consensus/premine_constants.h"
#include "primitives/uint256.h"
#include "primitives/hash_domains.h"
#include "daemon/bech32_encoder.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <cstring>

using namespace dinero;
using namespace dinero::consensus;

int main() {
    // ===== Step 1: Compute the premine leaf hash =====
    // The premine coinbase txid (from PREMINE_MERKLE_ROOT since it's the only tx)
    uint256 coinbase_txid = uint256::FromHexUnsafe(dinero::premine::PREMINE_MERKLE_ROOT);

    // The premine scriptPubKey (P2TR from bech32m address)
    auto decoded = Bech32Encoder::decode_segwit_address(dinero::premine::PREMINE_ADDRESS);
    std::vector<uint8_t> scriptPubKey;
    scriptPubKey.push_back(0x51); // OP_1 (witness version 1)
    scriptPubKey.push_back(0x20); // Push 32 bytes
    scriptPubKey.insert(scriptPubKey.end(), decoded.witness_program.begin(), decoded.witness_program.end());

    uint64_t amount = dinero::premine::PREMINE_AMOUNT_UNA;

    UtreexoHash leaf_hash = HashUTXOLegacy(coinbase_txid, 0, amount, scriptPubKey);

    std::cout << "=== Premine Commitment v2 ===" << std::endl;
    std::cout << "Leaf hash (bare):  ";
    for (uint8_t b : leaf_hash)
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)b;
    std::cout << std::endl;

    // ===== Step 2: Build single-leaf forest and get canonical commitment =====
    UtreexoForest forest;
    forest.add(leaf_hash);

    UtreexoHash commitment = forest.getCommitment();

    std::cout << "Commitment (v2):   ";
    for (uint8_t b : commitment)
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)b;
    std::cout << std::endl;

    // Print as uint256 GetHex (reversed) for constant files
    uint256 u256;
    std::memcpy(u256.data, commitment.data(), 32);
    std::cout << "GetHex (display):  " << u256.GetHex() << std::endl;

    std::cout << std::dec << "numLeaves:         " << forest.getNumLeaves() << std::endl;

    // ===== Step 3: Empty forest commitment (for genesis block) =====
    UtreexoForest empty_forest;
    UtreexoHash empty_commitment = empty_forest.getCommitment();
    std::cout << std::endl << "=== Genesis (Empty Forest) ===" << std::endl;
    std::cout << "Commitment (v2):   ";
    for (uint8_t b : empty_commitment)
        std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)b;
    std::cout << std::endl;

    uint256 empty_u256;
    std::memcpy(empty_u256.data, empty_commitment.data(), 32);
    std::cout << "GetHex (display):  " << empty_u256.GetHex() << std::endl;

    return 0;
}
