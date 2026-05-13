/**
 * TestBlockBuilder - Generate Real Blocks for Integration Testing
 *
 * Purpose:
 * - Create real Block objects with controlled structure
 * - Build chains and forks deterministically
 * - Predictable UTXO diffs for testing
 *
 * Capabilities:
 * - Genesis block generation
 * - Block chains (sequential)
 * - Forks at arbitrary heights
 * - Deep competing chains
 */

#pragma once

#include "../../include/primitives/block.h"
#include "../../include/wallet/transaction.h"
#include "../../include/primitives/uint256.h"
#include "../../include/consensus/outpoint.h"
#include <vector>
#include <string>
#include <ctime>

namespace dinero {
namespace test {

class TestBlockBuilder {
public:
    /**
     * Build genesis block
     *
     * @param coinbase_value Value of coinbase output (in una)
     * @param address Recipient address (simplified for tests)
     * @return Genesis block
     */
    static Block BuildGenesisBlock(uint64_t coinbase_value = 50'000'000'000,
                                     const std::string& address = "test_genesis_address") {
        Block block;

        // Header
        block.header.version = 1;
        block.header.prev_block_hash = uint256().GetHex();  // All zeros
        block.header.prev_block_hash = uint256().GetHex();
        block.header.merkle_root = "";  // Compute later if needed
        block.header.timestamp = static_cast<uint64_t>(std::time(nullptr));
        block.header.time = static_cast<uint32_t>(block.header.timestamp);
        block.header.difficulty = 0x1d00ffff;  // Regtest difficulty
        block.header.bits = block.header.difficulty;
        block.header.nonce = 0;
        block.header.utreexo_root = uint256().GetHex();  // Empty for genesis

        // Coinbase transaction
        Transaction coinbase = BuildCoinbase(0, coinbase_value, address);
        block.vtx.push_back(coinbase);

        return block;
    }

    /**
     * Build block on top of previous block
     *
     * @param prev Previous block
     * @param height Block height
     * @param txs Transactions to include (coinbase added automatically)
     * @param coinbase_value Coinbase reward
     * @return New block
     */
    static Block BuildBlock(const Block& prev,
                             uint32_t height,
                             const std::vector<Transaction>& txs = {},
                             uint64_t coinbase_value = 50'000'000'000) {
        Block block;

        // Header
        block.header.version = 1;
        block.header.prev_block_hash = prev.GetHash();
        block.header.prev_block_hash = prev.GetHash();
        block.header.merkle_root = "";
        block.header.timestamp = prev.header.timestamp + 60;  // 1 minute later
        block.header.time = static_cast<uint32_t>(block.header.timestamp);
        block.header.difficulty = 0x1d00ffff;
        block.header.bits = block.header.difficulty;
        block.header.nonce = 0;
        block.header.utreexo_root = uint256().GetHex();

        // Coinbase
        Transaction coinbase = BuildCoinbase(height, coinbase_value, "test_address");
        block.vtx.push_back(coinbase);

        // Add other transactions
        for (const auto& tx : txs) {
            block.vtx.push_back(tx);
        }

        return block;
    }

    /**
     * Build a chain of N blocks from genesis
     *
     * @param chain_length Number of blocks to build (including genesis)
     * @param coinbase_value Coinbase value for each block
     * @return Vector of blocks (genesis at index 0)
     */
    static std::vector<Block> BuildChain(size_t chain_length,
                                          uint64_t coinbase_value = 50'000'000'000) {
        std::vector<Block> chain;

        if (chain_length == 0) {
            return chain;
        }

        // Genesis
        Block genesis = BuildGenesisBlock(coinbase_value);
        chain.push_back(genesis);

        // Build chain
        for (size_t i = 1; i < chain_length; ++i) {
            Block block = BuildBlock(chain.back(), i, {}, coinbase_value);
            chain.push_back(block);
        }

        return chain;
    }

    /**
     * Build fork from existing chain
     *
     * @param main_chain Main chain to fork from
     * @param fork_height Height at which to fork (0-indexed)
     * @param fork_length Number of blocks in fork (after fork point)
     * @return Fork chain (starting from fork_height + 1)
     */
    static std::vector<Block> BuildFork(const std::vector<Block>& main_chain,
                                         size_t fork_height,
                                         size_t fork_length) {
        std::vector<Block> fork;

        if (fork_height >= main_chain.size()) {
            return fork;  // Invalid fork height
        }

        const Block& fork_point = main_chain[fork_height];

        for (size_t i = 0; i < fork_length; ++i) {
            uint32_t height = fork_height + 1 + i;
            const Block& prev = (i == 0) ? fork_point : fork.back();

            // Modify something to make it different from main chain
            Block block = BuildBlock(prev, height, {}, 50'000'000'000);
            block.header.nonce = 0xDEADBEEF;  // Different nonce = different hash

            fork.push_back(block);
        }

        return fork;
    }

    /**
     * Build coinbase transaction
     *
     * @param height Block height
     * @param value Coinbase value (in una)
     * @param address Recipient address
     * @return Coinbase transaction
     */
    static Transaction BuildCoinbase(uint32_t height,
                                      uint64_t value,
                                      const std::string& address) {
        Transaction tx;
        tx.version = 1;
        tx.lockTime = 0;
        tx.witness_version = 0xFF;  // Legacy
        tx.explicit_fee = 0;
        tx.has_explicit_fee = false;

        // Input (coinbase input with height in scriptSig)
        TxInput input;
        input.prevout.txid = uint256();  // Null
        input.prevout.vout = 0xFFFFFFFF;  // Coinbase marker
        input.scriptSig = "coinbase_height_" + std::to_string(height);
        input.sequence = 0xFFFFFFFF;
        tx.vin.push_back(input);

        // Output
        TxOutput output;
        output.value = value;
        output.scriptPubKey = "OP_DUP OP_HASH160 " + address + " OP_EQUALVERIFY OP_CHECKSIG";
        tx.vout.push_back(output);

        return tx;
    }

    /**
     * Build simple spend transaction
     *
     * @param input_outpoint Outpoint to spend
     * @param input_value Value of input (for fee calculation)
     * @param output_value Value of output
     * @param output_address Recipient address
     * @return Spend transaction
     */
    static Transaction BuildSpend(const OutPoint& input_outpoint,
                                   uint64_t input_value,
                                   uint64_t output_value,
                                   const std::string& output_address) {
        Transaction tx;
        tx.version = 1;
        tx.lockTime = 0;
        tx.witness_version = 0xFF;  // Legacy
        tx.explicit_fee = 0;
        tx.has_explicit_fee = false;

        // Input
        TxInput input;
        input.prevout = input_outpoint;
        input.scriptSig = "test_signature";  // Simplified for tests
        input.sequence = 0xFFFFFFFE;
        tx.vin.push_back(input);

        // Output
        TxOutput output;
        output.value = output_value;
        output.scriptPubKey = "OP_DUP OP_HASH160 " + output_address + " OP_EQUALVERIFY OP_CHECKSIG";
        tx.vout.push_back(output);

        // Fee is implicit: input_value - output_value

        return tx;
    }

    /**
     * Build transaction with multiple inputs and outputs
     *
     * @param inputs Vector of input outpoints
     * @param outputs Vector of output (value, address) pairs
     * @return Transaction
     */
    static Transaction BuildMultiInOut(const std::vector<OutPoint>& inputs,
                                        const std::vector<std::pair<uint64_t, std::string>>& outputs) {
        Transaction tx;
        tx.version = 1;
        tx.lockTime = 0;
        tx.witness_version = 0xFF;
        tx.explicit_fee = 0;
        tx.has_explicit_fee = false;

        // Inputs
        for (const auto& outpoint : inputs) {
            TxInput input;
            input.prevout = outpoint;
            input.scriptSig = "test_signature";
            input.sequence = 0xFFFFFFFE;
            tx.vin.push_back(input);
        }

        // Outputs
        for (const auto& [value, address] : outputs) {
            TxOutput output;
            output.value = value;
            output.scriptPubKey = "OP_DUP OP_HASH160 " + address + " OP_EQUALVERIFY OP_CHECKSIG";
            tx.vout.push_back(output);
        }

        return tx;
    }
};

} // namespace test
} // namespace dinero
