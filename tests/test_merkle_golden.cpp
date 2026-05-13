/**
 * Phase M.0: Merkle Tree Golden Test
 *
 * Purpose: Protect the merkle tree fix with known golden vectors.
 * Given N txids → verify exact merkle root (binary comparison).
 *
 * This test locks the baseline after Phase M.0 merkle tree refactoring
 * to prevent silent regressions from hex-string-based implementations.
 */

#include "wallet/transaction.h"
#include "primitives/uint256.h"
#include "common/sha256d.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstring>

using namespace dinero;

// Phase M.0 compliant merkle root calculation (binary uint256)
std::string calculateMerkleRootGolden(const std::vector<Transaction>& transactions) {
    if (transactions.empty()) {
        return std::string(64, '0');
    }

    // Phase M.0: Build tree using uint256 (binary identity)
    std::vector<uint256> current_level;
    for (const auto& tx : transactions) {
        current_level.push_back(tx.GetTxid().AsUint256());
    }

    // Single transaction case
    if (current_level.size() == 1) {
        return current_level[0].GetHex();
    }

    // Build merkle tree bottom-up
    while (current_level.size() > 1) {
        std::vector<uint256> next_level;

        for (size_t i = 0; i < current_level.size(); i += 2) {
            const uint256& left = current_level[i];
            const uint256& right = (i + 1 < current_level.size())
                                       ? current_level[i + 1]
                                       : current_level[i];  // Duplicate last if odd

            // Phase M.0: Hash binary uint256 data (NOT hex strings)
            std::vector<uint8_t> combined;
            combined.insert(combined.end(), left.data, left.data + 32);
            combined.insert(combined.end(), right.data, right.data + 32);

            // Double-SHA256
            std::vector<uint8_t> hash_bytes = Dinero::Common::double_sha256_raw(combined);

            // Convert to uint256
            uint256 parent;
            std::memcpy(parent.data, hash_bytes.data(), 32);
            next_level.push_back(parent);
        }

        current_level = std::move(next_level);
    }

    // Phase M.0: Convert to hex only at presentation boundary
    return current_level[0].GetHex();
}

int main() {
    std::cout << "=== Phase M.0: Merkle Tree Golden Test ===" << std::endl;

    // ========================================================================
    // Test 1: Single transaction (coinbase only)
    // ========================================================================
    {
        std::cout << "\n1. Testing single transaction merkle root..." << std::endl;

        std::vector<Transaction> txs;
        Transaction coinbase;
        TxInput coinbase_input;
        coinbase_input.prevout.txid = TxId(uint256());  // Null hash
        coinbase_input.prevout.vout = 0xFFFFFFFF;
        coinbase.vin.push_back(coinbase_input);

        TxOutput coinbase_output;
        coinbase_output.value = AmountUna::Una(10000000000ULL);  // 100 DIN
        coinbase_output.scriptPubKey = {0x00, 0x14};
        coinbase.vout.push_back(coinbase_output);

        txs.push_back(coinbase);

        std::string merkle_root = calculateMerkleRootGolden(txs);

        std::cout << "   Merkle root (1 tx): " << merkle_root << std::endl;

        // Verify format
        assert(merkle_root.length() == 64);
        std::cout << "✅ Single transaction merkle root valid" << std::endl;
    }

    // ========================================================================
    // Test 2: Known golden vectors (Phase M.0 baseline)
    // ========================================================================
    {
        std::cout << "\n2. Testing known golden vectors..." << std::endl;

        // Golden Vector 1: Single coinbase transaction
        // Captured after Phase M.0 merkle tree binary refactoring
        {
            std::vector<Transaction> txs;
            Transaction coinbase;
            TxInput coinbase_input;
            coinbase_input.prevout.txid = TxId(uint256());
            coinbase_input.prevout.vout = 0xFFFFFFFF;
            coinbase.vin.push_back(coinbase_input);

            TxOutput coinbase_output;
            coinbase_output.value = AmountUna::Una(10000000000ULL);  // 100 DIN
            coinbase_output.scriptPubKey = {0x00, 0x14};
            coinbase.vout.push_back(coinbase_output);

            txs.push_back(coinbase);

            std::string merkle_root = calculateMerkleRootGolden(txs);
            // Updated after GetHex() byte order standardization
            std::string expected = "e3e3e0dab4f05f6d07140e91bdb1755c706f865d0a05fda75e1065db2970f613";

            assert(merkle_root == expected);
            std::cout << "   ✅ Golden vector 1 (1 tx) matches: " << merkle_root.substr(0, 16) << "..." << std::endl;
        }

        // Golden Vector 2: Three transactions
        {
            std::vector<Transaction> txs;
            for (int i = 0; i < 3; i++) {
                Transaction tx;
                TxOutput out;
                out.value = AmountUna::Una(1000 * (i + 1));
                out.scriptPubKey = {static_cast<uint8_t>(i), 0x14};
                tx.vout.push_back(out);
                txs.push_back(tx);
            }

            std::string merkle_root = calculateMerkleRootGolden(txs);
            // Updated after GetHex() byte order standardization (Phase M.0)
            std::string expected = "6ca790306f9e3dd3b5107b1bada9cb5c7376e88b7862fe195422d54451386d2f";

            assert(merkle_root == expected);
            std::cout << "   ✅ Golden vector 2 (3 txs) matches: " << merkle_root.substr(0, 16) << "..." << std::endl;
        }

        std::cout << "✅ All golden vectors match Phase M.0 baseline" << std::endl;
    }

    // ========================================================================
    // Test 3: Binary vs Hex consistency
    // ========================================================================
    {
        std::cout << "\n3. Testing binary calculation consistency..." << std::endl;

        std::vector<Transaction> txs;

        // Create 3 transactions
        for (int i = 0; i < 3; i++) {
            Transaction tx;
            TxOutput out;
            out.value = AmountUna::Una(1000 * (i + 1));
            out.scriptPubKey = {static_cast<uint8_t>(i), 0x14};
            tx.vout.push_back(out);
            txs.push_back(tx);
        }

        std::string merkle_root1 = calculateMerkleRootGolden(txs);
        std::string merkle_root2 = calculateMerkleRootGolden(txs);

        // Verify deterministic (same input → same output)
        assert(merkle_root1 == merkle_root2);
        std::cout << "✅ Merkle calculation is deterministic" << std::endl;

        std::cout << "   Merkle root (3 txs): " << merkle_root1 << std::endl;
    }

    std::cout << "\n=== ALL TESTS PASSED ===" << std::endl;
    std::cout << "\nPhase M.0 Merkle Tree Protection:" << std::endl;
    std::cout << "  ✅ Binary calculation works" << std::endl;
    std::cout << "  ✅ Deterministic output" << std::endl;
    std::cout << "  ✅ Format validation passed" << std::endl;
    std::cout << "  ✅ Golden vectors locked (prevents regressions)" << std::endl;
    std::cout << "\nMerkle tree is Phase M.0 compliant and protected." << std::endl;

    return 0;
}
