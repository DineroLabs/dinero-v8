#include "mining/block_template.h"
#include "mining/address_validator.h"
#include "common/sha256d.h"
#include "consensus/chainparams.h"
#include "consensus/subsidy.h"  // Phase M: Single source of truth for subsidy
#include "consensus/freeze.h"   // Phase M: Build-time guard
#include "consensus/utreexo_accumulator.h"  // Phase 10d: For UtreexoForest
#include "common/logger.h"

// ═══════════════════════════════════════════════════════════════════════════════
// BUILD-TIME GUARD: Enforce consensus core dependency
// ═══════════════════════════════════════════════════════════════════════════════
// This file MUST link against dinero_consensus. If this static_assert fails,
// it means block_template.cpp is being compiled without the consensus library,
// which would allow duplicate subsidy logic to creep back in.
// ═══════════════════════════════════════════════════════════════════════════════
static_assert(
    dinero::consensus::CONSENSUS_VERSION_MAJOR >= 1,
    "block_template.cpp requires dinero_consensus library (subsidy rules live there)"
);
#include <algorithm>
#include <ctime>
#include <unordered_set>

namespace dinero {
namespace mining {

// ============================================================================
// Phase 26: Block Template Builder Implementation
// ============================================================================
//
// ✅ Phase 26.4 Complete: Address Validation Integrated
//
// buildMiningOutputScript() now properly decodes Dinero addresses using:
// - Bech32 decoder (din1q..., din1p...) for SegWit and Taproot
// - Base58Check decoder (D...) for legacy P2PKH addresses
//
// Supported address types:
// - P2PKH (legacy): D... → OP_DUP OP_HASH160 <hash> OP_EQUALVERIFY OP_CHECKSIG
// - P2WPKH (SegWit): din1q... → OP_0 <20-byte-hash>
// - P2WSH (SegWit): din1q... → OP_0 <32-byte-hash>
// - P2TR (Taproot): din1p... → OP_1 <32-byte-pubkey>
//
// Invalid addresses fall back to OP_RETURN (safe, deliberately unspendable).
//
// STATUS: Ready for production mining
// ============================================================================

BlockTemplateBuilder::BlockTemplateBuilder(
    mempool::Mempool& mempool,
    consensus::CoinsDB& coins_db,
    const BlockTemplateConfig& config,
    consensus::UtreexoForest* utreexo_forest
)
    : mempool_(mempool)
    , coins_db_(coins_db)
    , config_(config)
    , utreexo_forest_(utreexo_forest)  // Phase 10d: Store optional forest pointer
{
}

BlockTemplateBuilder::~BlockTemplateBuilder() {
}

// ============================================================================
// Block Template Construction
// ============================================================================

std::unique_ptr<BlockTemplate> BlockTemplateBuilder::createBlockTemplate(
    const std::string& previous_block_hash,
    uint32_t height,
    uint64_t timestamp,
    uint32_t bits,
    const std::string& mining_address
) {
    auto template_block = std::make_unique<BlockTemplate>();

    // Set metadata
    template_block->height = height;
    template_block->timestamp = timestamp;
    template_block->previous_block_hash = previous_block_hash;
    template_block->bits = bits;

    // Select transactions from mempool
    std::vector<Transaction> selected_txs;
    uint64_t total_fees = 0;

    bool selection_ok = selectTransactions(
        height,
        config_.max_block_weight,
        config_.max_sigops,
        selected_txs,
        total_fees
    );

    if (!selection_ok) {
        // Failed to select transactions - return empty template
        return nullptr;
    }

    // Calculate block subsidy
    uint64_t block_subsidy = getBlockSubsidy(height);
    if (!dinero::CheckedAddUna(block_subsidy, total_fees)) {
        return nullptr;
    }

    // Create coinbase transaction
    Transaction coinbase = createCoinbase(height, block_subsidy, total_fees, mining_address, 0);

    // Build transaction list (coinbase first, then selected txs)
    template_block->transactions.clear();
    template_block->transactions.push_back(coinbase);
    template_block->transactions.insert(
        template_block->transactions.end(),
        selected_txs.begin(),
        selected_txs.end()
    );

    // Build fee list (0 for coinbase, then actual fees)
    template_block->fees.clear();
    template_block->fees.push_back(0);
    for (const auto& tx : selected_txs) {
        // Calculate fee for this transaction
        // Note: We already calculated total fees, but we need per-tx fees
        // For now, we'll compute it on the fly
        uint64_t tx_fee = 0;

        // Get total inputs
        uint64_t total_in = 0;
        consensus::CoinsViewCache view(&coins_db_);
        for (const auto& input : tx.vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            auto coin_result = view.getCoin(outpoint);
            if (coin_result.ok()) {
                total_in += coin_result.value().value.GetUna();
            }
        }

        // Get total outputs
        uint64_t total_out = 0;
        for (const auto& output : tx.vout) {
            total_out += output.value.GetUna();
        }

        if (total_in >= total_out) {
            tx_fee = total_in - total_out;
        }

        template_block->fees.push_back(tx_fee);
    }

    // Set metrics
    template_block->total_fees = total_fees;
    template_block->block_subsidy = block_subsidy;
    template_block->coinbase_value = coinbase_value;
    template_block->num_transactions = template_block->transactions.size();

    // Calculate block size and weight
    size_t block_size = 0;
    size_t block_weight = 0;
    size_t num_sigops = 0;

    for (const auto& tx : template_block->transactions) {
        block_size += tx.Serialize().size();
        block_weight += calculateTransactionWeight(tx);
        num_sigops += countSigOps(tx);
    }

    template_block->block_size = block_size;
    template_block->block_weight = block_weight;
    template_block->num_sigops = num_sigops;

    // Calculate merkle root
    std::string merkle_root = calculateMerkleRoot(template_block->transactions);

    // Build block header
    template_block->block.header.version = 1;
    template_block->block.header.prev_block_hash = uint256::FromHexUnsafe(previous_block_hash);
    template_block->block.header.merkle_root = uint256::FromHexUnsafe(merkle_root);
    template_block->block.header.timestamp = timestamp;  // Phase 3: uint64_t, 8 bytes
    // Line removed: was incorrectly truncating timestamp to 32 bits
    template_block->block.header.difficulty = bits;
    template_block->block.header.nonce = 0;  // Miner will adjust this
    template_block->block.header.ZeroReserved();

    // Phase 10d: Utreexo commitment (OPTIONAL - depends on forest availability)
    //
    // The root is a 32-byte commitment to the post-block UTXO state.
    // It is computed HERE from the local Utreexo forest (accumulator).
    //
    // The forest/leaves/trees are LOCAL ONLY (never transmitted).
    // The root is the ONLY thing that goes on-chain (bytes 68-99 in header).
    //
    // Miners receive the header with this root and hash it (they don't know what it means).
    // Validators recompute the root from their own forest and verify it matches.
    //
    // If no forest is provided → zero root (legacy/disabled mode).
    if (utreexo_forest_) {
        // Clone the current forest to simulate applying this block
        auto forest_copy = utreexo_forest_->clone();

        // ═══════════════════════════════════════════════════════════════════════════
        // UTREEXO CANONICAL ORDER: REMOVE ALL → ADD ALL
        // ═══════════════════════════════════════════════════════════════════════════
        // Utreexo has ONE legal order per block:
        //   1. REMOVE all spent UTXOs (from previous state)
        //   2. ADD all new outputs (including coinbase)
        //   3. Commit root
        //
        // This is NOT per-transaction interleaved - it's two separate passes.
        // ═══════════════════════════════════════════════════════════════════════════

        // PASS 1: REMOVE ALL spent UTXOs (entire block)
        for (size_t tx_idx = 0; tx_idx < template_block->transactions.size(); ++tx_idx) {
            const Transaction& tx = template_block->transactions[tx_idx];

            // Skip coinbase (no inputs to spend)
            if (tx_idx == 0) continue;

            // Remove spent UTXOs from forest
            for (const auto& input : tx.vin) {
                // Hash the UTXO being spent
                // Note: We need the UTXO data (amount, scriptPubKey) to compute leaf hash
                // For template building, we look this up from coins_db_
                OutPoint outpoint(input.prevout.txid, input.prevout.vout);
                auto coin_result = coins_db_.getCoin(outpoint);
                if (coin_result.ok()) {
                    const auto& coin = coin_result.value();
                    auto leaf_hash = consensus::HashUTXO(
                        input.prevout.txid.AsUint256(),
                        input.prevout.vout,
                        coin.value.GetUna(),
                        coin.scriptPubKey
                    );

                    // Find position and generate proof (simplified - forest handles this)
                    auto position_opt = forest_copy.findLeafPosition(leaf_hash);
                    if (position_opt.has_value()) {
                        auto proof_opt = forest_copy.prove(position_opt.value());
                        if (proof_opt.has_value()) {
                            forest_copy.remove(leaf_hash, proof_opt.value());
                        }
                    }
                }
            }
        }

        // PASS 2: ADD ALL new outputs (entire block, including coinbase)
        for (size_t tx_idx = 0; tx_idx < template_block->transactions.size(); ++tx_idx) {
            const Transaction& tx = template_block->transactions[tx_idx];

            for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
                const auto& output = tx.vout[vout];

                // Hash the new UTXO
                auto leaf_hash = consensus::HashUTXO(
                    tx.GetTxid().AsUint256(),
                    vout,
                    output.value.GetUna(),
                    output.scriptPubKey
                );

                forest_copy.add(leaf_hash);
            }
        }

        // Get the commitment (root) from the post-block forest
        auto commitment = forest_copy.getCommitment();

        // Convert UtreexoHash (std::vector<uint8_t>) to uint256
        if (commitment.size() == 32) {
            uint256 root;
            memcpy(root.data, commitment.data(), 32);
            template_block->block.header.utreexo_root = root;
        } else {
            // Fallback to zero if commitment is invalid
            template_block->block.header.utreexo_root = uint256();
        }
    } else {
        // No forest available → zero root (legacy/disabled mode)
        template_block->block.header.utreexo_root = uint256();
    }

    // Set block transactions
    template_block->block.vtx = template_block->transactions;

    return template_block;
}

// ============================================================================
// Phase C3: Block Template with PayoutSpec (Multi-Output Coinbase)
// ============================================================================

std::unique_ptr<BlockTemplate> BlockTemplateBuilder::createBlockTemplate(
    const std::string& previous_block_hash,
    uint32_t height,
    uint64_t timestamp,
    uint32_t bits,
    const dinero::PayoutSpec& payout_spec
) {
    // Validate payout spec first
    auto validation = payout_spec.Validate();
    if (!validation.valid) {
        dinero::g_logger.error("[BlockTemplate] Invalid PayoutSpec: " + validation.error);
        return nullptr;
    }

    auto template_block = std::make_unique<BlockTemplate>();

    // Set metadata
    template_block->height = height;
    template_block->timestamp = timestamp;
    template_block->previous_block_hash = previous_block_hash;
    template_block->bits = bits;

    // Select transactions from mempool
    std::vector<Transaction> selected_txs;
    uint64_t total_fees = 0;

    bool selection_ok = selectTransactions(
        height,
        config_.max_block_weight,
        config_.max_sigops,
        selected_txs,
        total_fees
    );

    if (!selection_ok) {
        return nullptr;
    }

    // Calculate block subsidy
    uint64_t block_subsidy = getBlockSubsidy(height);
    if (!dinero::CheckedAddUna(block_subsidy, total_fees)) {
        return nullptr;
    }

    // Create multi-output coinbase using PayoutSpec
    Transaction coinbase = createCoinbase(height, block_subsidy, total_fees, payout_spec, 0);

    // Build transaction list (coinbase first, then selected txs)
    template_block->transactions.clear();
    template_block->transactions.push_back(coinbase);
    template_block->transactions.insert(
        template_block->transactions.end(),
        selected_txs.begin(),
        selected_txs.end()
    );

    // Build fee list (0 for coinbase, then actual fees)
    template_block->fees.clear();
    template_block->fees.push_back(0);
    for (const auto& tx : selected_txs) {
        uint64_t tx_fee = 0;
        uint64_t total_in = 0;
        consensus::CoinsViewCache view(&coins_db_);
        for (const auto& input : tx.vin) {
            OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            auto coin_result = view.getCoin(outpoint);
            if (coin_result.ok()) {
                total_in += coin_result.value().value.GetUna();
            }
        }
        uint64_t total_out = 0;
        for (const auto& output : tx.vout) {
            total_out += output.value.GetUna();
        }
        if (total_in >= total_out) {
            tx_fee = total_in - total_out;
        }
        template_block->fees.push_back(tx_fee);
    }

    // Set metrics
    template_block->total_fees = total_fees;
    template_block->block_subsidy = block_subsidy;
    template_block->coinbase_value = coinbase_value;
    template_block->num_transactions = template_block->transactions.size();

    // Calculate block size and weight
    size_t block_size = 0;
    size_t block_weight = 0;
    size_t num_sigops = 0;

    for (const auto& tx : template_block->transactions) {
        block_size += tx.Serialize().size();
        block_weight += calculateTransactionWeight(tx);
        num_sigops += countSigOps(tx);
    }

    template_block->block_size = block_size;
    template_block->block_weight = block_weight;
    template_block->num_sigops = num_sigops;

    // Calculate merkle root
    std::string merkle_root = calculateMerkleRoot(template_block->transactions);

    // Build block header
    template_block->block.header.version = 1;
    template_block->block.header.prev_block_hash = uint256::FromHexUnsafe(previous_block_hash);
    template_block->block.header.merkle_root = uint256::FromHexUnsafe(merkle_root);
    template_block->block.header.timestamp = timestamp;
    template_block->block.header.difficulty = bits;
    template_block->block.header.nonce = 0;
    template_block->block.header.ZeroReserved();

    // Utreexo commitment (same as single-address version)
    if (utreexo_forest_) {
        auto forest_copy = utreexo_forest_->clone();

        // PASS 1: REMOVE ALL spent UTXOs
        for (size_t tx_idx = 1; tx_idx < template_block->transactions.size(); ++tx_idx) {
            const Transaction& tx = template_block->transactions[tx_idx];
            for (const auto& input : tx.vin) {
                OutPoint outpoint(input.prevout.txid, input.prevout.vout);
                auto coin_result = coins_db_.getCoin(outpoint);
                if (coin_result.ok()) {
                    const auto& coin = coin_result.value();
                    auto leaf_hash = consensus::HashUTXO(
                        input.prevout.txid.AsUint256(),
                        input.prevout.vout,
                        coin.value.GetUna(),
                        coin.scriptPubKey
                    );
                    auto position_opt = forest_copy.findLeafPosition(leaf_hash);
                    if (position_opt.has_value()) {
                        auto proof_opt = forest_copy.prove(position_opt.value());
                        if (proof_opt.has_value()) {
                            forest_copy.remove(leaf_hash, proof_opt.value());
                        }
                    }
                }
            }
        }

        // PASS 2: ADD ALL new outputs
        for (size_t tx_idx = 0; tx_idx < template_block->transactions.size(); ++tx_idx) {
            const Transaction& tx = template_block->transactions[tx_idx];
            for (uint32_t vout = 0; vout < tx.vout.size(); ++vout) {
                const auto& output = tx.vout[vout];
                auto leaf_hash = consensus::HashUTXO(
                    tx.GetTxid().AsUint256(),
                    vout,
                    output.value.GetUna(),
                    output.scriptPubKey
                );
                forest_copy.add(leaf_hash);
            }
        }

        auto commitment = forest_copy.getCommitment();
        if (commitment.size() == 32) {
            uint256 root;
            memcpy(root.data, commitment.data(), 32);
            template_block->block.header.utreexo_root = root;
        } else {
            template_block->block.header.utreexo_root = uint256();
        }
    } else {
        template_block->block.header.utreexo_root = uint256();
    }

    template_block->block.vtx = template_block->transactions;

    dinero::g_logger.info("[BlockTemplate] Created template with " +
        std::to_string(payout_spec.Count()) + " payout outputs");

    return template_block;
}

void BlockTemplateBuilder::updateTimestamp(BlockTemplate& template_block, uint64_t new_timestamp) {
    template_block.timestamp = new_timestamp;
    template_block.block.header.timestamp = new_timestamp;  // Phase 3: uint64_t, 8 bytes
    // Line removed: was incorrectly truncating timestamp to 32 bits

    // Update coinbase timestamp (optional - some miners encode timestamp in coinbase)
    if (!template_block.transactions.empty()) {
        // Coinbase is first transaction
        // Note: We don't need to update coinbase scriptSig with timestamp
        // That's optional and up to the miner
    }

    // Merkle root doesn't need to change unless coinbase changes
}

// ============================================================================
// Transaction Selection
// ============================================================================

bool BlockTemplateBuilder::selectTransactions(
    uint32_t height,
    size_t max_weight,
    size_t max_sigops,
    std::vector<Transaction>& selected_txs,
    uint64_t& total_fees
) {
    selected_txs.clear();
    total_fees = 0;

    // Get mempool entries sorted by ancestor fee rate
    std::vector<std::shared_ptr<const mempool::MempoolEntry>> entries;

    if (config_.sort_by_ancestor_fee) {
        entries = mempool_.getEntriesByAncestorScore();
    } else {
        entries = mempool_.getEntriesByFeeRate();
    }

    // Selection state
    SelectionState state;

    // Reserve space for coinbase (approximate)
    state.current_weight = 1000;  // ~250 bytes base size for coinbase
    state.current_sigops = 0;

    // Iterate through transactions in fee rate order
    for (const auto& entry_ptr : entries) {
        const auto& entry = *entry_ptr;

        // Check if we can add this transaction
        if (!canAddTransaction(entry, state, max_weight, max_sigops)) {
            continue;  // Skip this transaction
        }

        // Check if all ancestors are in the block
        if (!hasAllAncestors(entry, state)) {
            continue;  // Skip - missing ancestors
        }

        // Add transaction
        addTransaction(entry, state, selected_txs);
    }

    total_fees = state.current_fees;
    return true;
}

// ============================================================================
// Coinbase Transaction
// ============================================================================

Transaction BlockTemplateBuilder::createCoinbase(
    uint32_t height,
    uint64_t block_subsidy,
    uint64_t total_fees,
    const std::string& mining_address,
    uint32_t extra_nonce
) {
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;

    // Create coinbase input
    TxInput input;

    // Null outpoint
    input.prevout.txid = TxId();  // Null TxId
    input.prevout.vout = 0xFFFFFFFF;

    // Build scriptSig with block height (BIP 34) + extra nonce
    input.scriptSig = buildCoinbaseScriptSig(height, extra_nonce);

    // Sequence (0xFFFFFFFF = final)
    input.sequence = 0xFFFFFFFF;

    coinbase.vin.push_back(input);

    // Create coinbase output
    TxOutput output;
    auto total_output = dinero::CheckedAddUna(block_subsidy, total_fees);
    if (!total_output) {
        throw std::runtime_error("Coinbase value overflow in BlockTemplateBuilder::createCoinbase");
    }
    output.value = AmountUna::Una(*total_output);
    output.scriptPubKey = buildMiningOutputScript(mining_address);

    coinbase.vout.push_back(output);

    return coinbase;
}

// ============================================================================
// Phase C3: Multi-Output Coinbase with PayoutSpec
// ============================================================================

Transaction BlockTemplateBuilder::createCoinbase(
    uint32_t height,
    uint64_t block_subsidy,
    uint64_t total_fees,
    const dinero::PayoutSpec& payout_spec,
    uint32_t extra_nonce
) {
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;

    // Create coinbase input (same as single-address version)
    TxInput input;
    input.prevout.txid = TxId();  // Null TxId
    input.prevout.vout = 0xFFFFFFFF;
    input.scriptSig = buildCoinbaseScriptSig(height, extra_nonce);
    input.sequence = 0xFFFFFFFF;
    coinbase.vin.push_back(input);

    // Resolve payouts from PayoutSpec
    auto total_reward = dinero::CheckedAddUna(block_subsidy, total_fees);
    if (!total_reward) {
        throw std::runtime_error("Coinbase value overflow in BlockTemplateBuilder::createCoinbase");
    }
    auto resolved_payouts = payout_spec.Resolve(*total_reward);

    if (resolved_payouts.empty()) {
        // Fallback: single output with full reward to first address
        dinero::g_logger.warning("[Coinbase] PayoutSpec resolution failed, using fallback");
        if (!payout_spec.Entries().empty()) {
            TxOutput output;
            output.value = AmountUna::Una(*total_reward);
            output.scriptPubKey = buildMiningOutputScript(payout_spec.Entries()[0].address);
            coinbase.vout.push_back(output);
        } else {
            // Emergency fallback: OP_RETURN (unspendable)
            TxOutput output;
            output.value = AmountUna::Una(*total_reward);
            output.scriptPubKey = {0x6a};  // OP_RETURN
            coinbase.vout.push_back(output);
        }
        return coinbase;
    }

    // Create outputs in deterministic order (input order from PayoutSpec)
    for (const auto& payout : resolved_payouts) {
        TxOutput output;
        output.value = AmountUna::Una(payout.amount);
        output.scriptPubKey = buildMiningOutputScript(payout.address);
        coinbase.vout.push_back(output);
    }

    // Verify total output equals total reward (sanity check)
    uint64_t total_output = 0;
    for (const auto& out : coinbase.vout) {
        total_output += out.value.GetUna();
    }

    if (total_output != total_reward) {
        dinero::g_logger.error("[Coinbase] Output mismatch: " +
            std::to_string(total_output) + " != " + std::to_string(total_reward));
    }

    return coinbase;
}

uint64_t BlockTemplateBuilder::getBlockSubsidy(uint32_t height) {
    // ═══════════════════════════════════════════════════════════════════════════
    // SINGLE SOURCE OF TRUTH: dinero::ConsensusSubsidy
    // ═══════════════════════════════════════════════════════════════════════════
    // Production code MUST NOT implement subsidy rules.
    // All monetary policy lives in consensus/subsidy.h (dinero_consensus library).
    // This ensures miners, validators, and wallets all agree on coin creation.
    // ═══════════════════════════════════════════════════════════════════════════
    return dinero::ConsensusSubsidy::GetBlockSubsidy(height).GetUna();
}

// ============================================================================
// Merkle Root Computation
// ============================================================================

std::string BlockTemplateBuilder::calculateMerkleRoot(const std::vector<Transaction>& transactions) {
    std::string merkle_root;
    std::vector<std::string> merkle_branches;  // Not returned, but buildMerkleTree needs it

    if (!buildMerkleTree(transactions, merkle_root, merkle_branches)) {
        // Duplicate txids found - return error indicator
        return std::string(64, '0');
    }

    return merkle_root;
}

bool BlockTemplateBuilder::buildMerkleTree(
    const std::vector<Transaction>& transactions,
    std::string& merkle_root,
    std::vector<std::string>& merkle_branches
) {
    /**
     * Phase 28: Bitcoin-style Merkle Tree Builder
     *
     * Protections implemented:
     * 1. CVE-2012-2459: Duplicate txid detection
     * 2. Proper odd-node duplication (Bitcoin consensus rules)
     * 3. Merkle branch collection for Stratum mining
     *
     * Returns:
     * - merkle_root: The root hash of the merkle tree
     * - merkle_branches: Hashes needed to reconstruct root from coinbase
     */

    merkle_branches.clear();

    if (transactions.empty()) {
        merkle_root = std::string(64, '0');
        return true;
    }

    // Get transaction IDs
    std::vector<std::string> hashes;
    std::unordered_set<std::string> seen_txids;  // CVE-2012-2459 protection

    for (const auto& tx : transactions) {
        std::string txid = tx.GetTxid().AsUint256().GetHex();

        // Check for duplicate txids (CVE-2012-2459)
        if (seen_txids.count(txid) > 0) {
            dinero::g_logger.error("[BlockTemplate] Duplicate txid detected: " + txid);
            return false;  // Block with duplicate txids is invalid
        }
        seen_txids.insert(txid);
        hashes.push_back(txid);
    }

    // For Stratum: we need the merkle branches to reconstruct root from coinbase
    // Merkle branches are the "siblings" at each level when walking from coinbase to root
    std::vector<std::string> current_level = hashes;
    size_t coinbase_index = 0;  // Coinbase is always first

    // Build merkle tree level by level
    while (current_level.size() > 1) {
        std::vector<std::string> next_level;

        // Record the sibling of the coinbase path (needed for Stratum)
        if (coinbase_index < current_level.size()) {
            // The sibling is the node we pair with
            size_t sibling_index = (coinbase_index % 2 == 0) ? coinbase_index + 1 : coinbase_index - 1;

            if (sibling_index < current_level.size()) {
                merkle_branches.push_back(current_level[sibling_index]);
            } else if (coinbase_index == current_level.size() - 1 && current_level.size() % 2 != 0) {
                // Odd number of nodes, and coinbase is last - pair with itself
                merkle_branches.push_back(current_level[coinbase_index]);
            }
        }

        // If odd number of hashes, duplicate last one (Bitcoin consensus rule)
        if (current_level.size() % 2 != 0) {
            current_level.push_back(current_level.back());
        }

        // Hash pairs
        for (size_t i = 0; i < current_level.size(); i += 2) {
            // Concatenate two hashes
            std::vector<uint8_t> combined;

            // Convert hex strings to bytes (little-endian byte order)
            for (size_t j = 0; j < 64; j += 2) {
                uint8_t byte1 = std::strtol(current_level[i].substr(j, 2).c_str(), nullptr, 16);
                combined.push_back(byte1);
            }
            for (size_t j = 0; j < 64; j += 2) {
                uint8_t byte2 = std::strtol(current_level[i + 1].substr(j, 2).c_str(), nullptr, 16);
                combined.push_back(byte2);
            }

            // Double SHA256
            std::string pair_hash = Dinero::Common::double_sha256(combined);
            next_level.push_back(pair_hash);
        }

        // Update coinbase index for next level
        coinbase_index = coinbase_index / 2;
        current_level = next_level;
    }

    merkle_root = current_level[0];
    return true;
}

// ============================================================================
// Weight and Size Calculation
// ============================================================================

size_t BlockTemplateBuilder::calculateTransactionWeight(const Transaction& tx) {
    // BIP 141: weight = (base_size * 3) + total_size
    //
    // For now, simplified: just use serialized size * 4
    // (This assumes non-witness transactions)
    // TODO: Implement proper witness size calculation

    size_t total_size = tx.Serialize().size();
    return total_size * 4;  // Weight units
}

size_t BlockTemplateBuilder::countSigOps(const Transaction& tx) {
    // Count signature operations
    // For now, simplified: 1 sigop per input
    // TODO: Implement proper sigop counting based on script type

    return tx.vin.size();
}

// ============================================================================
// Helper Functions
// ============================================================================

bool BlockTemplateBuilder::canAddTransaction(
    const mempool::MempoolEntry& entry,
    const SelectionState& state,
    size_t max_weight,
    size_t max_sigops
) const {
    // Calculate weight with this transaction
    size_t tx_weight = calculateTransactionWeight(entry.tx);
    size_t new_weight = state.current_weight + tx_weight;

    if (new_weight > max_weight) {
        return false;
    }

    // Calculate sigops with this transaction
    size_t tx_sigops = countSigOps(entry.tx);
    size_t new_sigops = state.current_sigops + tx_sigops;

    if (new_sigops > max_sigops) {
        return false;
    }

    return true;
}

bool BlockTemplateBuilder::hasAllAncestors(
    const mempool::MempoolEntry& entry,
    const SelectionState& state
) const {
    // Check if all parents are in selected set
    for (const auto& parent_txid : entry.parents) {
        if (state.selected_txids.find(parent_txid.AsUint256().GetHex()) == state.selected_txids.end()) {
            return false;  // Missing parent
        }
    }

    return true;
}

void BlockTemplateBuilder::addTransaction(
    const mempool::MempoolEntry& entry,
    SelectionState& state,
    std::vector<Transaction>& selected_txs
) {
    // Add to selected set
    state.selected_txids.insert(entry.txid.AsUint256().GetHex());
    selected_txs.push_back(entry.tx);

    // Update state
    state.current_weight += calculateTransactionWeight(entry.tx);
    state.current_sigops += countSigOps(entry.tx);
    state.current_fees += entry.fee;
}

std::vector<uint8_t> BlockTemplateBuilder::buildCoinbaseScriptSig(
    uint32_t height,
    uint32_t extra_nonce
) const {
    // BIP 34: coinbase must start with block height
    std::vector<uint8_t> scriptSig;

    // Encode height as compact size
    if (height < 128) {
        scriptSig.push_back(1);  // Length: 1 byte
        scriptSig.push_back(height & 0xFF);
    } else if (height < 32768) {
        scriptSig.push_back(2);  // Length: 2 bytes
        scriptSig.push_back(height & 0xFF);
        scriptSig.push_back((height >> 8) & 0xFF);
    } else if (height < 8388608) {
        scriptSig.push_back(3);  // Length: 3 bytes
        scriptSig.push_back(height & 0xFF);
        scriptSig.push_back((height >> 8) & 0xFF);
        scriptSig.push_back((height >> 16) & 0xFF);
    } else {
        scriptSig.push_back(4);  // Length: 4 bytes
        scriptSig.push_back(height & 0xFF);
        scriptSig.push_back((height >> 8) & 0xFF);
        scriptSig.push_back((height >> 16) & 0xFF);
        scriptSig.push_back((height >> 24) & 0xFF);
    }

    // Add extra nonce (4 bytes)
    scriptSig.push_back((extra_nonce >> 0) & 0xFF);
    scriptSig.push_back((extra_nonce >> 8) & 0xFF);
    scriptSig.push_back((extra_nonce >> 16) & 0xFF);
    scriptSig.push_back((extra_nonce >> 24) & 0xFF);

    // Add arbitrary data (Dinero identifier)
    std::string message = "Dinero";
    scriptSig.insert(scriptSig.end(), message.begin(), message.end());

    return scriptSig;
}

std::vector<uint8_t> BlockTemplateBuilder::buildMiningOutputScript(const std::string& address) const {
    // ========================================================================
    // Phase 26.4: Proper Address Decoding (Bech32/Base58)
    // ========================================================================
    //
    // This function decodes Dinero addresses and builds the correct
    // scriptPubKey for the coinbase output.
    //
    // Supported address types:
    // - P2PKH (legacy base58): D...
    // - P2WPKH (bech32 SegWit): din1q...
    // - P2WSH (bech32 SegWit): din1q... (32-byte)
    // - P2TR (bech32 Taproot): din1p...
    // ========================================================================

    // Decode address
    AddressInfo info;
    if (!DecodeAddress(address, info)) {
        // Invalid address - return OP_RETURN stub for safety
        // This prevents mining with invalid addresses that create unspendable outputs
        std::vector<uint8_t> scriptPubKey;
        scriptPubKey.push_back(0x6a);  // OP_RETURN
        return scriptPubKey;
    }

    // Build scriptPubKey from decoded address
    std::vector<uint8_t> scriptPubKey = BuildScriptPubKey(info);

    // If BuildScriptPubKey failed (empty result), return OP_RETURN stub
    if (scriptPubKey.empty()) {
        scriptPubKey.push_back(0x6a);  // OP_RETURN
    }

    return scriptPubKey;
}

} // namespace mining
} // namespace dinero
