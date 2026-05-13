#include "mining/miner_engine.h"
#include "mining/block_template.h"
#include "consensus/block_validation.h"
#include "consensus/tx_validation.h"  // For TxValidationContext
#include "consensus/mtp_lookup.h"     // Phase 23.3: For CreateMtpLookup (BIP68 MTP lookup)
#include "consensus/asert_params.h"   // For DIFFICULTY_1_BITS constant
#include "common/sha256d.h"
#include <chrono>
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace dinero {
namespace mining {

// ============================================================================
// Phase 26.2: Mining Engine Implementation
// ============================================================================

MiningEngine::MiningEngine()
    : is_mining_(false)
{
}

MiningEngine::~MiningEngine() {
    stopMining();
}

// ============================================================================
// Mining Control
// ============================================================================

bool MiningEngine::startMining(
    std::unique_ptr<BlockTemplate> template_block,
    MiningCallback callback
) {
    // Stop any existing mining
    stopMining();

    if (!template_block) {
        return false;
    }

    // Store template and callback
    current_template_ = std::move(template_block);
    current_callback_ = callback;

    // Reset statistics
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.current_nonce = 0;
    }

    // Start mining thread
    is_mining_.store(true);
    mining_thread_ = std::make_unique<std::thread>(&MiningEngine::miningLoop, this);

    return true;
}

void MiningEngine::stopMining() {
    if (!is_mining_.load()) {
        return;
    }

    // Signal thread to stop
    is_mining_.store(false);

    // Wait for thread to finish
    if (mining_thread_ && mining_thread_->joinable()) {
        mining_thread_->join();
    }

    mining_thread_.reset();
    current_template_.reset();
    current_callback_ = nullptr;
}

// ============================================================================
// Statistics
// ============================================================================

MiningStats MiningEngine::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void MiningEngine::resetStats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = MiningStats();
}

// ============================================================================
// Mining Loop
// ============================================================================

void MiningEngine::miningLoop() {
    if (!current_template_) {
        return;
    }

    auto start_time = std::chrono::steady_clock::now();
    uint64_t hashes_this_period = 0;
    auto last_hashrate_update = start_time;

    // Mining parameters
    const uint32_t NONCE_BATCH_SIZE = 100000;  // Check stop flag every 100k hashes
    const uint32_t TIMESTAMP_UPDATE_INTERVAL = 0xFFFFFFFF;  // Update timestamp on nonce overflow

    uint32_t nonce = 0;

    while (is_mining_.load()) {
        // Mine a batch of nonces
        Block& block = current_template_->block;
        block.header.nonce = nonce;

        for (uint32_t i = 0; i < NONCE_BATCH_SIZE && is_mining_.load(); i++) {
            // Compute block hash
            std::string block_hash = hashBlockHeader(block.header);

            // Update statistics
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.hashes_computed++;
                stats_.current_nonce = nonce;
            }
            hashes_this_period++;

            // Check if we found a valid block
            if (checkProofOfWork(block_hash, current_template_->bits)) {
                // Found a valid block!
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    stats_.blocks_found++;
                    stats_.last_block_height = current_template_->height;
                    stats_.last_block_hash = block_hash;
                }

                // Call callback
                bool accepted = false;
                if (current_callback_) {
                    accepted = current_callback_(block);
                }

                // Stop mining (block found)
                is_mining_.store(false);
                return;
            }

            // Increment nonce
            nonce++;
            block.header.nonce = nonce;

            // Check for nonce overflow (update timestamp)
            if (nonce == 0) {
                updateBlockTimestamp(*current_template_);
                block = current_template_->block;  // Reload block with new timestamp
            }
        }

        // Update hash rate every second
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_hashrate_update).count();

        if (elapsed >= 1000) {
            uint64_t hash_rate = (hashes_this_period * 1000) / elapsed;

            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.hash_rate = hash_rate;
            stats_.mining_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();

            hashes_this_period = 0;
            last_hashrate_update = now;
        }
    }
}

bool MiningEngine::mineBlock(Block& block, uint32_t bits, uint32_t max_iterations) {
    for (uint32_t nonce = 0; nonce < max_iterations; nonce++) {
        block.header.nonce = nonce;

        std::string block_hash = hashBlockHeader(block.header);

        if (checkProofOfWork(block_hash, bits)) {
            return true;  // Found valid block
        }
    }

    return false;  // No valid block found in max_iterations
}

std::string MiningEngine::hashBlockHeader(const BlockHeader& header) const {
    // ✅ CRITICAL FIX (Phase 3): Use BlockHeader::SerializeForHash() for correct 128-byte serialization
    // Previous code was WRONG:
    // - Used 112 bytes instead of 128
    // - Truncated timestamp from 64-bit to 32-bit
    // - Did not include reserved[12] field
    // BlockHeader v1 is 128 bytes and MUST hash all 128 bytes for valid PoW

    // Use canonical serialization (BlockHeader::SerializeForHash())
    auto header_bytes = header.SerializeForHash();  // Returns std::array<uint8_t, 128>

    // Verify size (compile-time guarantee, but defensive check)
    static_assert(std::tuple_size<decltype(header_bytes)>::value == 128,
                  "BlockHeader::SerializeForHash() must return exactly 128 bytes");

    // Double SHA256 (Bitcoin PoW standard)
    std::vector<uint8_t> hash_raw = Dinero::Common::double_sha256_raw(header_bytes.data(), header_bytes.size());

    // Convert to hex string (big-endian for display/comparison)
    std::ostringstream hash_hex;
    hash_hex << std::hex << std::setfill('0');
    for (uint8_t byte : hash_raw) {
        hash_hex << std::setw(2) << static_cast<int>(byte);
    }

    return hash_hex.str();
}

void MiningEngine::updateBlockTimestamp(BlockTemplate& template_block) {
    // Update timestamp to current time
    uint64_t new_timestamp = std::chrono::system_clock::now().time_since_epoch().count() / 1000000000ULL;

    template_block.timestamp = new_timestamp;
    template_block.block.header.timestamp = new_timestamp;
    // Phase 3 fix: BlockHeader.timestamp is uint64_t (8 bytes), not uint32_t
    // Line removed: was incorrectly truncating to 32 bits

    // Recalculate merkle root (in case coinbase includes timestamp)
    // For now, merkle root stays the same (coinbase doesn't change)
}

// ============================================================================
// Difficulty Validation
// ============================================================================

bool MiningEngine::checkProofOfWork(const std::string& block_hash, uint32_t bits) {
    // BUG FIX: Use byte-by-byte comparison (same as validator in block_acceptor.cpp)
    // hashBlockHeader() returns big-endian hex - DO NOT reverse it
    // Target from bitsToTarget() is also big-endian

    // Convert bits to target (big-endian)
    std::string target = bitsToTarget(bits);

    // Both hash and target are in big-endian hex format
    // Pad hash to 64 chars if needed
    std::string hash_padded = block_hash;
    while (hash_padded.length() < 64) {
        hash_padded = "0" + hash_padded;  // Left-pad with zeros for big-endian
    }

    // Convert to lowercase for comparison
    std::string hash_lower = hash_padded;
    std::string target_lower = target;
    std::transform(hash_lower.begin(), hash_lower.end(), hash_lower.begin(), ::tolower);
    std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(), ::tolower);

    // Byte-by-byte comparison (big-endian: first byte is most significant)
    // hash must be < target for valid PoW
    for (size_t i = 0; i < 64; i += 2) {
        unsigned int hash_byte = 0, target_byte = 0;
        std::sscanf(hash_lower.c_str() + i, "%2x", &hash_byte);
        std::sscanf(target_lower.c_str() + i, "%2x", &target_byte);

        if (hash_byte < target_byte) {
            return true;   // hash < target, valid PoW
        } else if (hash_byte > target_byte) {
            return false;  // hash > target, invalid PoW
        }
        // If equal, continue to next byte
    }

    return true;  // hash == target, valid PoW (exact match)
}

std::string MiningEngine::bitsToTarget(uint32_t bits) {
    // BUG FIX: Build target in big-endian format (matching validator's TargetFromBits)
    //
    // Bitcoin compact target format:
    //   bits = 0xEEMMMMM where EE = exponent, MMMMMM = mantissa
    //   target = mantissa * 2^(8 * (exponent - 3))
    //
    // In big-endian 32-byte array:
    //   - Leading zeros: (32 - exponent) bytes
    //   - Mantissa: 3 bytes at position (32 - exponent)
    //   - Trailing zeros: (exponent - 3) bytes
    //
    // Example: 0x1d31ffce (exponent=29, mantissa=0x31ffce)
    //   - Leading zeros: 32 - 29 = 3 bytes = 6 hex chars
    //   - Mantissa: "31ffce" at position 6
    //   - Trailing zeros: 29 - 3 = 26 bytes = 52 hex chars
    //   - Result: "00000031ffce" + 52 zeros

    uint8_t exponent = (bits >> 24) & 0xFF;
    uint32_t mantissa = bits & 0x00FFFFFF;

    // Build 32-byte (64 hex char) target
    std::string result(64, '0');  // Start with all zeros

    if (exponent >= 3 && exponent <= 32) {
        // Position where mantissa starts (in hex chars)
        size_t mantissa_pos = (32 - exponent) * 2;

        // Format mantissa as 6 hex chars
        char mantissa_hex[7];
        std::snprintf(mantissa_hex, sizeof(mantissa_hex), "%06x", mantissa);

        // Place mantissa at correct position
        if (mantissa_pos + 6 <= 64) {
            for (int i = 0; i < 6; i++) {
                result[mantissa_pos + i] = mantissa_hex[i];
            }
        }
    }

    return result;
}

double MiningEngine::calculateDifficulty(uint32_t bits) {
    // Difficulty = max_target / current_target
    // Max target corresponds to "difficulty 1.0" (Bitcoin genesis)

    const uint32_t MAX_BITS = dinero::ASERTConsensus::DIFFICULTY_1_BITS;

    std::string max_target = bitsToTarget(MAX_BITS);
    std::string current_target = bitsToTarget(bits);

    // For simplicity, approximate difficulty from exponent and mantissa
    uint8_t max_exp = (MAX_BITS >> 24) & 0xFF;
    uint32_t max_mant = MAX_BITS & 0x00FFFFFF;

    uint8_t curr_exp = (bits >> 24) & 0xFF;
    uint32_t curr_mant = bits & 0x00FFFFFF;

    // Difficulty ≈ (max_mant / curr_mant) * 2^(8 * (max_exp - curr_exp))

    double base = static_cast<double>(max_mant) / static_cast<double>(curr_mant);
    int exp_diff = max_exp - curr_exp;
    double multiplier = 1.0;

    for (int i = 0; i < exp_diff * 8; i++) {
        multiplier *= 2.0;
    }
    for (int i = 0; i > exp_diff * 8; i--) {
        multiplier /= 2.0;
    }

    return base * multiplier;
}

// ============================================================================
// Block Submission Validator
// ============================================================================

BlockSubmissionValidator::BlockSubmissionValidator(
    consensus::CoinsDB& coins_db,
    ::dinero::ChainDB* chain_db
)
    : coins_db_(coins_db)
    , chain_db_(chain_db)
{
}

bool BlockSubmissionValidator::validateBlock(
    const Block& block,
    uint32_t expected_height,
    uint32_t expected_bits,
    std::string& error
) {
    // 1. Validate Proof of Work
    if (!validateProofOfWork(block, expected_bits, error)) {
        return false;
    }

    // 2. Validate Merkle Root
    if (!validateMerkleRoot(block, error)) {
        return false;
    }

    // 3. Validate Coinbase
    if (!validateCoinbase(block, expected_height, error)) {
        return false;
    }

    // 4. Validate Transactions
    if (!validateTransactions(block, expected_height, error)) {
        return false;
    }

    return true;
}

bool BlockSubmissionValidator::validateProofOfWork(
    const Block& block,
    uint32_t expected_bits,
    std::string& error
) {
    // Check bits match expected
    if (block.header.difficulty != expected_bits) {
        error = "Invalid difficulty bits";
        return false;
    }

    // Compute block hash
    MiningEngine engine;
    std::string block_hash = engine.hashBlockHeader(block.header);

    // Check PoW
    if (!MiningEngine::checkProofOfWork(block_hash, expected_bits)) {
        error = "Insufficient proof of work";
        return false;
    }

    return true;
}

bool BlockSubmissionValidator::validateMerkleRoot(const Block& block, std::string& error) {
    // Recompute merkle root
    std::string computed_merkle = BlockTemplateBuilder::calculateMerkleRoot(block.vtx);

    // Compare with block header
    if (computed_merkle != block.header.merkle_root) {
        error = "Invalid merkle root";
        return false;
    }

    return true;
}

bool BlockSubmissionValidator::validateCoinbase(
    const Block& block,
    uint32_t expected_height,
    std::string& error
) {
    if (block.vtx.empty()) {
        error = "Block has no transactions";
        return false;
    }

    const Transaction& coinbase = block.vtx[0];

    // Validate coinbase transaction
    consensus::TxValidationContext ctx;
    ctx.block_height = expected_height;
    ctx.median_time_past = block.header.timestamp;

    consensus::TxValidationOutput result = consensus::validateCoinbase(coinbase, ctx);

    if (result.result != consensus::TxValidationResult::OK) {
        error = "Invalid coinbase transaction";
        return false;
    }

    // Validate coinbase value
    uint64_t block_subsidy = BlockTemplateBuilder::getBlockSubsidy(expected_height);

    // Calculate total fees from non-coinbase transactions.
    // CT/ring txs (HasExplicitFee) carry the fee in the explicit_fee field because
    // their inputs are opaque Pedersen commitments — the coin view holds value=0 for
    // those UTXOs and cannot compute total_in - total_out.
    uint64_t total_fees = 0;
    consensus::CoinsViewCache view(&coins_db_);

    for (size_t i = 1; i < block.vtx.size(); i++) {
        const auto& tx = block.vtx[i];

        if (tx.HasExplicitFee()) {
            // CT/ring tx: fee is committed in the explicit_fee field set by the wallet builder.
            total_fees += tx.GetExplicitFee();
        } else {
            // Transparent tx: fee = total_in - total_out via the UTXO coin view.
            uint64_t total_in = 0;
            for (const auto& input : tx.vin) {
                consensus::OutPoint outpoint(input.prevout.txid, input.prevout.vout);
                auto coin_result = view.getCoin(outpoint);
                if (coin_result.ok()) {
                    total_in += coin_result.value().value;
                }
            }

            uint64_t total_out = 0;
            for (const auto& output : tx.vout) {
                total_out += output.value.GetUna();
            }

            if (total_in >= total_out) {
                total_fees += (total_in - total_out);
            }
        }
    }

    // Check coinbase value
    uint64_t coinbase_value = 0;
    for (const auto& output : coinbase.vout) {
        coinbase_value += output.value.GetUna();
    }

    if (coinbase_value > block_subsidy + total_fees) {
        error = "Coinbase value exceeds subsidy + fees";
        return false;
    }

    return true;
}

bool BlockSubmissionValidator::validateTransactions(
    const Block& block,
    uint32_t expected_height,
    std::string& error
) {
    // Validate all non-coinbase transactions
    consensus::CoinsViewCache view(&coins_db_);

    // Phase 23.3: Create TxValidationContext with BIP68 MTP lookup
    consensus::TxValidationContext ctx;
    ctx.block_height = expected_height;
    ctx.median_time_past = block.header.timestamp;
    ctx.check_sequence_locks = true;

    // Wire MTP lookup for BIP68 time-based sequence locks
    // If chain_db_ is nullptr, time-based locks will fail-closed (be rejected)
    ctx.mtp_at_height = consensus::CreateMtpLookup(chain_db_);

    for (size_t i = 1; i < block.vtx.size(); i++) {
        const auto& tx = block.vtx[i];

        consensus::TxValidationOutput result = consensus::validateTransaction(tx, view, ctx, false);

        if (result.result != consensus::TxValidationResult::OK) {
            error = "Invalid transaction in block";
            return false;
        }

        // Add transaction outputs to view (for subsequent tx validation)
        // This allows transactions in the same block to spend each other
        std::string txid = tx.GetTxid();
        for (uint32_t vout = 0; vout < tx.vout.size(); vout++) {
            consensus::OutPoint outpoint(txid, vout);
            consensus::UTXOEntry entry;
            entry.value = tx.vout[vout].value;
            entry.scriptPubKey = tx.vout[vout].scriptPubKey;
            entry.height = expected_height;
            entry.isCoinbase = false;

            view.addCoin(outpoint, entry);
        }
    }

    return true;
}

} // namespace mining
} // namespace dinero
