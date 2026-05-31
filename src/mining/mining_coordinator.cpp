/**
 * Phase 26.8: Mining Coordinator Implementation
 *
 * Central orchestrator for all mining activities.
 * Integrates CPU/GPU miners, Stratum V1/V2, and external miners.
 */

#include "mining/mining_coordinator.h"
#include "mining/block_template.h"       // Phase 28: For buildMerkleTree
#include "mining/header_layout.h"        // Unified header constants (128-byte BlockHeader v1)
#include "mining/address_validator.h"    // Taproot address validation
#include "daemon/daemon_context.h"
#include "daemon/services/chainstate_service.h"
#include "daemon/services/mempool_service.h"
#include "daemon/block_acceptor.h"
#include "common/logger.h"
#include "common/sha256d.h"
#include "common/json_adapter.h"
#include "common/address_script_builder.h"  // BuildScriptPubKeyFromAddress
#include "primitives/amount.h"
#include "consensus/pow.hpp"               // GetNextWorkRequired, BitsHex
#include "storage/chain_direct.h"          // GetMedianTimePast
#include "consensus/consensus.hpp"         // Consensus struct
#include "consensus/subsidy.hpp"           // GetBlockSubsidy (canonical)
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <cmath>

namespace dinero {
namespace mining {

// ============================================================================
// Helper: Convert pool difficulty to compact bits
// ============================================================================

namespace {

/**
 * Convert a pool difficulty value to compact bits format.
 *
 * Pool difficulty is expressed as a multiplier of difficulty-1.
 * difficulty=1 means the standard difficulty-1 target (0x1d00ffff in Bitcoin).
 * Higher difficulty = smaller target = harder to find valid shares.
 *
 * Formula: target = difficulty_1_target / difficulty
 * Then convert target to compact bits format.
 */
uint32_t difficultyToBits(double difficulty) {
    if (difficulty <= 0.0) {
        // Invalid difficulty, return maximum target (easiest)
        return dinero::ASERTConsensus::DIFFICULTY_1_BITS;
    }

    if (difficulty <= 1.0) {
        // Difficulty <= 1 means difficulty-1 target or easier
        return dinero::ASERTConsensus::DIFFICULTY_1_BITS;
    }

    // Standard difficulty-1 bits: 0x1d00ffff
    // exponent = 0x1d (29), mantissa = 0x00ffff
    const uint32_t diff1_bits = dinero::ASERTConsensus::DIFFICULTY_1_BITS;
    const uint8_t diff1_exp = (diff1_bits >> 24) & 0xFF;
    const uint32_t diff1_mant = diff1_bits & 0x00FFFFFF;

    // Target at difficulty 1: mantissa * 2^(8*(exp-3))
    // New target = diff1_target / difficulty
    // We need to express this in compact form

    // Calculate how many bits to shift down (difficulty = 2^shift approximately)
    double log2_diff = std::log2(difficulty);
    int bit_shift = static_cast<int>(log2_diff);

    // Adjust exponent: each 8 bits of shift reduces exponent by 1
    int exp_reduction = bit_shift / 8;
    int remaining_shift = bit_shift % 8;

    int new_exp = static_cast<int>(diff1_exp) - exp_reduction;
    if (new_exp < 3) {
        new_exp = 3;  // Minimum exponent (3 bytes for mantissa)
    }

    // Adjust mantissa for remaining shift
    double mantissa_divisor = std::pow(2.0, remaining_shift) * (difficulty / std::pow(2.0, bit_shift));
    uint32_t new_mant = static_cast<uint32_t>(diff1_mant / mantissa_divisor);

    // Ensure mantissa fits in 3 bytes
    if (new_mant > 0x7FFFFF) {
        new_mant = 0x7FFFFF;
    }
    if (new_mant == 0) {
        new_mant = 1;  // Minimum mantissa
    }

    return (static_cast<uint32_t>(new_exp) << 24) | new_mant;
}

} // anonymous namespace

// ============================================================================
// MiningCoordinator Implementation
// ============================================================================

MiningCoordinator::MiningCoordinator(DaemonContext* daemon_ctx)
    : daemon_ctx_(daemon_ctx)
    , job_counter_(0)
    , total_shares_(0)
    , total_blocks_(0)
{
    if (!daemon_ctx_) {
        throw std::runtime_error("MiningCoordinator: DaemonContext is null");
    }

    // Get services
    auto chainstate = std::dynamic_pointer_cast<ChainstateService>(daemon_ctx_->chainstate);
    if (!chainstate) {
        throw std::runtime_error("MiningCoordinator: ChainstateService not available");
    }

    auto mempool_service = std::dynamic_pointer_cast<MempoolService>(daemon_ctx_->mempool);
    if (!mempool_service) {
        throw std::runtime_error("MiningCoordinator: MempoolService not available");
    }

    g_logger.info("[MiningCoordinator] Initialized");
}

MiningCoordinator::~MiningCoordinator() {
    g_logger.info("[MiningCoordinator] Shutting down");
}

std::string MiningCoordinator::generateJobId() {
    uint64_t counter = job_counter_.fetch_add(1);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << counter;
    return oss.str();
}

std::string MiningCoordinator::generateExtranonce1(const std::string& worker_id) {
    // Generate unique extranonce1 for this worker
    // Use worker_id hash + timestamp
    std::hash<std::string> hasher;
    size_t hash = hasher(worker_id);
    uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

    uint64_t extranonce = (hash & 0xFFFFFFFF) | (timestamp << 32);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(8) << (extranonce & 0xFFFFFFFF);
    return oss.str();
}

std::shared_ptr<MiningJob> MiningCoordinator::createJob(const std::string& mining_address) {
    // ========================================================================
    // OPTION B: CLEAN REWRITE - Production Architecture Only
    // ========================================================================
    // This version:
    // ✅ Uses PRODUCTION mempool (dinero::Mempool) from MempoolService
    // ✅ Calculates ACTUAL difficulty using GetNextWorkRequired
    // No Phase 25 dependencies (legacy mempool, BlockTemplateBuilder)
    // ✅ Direct integration with daemon services
    // ========================================================================

    // Phase 1: Get services and blockchain state
    // -------------------------------------------

    auto chainstate = std::dynamic_pointer_cast<ChainstateService>(daemon_ctx_->chainstate);
    if (!chainstate) {
        g_logger.error("[MiningCoordinator] Chainstate service not available");
        return nullptr;
    }

    auto mempool_service = std::dynamic_pointer_cast<MempoolService>(daemon_ctx_->mempool);
    if (!mempool_service) {
        g_logger.error("[MiningCoordinator] Mempool service not available");
        return nullptr;
    }

    ChainDB* chain_db = chainstate->chainDB();
    if (!chain_db) {
        g_logger.error("[MiningCoordinator] Chain database not available");
        return nullptr;
    }

    // Get current tip
    auto tip_result = chain_db->getTip();
    if (!tip_result.ok()) {
        g_logger.error("[MiningCoordinator] Failed to get blockchain tip");
        return nullptr;
    }

    auto tip_info = tip_result.value();
    uint32_t current_height = tip_info.height;
    std::string prev_hash = tip_info.hash;
    uint32_t next_height = current_height + 1;

    if (prev_hash.empty() || current_height == 0) {
        g_logger.error("[MiningCoordinator] Cannot create job: blockchain has no blocks");
        return nullptr;
    }

    // Phase 2: Get transactions from PRODUCTION mempool
    // --------------------------------------------------

    // V5 freeze fork: pass next_block_height so the mempool filters out
    // CT / v3 / v4 / non-Taproot txs that would fail post-activation.
    std::vector<Transaction> selected_txs = mempool_service->mempool().selectTransactionsForBlock(
        1000000,      // max_block_size (1 MB)
        4000000,      // max_block_weight (4M WU)
        next_height   // freeze-fork target height
    );

    // Calculate total fees
    uint64_t total_fees = 0;
    for (const auto& tx : selected_txs) {
        // Get fee from mempool entry
        auto entry_opt = mempool_service->mempool().getMempoolEntry(tx.GetTxid());
        if (entry_opt.has_value()) {
            total_fees += entry_opt.value().fee;
        }
    }

    g_logger.info("[MiningCoordinator] Selected " + std::to_string(selected_txs.size()) +
                  " transactions from mempool, total fees: " + std::to_string(total_fees / 100000000.0) + " DIN");

    // Phase 3: Build coinbase transaction
    // ------------------------------------

    // BuildCoinbaseForHeight returns Tx, convert to Transaction
    // For now, create a simple coinbase - TODO: use BuildCoinbaseForHeight properly
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;

    // Coinbase input (BIP 34: height in scriptSig)
    TxInput coinbase_in;
    coinbase_in.prevout.txid = std::string(64, '0');
    coinbase_in.prevout.vout = 0xFFFFFFFF;
    // Simple scriptSig with height (BIP 34)
    coinbase_in.scriptSig = {0x03};  // OP_PUSHDATA
    uint32_t h = next_height;
    coinbase_in.scriptSig.push_back(h & 0xFF);
    coinbase_in.scriptSig.push_back((h >> 8) & 0xFF);
    coinbase_in.scriptSig.push_back((h >> 16) & 0xFF);
    coinbase.vin.push_back(coinbase_in);

    // Coinbase output (subsidy + fees to mining address)
    int64_t subsidy = GetBlockSubsidy(next_height);
    auto coinbase_value = dinero::CheckedAddUna(static_cast<uint64_t>(subsidy), total_fees);
    if (!coinbase_value) {
        g_logger.error("[MiningCoordinator] Coinbase value overflow");
        return nullptr;
    }
    TxOutput coinbase_out;
    coinbase_out.value = AmountUna::Una(*coinbase_value);

    // v7: accept Taproot (din1p...) or P2MR (din1r...) coinbase destinations.
    if (!IsCoinbaseEligibleAddress(mining_address)) {
        g_logger.error("[MiningCoordinator] Mining requires Taproot (din1p...) or P2MR (din1r...) address. Got: " + mining_address);
        return nullptr;
    }

    std::vector<uint8_t> script_pubkey;
    std::string addr_error;
    if (!dinero::BuildScriptPubKeyFromAddress(mining_address, script_pubkey, addr_error)) {
        g_logger.error("[MiningCoordinator] Failed to build scriptPubKey from address: " +
                       mining_address + " - " + addr_error);
        return nullptr;
    }

    // Sanity check: P2TR = OP_1(0x51) + PUSH32(0x20) + 32 bytes, total 34.
    //               P2MR = OP_3(0x53) + PUSH32(0x20) + 32 bytes, total 34.
    const bool is_p2tr = script_pubkey.size() == 34 && script_pubkey[0] == 0x51 && script_pubkey[1] == 0x20;
    const bool is_p2mr = script_pubkey.size() == 34 && script_pubkey[0] == 0x53 && script_pubkey[1] == 0x20;
    if (!is_p2tr && !is_p2mr) {
        g_logger.error("[MiningCoordinator] Address produced non-coinbase-eligible script. Expected P2TR or P2MR (34 bytes).");
        return nullptr;
    }

    coinbase_out.scriptPubKey = script_pubkey;
    coinbase.vout.push_back(coinbase_out);

    // Phase 4: Assemble block transactions
    // -------------------------------------

    std::vector<Transaction> block_txs;
    block_txs.reserve(1 + selected_txs.size());
    block_txs.push_back(coinbase);  // Coinbase is always first
    block_txs.insert(block_txs.end(), selected_txs.begin(), selected_txs.end());

    // Phase 5: Calculate merkle root
    // -------------------------------

    std::string merkle_root;
    std::vector<std::string> merkle_branch;
    if (!BlockTemplateBuilder::buildMerkleTree(block_txs, merkle_root, merkle_branch)) {
        g_logger.error("[MiningCoordinator] Failed to build merkle tree - duplicate txids detected");
        return nullptr;
    }

    // Phase 6: Calculate ACTUAL network difficulty
    // ---------------------------------------------

    // Get previous block header for difficulty calculation
    uint256 prev_hash_uint256 = uint256::FromHex(prev_hash);
    auto prev_header_result = chain_db->getHeader(prev_hash_uint256);
    if (!prev_header_result.ok()) {
        g_logger.error("[MiningCoordinator] Failed to get previous block header");
        return nullptr;
    }

    auto prev_header = prev_header_result.value();
    uint32_t prev_bits = prev_header.difficulty;

    // Calculate median time past (MTP) for previous block and current block
    // CRITICAL: the timestamp handed to miners and the timestamp used for ASERT
    // must be identical. Reusing MTP here and a later wall-clock timestamp in the
    // job causes bad-diffbits blocks under ASERT.
    uint64_t current_mtp = storage::GetMedianTimePast(chain_db);

    // Phase 7: Build MiningJob structure
    // -----------------------------------

    // Phase 34.3: Bitcoin-correct timestamp formula (BIP113 compliance)
    // Rule: block.nTime = max(GetTime(), MedianTimePast + 1)
    uint64_t current_time = static_cast<uint64_t>(std::time(nullptr));
    uint64_t timestamp = std::max(current_time, static_cast<uint64_t>(current_mtp + 1));

    const Consensus consensus_params = GetConsensusForCurrentNetwork();
    uint32_t bits = GetNextWorkRequiredWithChainDB(
        static_cast<int32_t>(next_height),
        static_cast<int64_t>(timestamp),
        consensus_params,
        chain_db);

    if (bits == 0) {
        g_logger.error("[MiningCoordinator] GetNextWorkRequired returned invalid bits=0");
        return nullptr;
    }

    g_logger.info("[MiningCoordinator] Difficulty calculated: bits=" + BitsHex(bits) +
                  " (prev_bits=" + BitsHex(prev_bits) + ")");

    g_logger.info("[MiningCoordinator] Timestamp: now=" + std::to_string(current_time) +
                 ", mtp=" + std::to_string(current_mtp) +
                 ", timestamp=" + std::to_string(timestamp));

    auto job = std::make_shared<MiningJob>();
    job->job_id = generateJobId();
    job->version = 1;  // Block version
    job->prev_hash = prev_hash;
    job->merkle_root = merkle_root;
    job->timestamp = timestamp;  // ✅ Bitcoin-correct: max(GetTime(), MTP + 1)
    job->bits = bits;  // ✅ ACTUAL difficulty (not hardcoded!)
    job->height = next_height;

    // Extranonce configuration (Stratum V1)
    job->extranonce1 = "";  // Will be set per-worker
    job->extranonce2_size = 4;  // 4 bytes for extranonce2
    job->merkle_branch = merkle_branch;

    // Coinbase transaction - full hex
    std::string coinbase_serialized = coinbase.Serialize();
    job->coinbase_tx_hex = dinero::jj::toHex(
        reinterpret_cast<const uint8_t*>(coinbase_serialized.data()),
        coinbase_serialized.size()
    );

    // Stratum V1: Split coinbase into coinbase1 and coinbase2
    // The extranonce goes inside the coinbase scriptSig, right after the BIP34 height.
    // Coinbase structure (serialized):
    //   [version: 4B][input_count: 1B][prevout_txid: 32B][prevout_index: 4B]
    //   [scriptSig_len: 1-9B][scriptSig: variable][sequence: 4B]
    //   [output_count: 1B][outputs...][locktime: 4B]
    //
    // scriptSig typically contains:
    //   [0x03][height: 3B] (BIP34 height push) + [extranonce placeholder] + [optional extra data]
    //
    // For our coinbase, scriptSig = [0x03][3 bytes height] = 4 bytes
    // Split point is after the scriptSig (where extranonce would be inserted)
    //
    // coinbase1 = version + input_count + prevout + scriptSig_len_byte + height_push
    // coinbase2 = sequence + outputs + locktime
    //
    // Offsets (in bytes):
    //   version:        0-3   (4 bytes)
    //   input_count:    4     (1 byte, value=1 for coinbase)
    //   prevout_txid:   5-36  (32 bytes)
    //   prevout_index:  37-40 (4 bytes, value=0xFFFFFFFF)
    //   scriptSig_len:  41    (1 byte, varint - for small scripts it's 1 byte)
    //   scriptSig:      42+   (variable, our basic one is 4 bytes: 0x03 + 3-byte height)
    //
    // For Stratum: extranonce_size = extranonce1_size + extranonce2_size
    // We need to account for this in scriptSig length when splitting

    // Calculate scriptSig content (what's currently in the coinbase)
    size_t scriptsig_len = coinbase.vin[0].scriptSig.size();

    // coinbase1 ends right after the BIP34 height push (4 bytes: 0x03 + 3-byte height)
    // This is where extranonce1 + extranonce2 will be inserted
    // But the scriptSig length byte needs to be updated to include extranonces
    //
    // For simplicity, we'll split at a fixed offset:
    // coinbase1 = bytes 0 through (41 + scriptsig_len - 1) = all of scriptSig
    // But for Stratum, we want to split INSIDE scriptSig after height, before extranonce

    // Recalculate with extranonce space:
    // New scriptSig = [height_push(4)] + [extranonce1] + [extranonce2] + [optional_data]
    size_t extranonce1_size = 4;  // Bytes for extranonce1 (configurable)
    size_t extranonce2_size_bytes = job->extranonce2_size;  // Already set to 4

    // Build coinbase1: everything up to where extranonce goes
    // = version(4) + input_count(1) + prevout_txid(32) + prevout_index(4)
    //   + new_scriptSig_len(1) + height_push(4)
    // But we need to update scriptSig_len to account for extranonces

    size_t new_scriptsig_len = scriptsig_len + extranonce1_size + extranonce2_size_bytes;
    if (new_scriptsig_len > 252) {
        g_logger.error("[MiningCoordinator] scriptSig too large for varint encoding");
        return nullptr;
    }

    // Build coinbase1 hex:
    // version(4) + input_count(1) + prevout_txid(32) + prevout_index(4) + new_scriptsig_len(1) + original_scriptsig
    std::ostringstream cb1;
    // Version (4 bytes LE)
    cb1 << std::hex << std::setfill('0') << std::setw(2) << (coinbase.version & 0xFF);
    cb1 << std::setw(2) << ((coinbase.version >> 8) & 0xFF);
    cb1 << std::setw(2) << ((coinbase.version >> 16) & 0xFF);
    cb1 << std::setw(2) << ((coinbase.version >> 24) & 0xFF);
    // Input count (1 byte, always 1 for coinbase)
    cb1 << std::setw(2) << 1;
    // Prevout txid (32 bytes of zeros)
    for (int i = 0; i < 32; i++) cb1 << "00";
    // Prevout index (4 bytes, 0xFFFFFFFF)
    cb1 << "ffffffff";
    // New scriptSig length (1 byte varint)
    cb1 << std::setw(2) << static_cast<int>(new_scriptsig_len);
    // Original scriptSig content (height push)
    for (uint8_t byte : coinbase.vin[0].scriptSig) {
        cb1 << std::setw(2) << static_cast<int>(byte);
    }
    job->coinbase1 = cb1.str();

    // Build coinbase2 hex:
    // sequence(4) + output_count + outputs + locktime(4)
    std::ostringstream cb2;
    // Sequence (4 bytes, 0xFFFFFFFF)
    cb2 << std::hex << std::setfill('0') << "ffffffff";
    // Output count (varint)
    cb2 << std::setw(2) << coinbase.vout.size();
    // Each output: value(8) + scriptPubKey_len + scriptPubKey
    for (const auto& output : coinbase.vout) {
        // Value (8 bytes LE)
        uint64_t val = output.value;
        for (int i = 0; i < 8; i++) {
            cb2 << std::setw(2) << ((val >> (8 * i)) & 0xFF);
        }
        // ScriptPubKey length (varint)
        cb2 << std::setw(2) << output.scriptPubKey.size();
        // ScriptPubKey bytes
        for (uint8_t byte : output.scriptPubKey) {
            cb2 << std::setw(2) << static_cast<int>(byte);
        }
    }
    // Locktime (4 bytes LE)
    cb2 << std::setw(2) << (coinbase.lockTime & 0xFF);
    cb2 << std::setw(2) << ((coinbase.lockTime >> 8) & 0xFF);
    cb2 << std::setw(2) << ((coinbase.lockTime >> 16) & 0xFF);
    cb2 << std::setw(2) << ((coinbase.lockTime >> 24) & 0xFF);
    job->coinbase2 = cb2.str();

    g_logger.debug("[MiningCoordinator] Coinbase split: cb1_len=" + std::to_string(job->coinbase1.size()) +
                  " cb2_len=" + std::to_string(job->coinbase2.size()));

    // Calculate coinbase value (subsidy + fees) - already calculated earlier
    job->coinbase_value = *coinbase_value;

    // Difficulty metrics
    job->share_difficulty = 1.0;  // Default pool difficulty
    job->network_difficulty = MiningEngine::calculateDifficulty(bits);

    // Store selected transactions (excluding coinbase) for block reconstruction
    job->transactions = selected_txs;

    // Metadata
    job->created_at = static_cast<uint64_t>(std::time(nullptr));
    job->clean_jobs = true;  // Abandon previous jobs

    // Phase 8: Update shared state (with lock)
    // -----------------------------------------

    {
        std::lock_guard<std::mutex> lock(job_mutex_);
        current_job_ = job;
    }

    g_logger.info("[MiningCoordinator] ✅ Created job " + job->job_id +
                  " for height " + std::to_string(job->height) +
                  " with " + std::to_string(selected_txs.size()) + " transactions" +
                  " (bits=" + BitsHex(bits) + ")");

    return job;
}

std::shared_ptr<MiningJob> MiningCoordinator::getCurrentJob() const {
    std::lock_guard<std::mutex> lock(job_mutex_);
    return current_job_;
}

void MiningCoordinator::refreshJobs() {
    std::lock_guard<std::mutex> lock(job_mutex_);
    // Invalidate current job
    current_job_.reset();
    g_logger.info("[MiningCoordinator] Jobs refreshed (new block arrived)");
}

bool MiningCoordinator::validateShare(ShareSubmission& share, const MiningJob& job, const std::string& extranonce1) {
    // 1. Check job ID matches
    if (share.job_id != job.job_id) {
        g_logger.warning("[MiningCoordinator] Share for unknown job: " + share.job_id);
        return false;
    }

    // 2. Validate extranonce2 size
    if (share.extranonce2.size() / 2 != job.extranonce2_size) {
        g_logger.warning("[MiningCoordinator] Invalid extranonce2 size");
        return false;
    }

    // ASERT difficulty depends on the candidate block timestamp, so Stratum-style
    // ntime rolling cannot be treated as free. Lock shares to the timestamp that
    // was used when the job difficulty was computed.
    if (share.timestamp != job.timestamp) {
        g_logger.warning("[MiningCoordinator] Rejected share with timestamp drift: submitted=" +
                         std::to_string(share.timestamp) +
                         " job=" + std::to_string(job.timestamp));
        return false;
    }

    // 3. Reconstruct coinbase with extranonce1 + extranonce2
    // Stratum V1: coinbase = coinbase1 + extranonce1 + extranonce2 + coinbase2
    std::string full_coinbase;
    if (!job.coinbase1.empty() && !job.coinbase2.empty()) {
        // Stratum V1 mode: construct from split coinbase parts
        full_coinbase = job.coinbase1 + extranonce1 + share.extranonce2 + job.coinbase2;
    } else {
        // Fallback: use full coinbase_tx_hex (non-Stratum or internal mining)
        full_coinbase = job.coinbase_tx_hex;
    }

    // Convert hex string to bytes and hash the coinbase transaction (double SHA256)
    std::vector<uint8_t> coinbase_bytes;
    coinbase_bytes.reserve(full_coinbase.size() / 2);
    for (size_t i = 0; i + 1 < full_coinbase.size(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(std::strtol(full_coinbase.substr(i, 2).c_str(), nullptr, 16));
        coinbase_bytes.push_back(byte);
    }
    std::string coinbase_hash = Dinero::Common::double_sha256(coinbase_bytes);

    // 4. Compute merkle root using merkle branches
    std::string current_hash = coinbase_hash;
    for (const auto& branch : job.merkle_branch) {
        // Concatenate current_hash + branch and double-SHA256
        std::vector<uint8_t> combined;

        // Convert current_hash (hex) to bytes
        for (size_t i = 0; i < current_hash.size(); i += 2) {
            uint8_t byte = static_cast<uint8_t>(std::strtol(current_hash.substr(i, 2).c_str(), nullptr, 16));
            combined.push_back(byte);
        }
        // Convert branch (hex) to bytes
        for (size_t i = 0; i < branch.size(); i += 2) {
            uint8_t byte = static_cast<uint8_t>(std::strtol(branch.substr(i, 2).c_str(), nullptr, 16));
            combined.push_back(byte);
        }

        current_hash = Dinero::Common::double_sha256(combined);
    }
    std::string computed_merkle_root = current_hash;

    // 5. Build block header
    // Using the 128-byte header format (BlockHeader v1)
    BlockHeader header;
    header.version = job.version;
    header.prev_block_hash = uint256::FromHexUnsafe(job.prev_hash);
    header.merkle_root = uint256::FromHexUnsafe(computed_merkle_root);
    header.timestamp = job.timestamp;
    header.difficulty = job.bits;
    header.nonce = share.nonce;

    // 6. Compute block hash (double SHA256 of header)
    MiningEngine engine;
    std::string block_hash = engine.hashBlockHeader(header);
    share.block_hash = block_hash;

    // 7. Check against share difficulty
    // Convert share difficulty to a target and compare
    // share_difficulty is a multiplier of difficulty-1 target
    // For simplicity, use bits comparison (higher bits = easier target)
    bool meets_share = MiningEngine::checkProofOfWork(block_hash,
        difficultyToBits(job.share_difficulty));
    share.meets_share_target = meets_share;

    if (!meets_share) {
        g_logger.debug("[MiningCoordinator] Share does not meet share difficulty");
        return false;
    }

    // 8. Check against network difficulty
    bool meets_network = MiningEngine::checkProofOfWork(block_hash, job.bits);
    share.meets_network_target = meets_network;

    if (meets_network) {
        g_logger.info("[MiningCoordinator] Share meets NETWORK difficulty - valid block candidate!");
    }

    return true;
}

bool MiningCoordinator::checkProofOfWork(const std::string& block_hash, uint32_t bits) {
    return MiningEngine::checkProofOfWork(block_hash, bits);
}

bool MiningCoordinator::submitShare(
    ShareSubmission& share,
    const std::string& worker_id,
    WorkerType worker_type
) {
    total_shares_.fetch_add(1);

    // Get current job
    auto job = getCurrentJob();
    if (!job) {
        g_logger.warning("[MiningCoordinator] No current job for share submission");
        return false;
    }

    // Look up worker's extranonce1
    std::string extranonce1;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        auto it = workers_.find(worker_id);
        if (it == workers_.end()) {
            g_logger.warning("[MiningCoordinator] Unknown worker: " + worker_id);
            return false;
        }
        extranonce1 = it->second.extranonce1;
    }

    // Validate share
    if (!validateShare(share, *job, extranonce1)) {
        // Update worker stats (rejected)
        std::lock_guard<std::mutex> lock(workers_mutex_);
        if (workers_.find(worker_id) != workers_.end()) {
            workers_[worker_id].shares_rejected++;
        }
        return false;
    }

    // Update worker stats (accepted)
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        if (workers_.find(worker_id) != workers_.end()) {
            workers_[worker_id].shares_accepted++;
            workers_[worker_id].last_share_time = static_cast<uint64_t>(std::time(nullptr));
        }
    }

    // Check if this meets network target (is a valid block)
    if (share.meets_network_target) {
        g_logger.info("[MiningCoordinator] Valid block found by worker " + worker_id + "!");

        // Reconstruct full block (pass extranonce1 for proper coinbase construction)
        Block block = reconstructBlock(share, *job, extranonce1);

        // Submit block
        if (submitBlock(block)) {
            total_blocks_.fetch_add(1);

            // Update worker stats (block found)
            std::lock_guard<std::mutex> lock(workers_mutex_);
            if (workers_.find(worker_id) != workers_.end()) {
                workers_[worker_id].blocks_found++;
            }

            g_logger.info("[MiningCoordinator] Block submitted successfully!");
            return true;
        } else {
            g_logger.error("[MiningCoordinator] Block submission failed");
            return false;
        }
    }

    return true;
}

Block MiningCoordinator::reconstructBlock(const ShareSubmission& share, const MiningJob& job, const std::string& extranonce1) {
    // Reconstruct full block from share submission
    Block block;

    // 1. Reconstruct coinbase with extranonces
    std::string full_coinbase_hex;
    if (!job.coinbase1.empty() && !job.coinbase2.empty()) {
        full_coinbase_hex = job.coinbase1 + extranonce1 + share.extranonce2 + job.coinbase2;
    } else {
        full_coinbase_hex = job.coinbase_tx_hex;
    }

    // Parse coinbase from hex
    Transaction coinbase;
    if (!TransactionSerializer::Deserialize(coinbase, full_coinbase_hex)) {
        g_logger.error("[MiningCoordinator] Failed to deserialize coinbase in reconstructBlock");
        // Return partial block - will fail validation but won't crash
        block.header.version = job.version;
        block.header.prev_block_hash = uint256::FromHexUnsafe(job.prev_hash);
        block.header.merkle_root = uint256::FromHexUnsafe(job.merkle_root);
        block.header.timestamp = job.timestamp;
        block.header.difficulty = job.bits;
        block.header.nonce = share.nonce;
        return block;
    }

    // 2. Compute merkle root with the new coinbase
    // Hash the coinbase transaction
    std::vector<uint8_t> coinbase_bytes;
    for (size_t i = 0; i + 1 < full_coinbase_hex.size(); i += 2) {
        uint8_t byte = static_cast<uint8_t>(std::strtol(full_coinbase_hex.substr(i, 2).c_str(), nullptr, 16));
        coinbase_bytes.push_back(byte);
    }
    std::string coinbase_hash = Dinero::Common::double_sha256(coinbase_bytes);

    // Apply merkle branches
    std::string merkle_root = coinbase_hash;
    for (const auto& branch : job.merkle_branch) {
        std::vector<uint8_t> combined;
        for (size_t i = 0; i + 1 < merkle_root.size(); i += 2) {
            combined.push_back(static_cast<uint8_t>(std::strtol(merkle_root.substr(i, 2).c_str(), nullptr, 16)));
        }
        for (size_t i = 0; i + 1 < branch.size(); i += 2) {
            combined.push_back(static_cast<uint8_t>(std::strtol(branch.substr(i, 2).c_str(), nullptr, 16)));
        }
        merkle_root = Dinero::Common::double_sha256(combined);
    }

    // 3. Build block header
    block.header.version = job.version;
    block.header.prev_block_hash = uint256::FromHexUnsafe(job.prev_hash);
    block.header.merkle_root = uint256::FromHexUnsafe(merkle_root);
    block.header.timestamp = job.timestamp;
    block.header.difficulty = job.bits;
    block.header.nonce = share.nonce;

    // 4. Add coinbase as first transaction, then all other transactions
    block.vtx.push_back(coinbase);
    for (const auto& tx : job.transactions) {
        block.vtx.push_back(tx);
    }

    g_logger.debug("[MiningCoordinator] Reconstructed block with " +
                  std::to_string(block.vtx.size()) + " transactions");

    return block;
}

bool MiningCoordinator::submitBlock(const Block& block) {
    // Serialize block to hex
    std::string block_binary = block.Serialize();
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (unsigned char b : block_binary) {
        hex << std::setw(2) << static_cast<int>(b);
    }

    // Submit via BlockAcceptor
    auto accept_result = BlockAcceptor::AcceptBlockFromRPC(hex.str(), "coordinator");

    if (!accept_result.ok) {
        g_logger.error("[MiningCoordinator] Failed to accept block: " + accept_result.message);
        return false;
    }

    g_logger.info("[MiningCoordinator] Block accepted at height " + std::to_string(accept_result.newHeight));

    // Refresh jobs (new block means old templates are invalid)
    refreshJobs();

    return true;
}

void MiningCoordinator::registerWorker(const std::string& worker_id, WorkerType type) {
    std::lock_guard<std::mutex> lock(workers_mutex_);

    WorkerStats stats;
    stats.worker_id = worker_id;
    stats.type = type;
    stats.extranonce1 = generateExtranonce1(worker_id);  // Assign unique extranonce1 per-session
    stats.shares_accepted = 0;
    stats.shares_rejected = 0;
    stats.blocks_found = 0;
    stats.hashrate = 0.0;
    stats.difficulty = 1.0;
    stats.last_share_time = 0;

    workers_[worker_id] = stats;

    g_logger.info("[MiningCoordinator] Worker registered: " + worker_id +
                  " (extranonce1=" + stats.extranonce1 + ")");
}

void MiningCoordinator::unregisterWorker(const std::string& worker_id) {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    workers_.erase(worker_id);
    g_logger.info("[MiningCoordinator] Worker unregistered: " + worker_id);
}

std::vector<MiningCoordinator::WorkerStats> MiningCoordinator::getWorkerStats() const {
    std::lock_guard<std::mutex> lock(workers_mutex_);

    std::vector<WorkerStats> stats;
    for (const auto& [worker_id, worker_stats] : workers_) {
        stats.push_back(worker_stats);
    }

    return stats;
}

void MiningCoordinator::setWorkerDifficulty(const std::string& worker_id, double difficulty) {
    std::lock_guard<std::mutex> lock(workers_mutex_);

    if (workers_.find(worker_id) != workers_.end()) {
        workers_[worker_id].difficulty = difficulty;
        g_logger.info("[MiningCoordinator] Worker " + worker_id + " difficulty set to " + std::to_string(difficulty));
    }
}

double MiningCoordinator::calculateRecommendedDifficulty(
    const std::string& worker_id,
    double hashrate
) {
    // Target: 1 share every 15 seconds
    constexpr double TARGET_SHARE_TIME = 15.0;

    // Difficulty = hashrate * target_time / 2^32
    // (for SHA256d, difficulty 1 = 2^32 hashes on average)
    double difficulty = (hashrate * TARGET_SHARE_TIME) / 4294967296.0;

    // Clamp to reasonable range
    if (difficulty < 1.0) difficulty = 1.0;
    if (difficulty > 1000000.0) difficulty = 1000000.0;

    return difficulty;
}

MiningCoordinator::CoordinatorStats MiningCoordinator::getStats() const {
    CoordinatorStats stats;

    stats.total_shares = total_shares_.load();
    stats.total_blocks = total_blocks_.load();

    // Calculate total hashrate from all workers
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        stats.active_workers = workers_.size();

        double total_hashrate = 0.0;
        for (const auto& [worker_id, worker_stats] : workers_) {
            total_hashrate += worker_stats.hashrate;
        }
        stats.total_hashrate = total_hashrate;
    }

    // Get current job info
    auto job = getCurrentJob();
    if (job) {
        stats.current_job_id = job->job_id;
        stats.current_height = job->height;
    } else {
        stats.current_job_id = "";
        stats.current_height = 0;
    }

    return stats;
}

} // namespace mining
} // namespace dinero
