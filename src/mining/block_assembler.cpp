#include "mining/block_assembler.h"
#include "daemon/mempool.h"  // Week 7: Mempool for transaction selection (full definition)
#include "consensus/difficulty.h"
#include "consensus/outpoint.h"  // v2.3.0: OutPoint for IUTXOProvider interface
#include "consensus/subsidy.h"  // Canonical subsidy schedule
#include "consensus/consensus.hpp"
#include "consensus/pow.hpp"
#include "consensus/chainparams.h"  // For Params()
#include "consensus/merkle_root.h"  // Phase 11a.2: Canonical merkle computation
#include "consensus/witness_commitment.h"  // Phase 11c.1: Witness commitment structure
#include "consensus/pq/scheme_registry.h"  // Phase 8.5 Commit 2: per-scheme composition caps
#include "consensus/filter_commitment.h"  // DNRF coinbase filter commitment
#include "consensus/block_filter.h"       // BIP158 GCS filter construction
#include "consensus/utreexo_activation.h"  // For FullRulesActive()
#include "consensus/utreexo_canonical_roots_activation.h"  // Apr 13 2026 Stage 3 fork
#include "consensus/block_validation.h"  // For BlockValidator::ComputeUtreexoRootPure
#include "consensus/script.h"  // For Script::pushInt64 (BIP34 height encoding)
// Phase 39: chain_manager.h deleted (ChainManager removed)
#include "daemon/mining_payout_resolver.h"
#include "daemon/block_relay_manager.h"  // Phase W.1.3: For network context
#include "mining/block_assembly_context.h"  // Phase W.1.3: Network-aware context
#include "mining/transaction_scorer.h"  // Phase W.1.3: Intelligent scoring
#include "common/logger.h"
#include "common/sha256d.h"
#include "common/address_script_builder.h"  // For BuildScriptPubKeyFromAddress
#include "storage/chain_direct.h"
#include "storage/chain_db.h"  // For ChainDB access
#include "utils/hexwriter.h"
#include "wallet/transaction.h"  // For TransactionSerializer
#include "crypto/hash.h"  // For din::crypto::SHA256D (determinism hash)
#include <unordered_map>  // Week 7: For fee lookup map
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <algorithm>
#include <cstdio>
#include <ctime>  // For std::time (extra nonce)

// Linker sentinel to prove this TU uses GetNextWorkRequired
extern "C" const char* DIN_ASSEMBLER_SENTINEL = "assembler_uses_GetNextWorkRequired_v1";

namespace dinero {

std::optional<OutPoint> ParseTemplatePoisonMissingPrevout(const std::string& error) {
    static constexpr std::string_view kNeedle = "utreexo-leaf-missing-in-pure: ";
    const size_t start = error.find(kNeedle);
    if (start == std::string::npos) {
        return std::nullopt;
    }

    const size_t txid_start = start + kNeedle.size();
    const size_t colon = error.find(':', txid_start);
    if (colon == std::string::npos || colon == txid_start) {
        return std::nullopt;
    }

    const std::string txid_hex = error.substr(txid_start, colon - txid_start);
    if (txid_hex.size() != 64) {
        return std::nullopt;
    }

    const size_t vout_end = error.find_first_not_of("0123456789", colon + 1);
    const std::string vout_text = error.substr(colon + 1, vout_end - (colon + 1));
    if (vout_text.empty()) {
        return std::nullopt;
    }

    try {
        return OutPoint(TxId(uint256::FromHexUnsafe(txid_hex)),
                        static_cast<uint32_t>(std::stoul(vout_text)));
    } catch (...) {
        return std::nullopt;
    }
}

std::unordered_set<uint256> CollectTemplatePoisonRemovalSet(
    const std::vector<Transaction>& candidate_txs,
    const OutPoint& missing_prevout,
    std::unordered_set<uint256>* direct_spenders
) {
    std::unordered_set<uint256> removal_set;
    std::unordered_set<uint256> direct_matches;

    for (const auto& tx : candidate_txs) {
        for (const auto& input : tx.vin) {
            if (input.prevout.txid == missing_prevout.txid &&
                input.prevout.vout == missing_prevout.vout) {
                const auto txid = tx.GetTxid().AsUint256();
                removal_set.insert(txid);
                direct_matches.insert(txid);
                break;
            }
        }
    }

    if (direct_spenders) {
        *direct_spenders = direct_matches;
    }

    if (removal_set.empty()) {
        return removal_set;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& tx : candidate_txs) {
            const auto txid = tx.GetTxid().AsUint256();
            if (removal_set.count(txid) != 0) {
                continue;
            }
            for (const auto& input : tx.vin) {
                if (removal_set.count(input.prevout.txid.AsUint256()) != 0) {
                    removal_set.insert(txid);
                    changed = true;
                    break;
                }
            }
        }
    }

    return removal_set;
}

std::vector<Transaction> FilterChainBackedTemplateTransactions(
    const std::vector<Transaction>& candidate_txs,
    const std::function<bool(const OutPoint&)>& has_chain_utxo,
    std::unordered_set<uint256>* deferred_txids
) {
    if (!has_chain_utxo) {
        return candidate_txs;
    }

    // Relies on candidate_txs being topologically ordered (parent before child),
    // as guaranteed by Mempool::selectTransactionsForBlock. The running
    // accepted_txids set lets a child's input resolve via an in-package parent
    // that has already been accepted, without falsely accepting a child whose
    // parent itself gets deferred.
    std::vector<Transaction> filtered_txs;
    filtered_txs.reserve(candidate_txs.size());
    std::unordered_set<uint256> accepted_txids;
    accepted_txids.reserve(candidate_txs.size());

    for (const auto& tx : candidate_txs) {
        bool chain_backed = true;
        for (const auto& input : tx.vin) {
            const OutPoint outpoint(input.prevout.txid, input.prevout.vout);
            if (has_chain_utxo(outpoint)) continue;
            if (accepted_txids.count(input.prevout.txid.AsUint256()) != 0) continue;
            chain_backed = false;
            break;
        }

        if (chain_backed) {
            filtered_txs.push_back(tx);
            accepted_txids.insert(tx.GetTxid().AsUint256());
        } else if (deferred_txids) {
            deferred_txids->insert(tx.GetTxid().AsUint256());
        }
    }

    return filtered_txs;
}

// Phase 39: Constructor updated - ChainManager parameter removed (ChainManager deleted)
BlockAssembler::BlockAssembler(ChainDB* chain_db)
    : chain_db_(chain_db)
    , mempool_(nullptr)
    , block_relay_manager_(nullptr)  // Phase W.1.3
    , use_intelligent_selection_(false)  // Phase W.1.3: Disabled by default
    , last_template_time_ms_(0)  // Phase W.1.4
{
    if (!chain_db_) {
        throw std::runtime_error("BlockAssembler: ChainDB cannot be null");
    }

    // Mempool is optional - transaction selection will be empty if not provided
    if (!mempool_) {
        dinero::g_logger.debug("BlockAssembler: Mempool not provided, transaction selection will be empty");
    }

    // Initialize state
    UpdateSupplyState();
    UpdateAlgoState();

    dinero::g_logger.info("BlockAssembler initialized with ChainDB");
}

void BlockAssembler::SetMiningAddress(const std::string& address) {
    mining_address_ = address;
    dinero::g_logger.debug("Mining address set: " + address);
}

void BlockAssembler::setChainDB(ChainDB* chain_db) {
    chain_db_ = chain_db;
    dinero::g_logger.debug("ChainDB set for BlockAssembler");
}

void BlockAssembler::setMempool(Mempool* mempool) {
    mempool_ = mempool;
    dinero::g_logger.debug("Mempool set for BlockAssembler");
}

// ============================================================================
// Template Determinism Guard (Consensus Safety)
// ============================================================================
// Computes a hash over txid || flags for each transaction to detect silent
// mutations between template creation and block finalization.
//
// This catches:
// - Silent tx reordering (priority queues, unordered containers)
// - Template mutation (late fee adjustments, coinbase rewrites)
// - CT/transparent reclassification bugs
// - Reorg-induced state drift
//
// If pre/post hashes differ, block assembly MUST abort.
// ============================================================================

uint256 BlockAssembler::ComputeTemplateDeterminismHash(
    const std::vector<Transaction>& transactions,
    const std::vector<TxEntryFlags>& flags
) {
    if (transactions.size() != flags.size()) {
        dinero::g_logger.error("ComputeTemplateDeterminismHash: size mismatch");
        return uint256();
    }

    // Build hash input: for each tx, append txid (32 bytes) + flags (4 bytes)
    std::vector<uint8_t> hash_input;
    hash_input.reserve(transactions.size() * 36);

    for (size_t i = 0; i < transactions.size(); ++i) {
        // Get txid bytes (Phase M.4: GetTxid() returns TxId, extract uint256)
        uint256 txid = transactions[i].GetTxid().AsUint256();
        const uint8_t* txid_bytes = reinterpret_cast<const uint8_t*>(&txid);
        hash_input.insert(hash_input.end(), txid_bytes, txid_bytes + 32);

        // Append flags as 4 bytes (little-endian)
        uint32_t flags_val = flags[i].ToFlagsByte();
        hash_input.push_back(static_cast<uint8_t>(flags_val & 0xFF));
        hash_input.push_back(static_cast<uint8_t>((flags_val >> 8) & 0xFF));
        hash_input.push_back(static_cast<uint8_t>((flags_val >> 16) & 0xFF));
        hash_input.push_back(static_cast<uint8_t>((flags_val >> 24) & 0xFF));
    }

    // SHA256d for consensus-critical hash
    din::crypto::Sha256Hash final_hash = din::crypto::SHA256D(hash_input);

    uint256 result;
    std::memcpy(&result, final_hash.data(), 32);
    return result;
}

// Helper: Classify fee rate into tiers for determinism hash
static uint8_t ClassifyFeeRate(double fee_rate) {
    if (fee_rate < 1.0) return 0;        // Dust
    if (fee_rate < 5.0) return 1;        // Low
    if (fee_rate < 20.0) return 2;       // Medium
    if (fee_rate < 100.0) return 3;      // High
    return 4;                            // Urgent
}

// ============================================================================
// WITNESS NONCE OFFSET COMPUTATION (Utreexo-compatible extranonce)
// ============================================================================
// Computes the byte offset of the 8-byte witness nonce in a serialized coinbase.
// This offset is returned to miners so they can inject their extranonce without
// changing the coinbase txid (which would break Utreexo commitments).
//
// CRITICAL: This computation must match the coinbase structure built in
// createCoinbaseTransaction() exactly. Any structural change there requires
// updating this function.
// ============================================================================

// Helper: Compute Bitcoin varint size for a given value
static size_t VarIntSize(uint64_t value) {
    if (value < 0xFD) return 1;
    if (value <= 0xFFFF) return 3;      // 0xFD + 2 bytes
    if (value <= 0xFFFFFFFF) return 5;  // 0xFE + 4 bytes
    return 9;                            // 0xFF + 8 bytes
}

// Compute witness nonce offset for a coinbase transaction
// Returns the byte offset in the serialized transaction where the 8-byte witness nonce lives
static size_t ComputeWitnessNonceOffset(const Transaction& coinbase) {
    if (!coinbase.IsCoinbase() || coinbase.vin.empty()) {
        return 0;  // Invalid coinbase
    }

    // Verify coinbase has witness nonce (single 8-byte witness item)
    if (coinbase.vin[0].witness.empty() ||
        coinbase.vin[0].witness[0].size() != 8) {
        return 0;  // No witness nonce present
    }

    // Serialized coinbase structure (SegWit format):
    //   version:           4 bytes
    //   marker:            1 byte (0x00)
    //   flag:              1 byte (0x01)
    //   input_count:       varint (1 for coinbase)
    //   prevout txid:      32 bytes
    //   prevout vout:      4 bytes
    //   scriptSig length:  varint
    //   scriptSig:         N bytes
    //   sequence:          4 bytes
    //   output_count:      varint (typically 1)
    //   output value:      8 bytes
    //   scriptPubKey len:  varint
    //   scriptPubKey:      M bytes
    //   witness:
    //     item_count:      varint (1)
    //     item_length:     varint (8)
    //     item_data:       8 bytes  <-- THIS IS THE NONCE OFFSET
    //   lockTime:          4 bytes

    size_t scriptSig_size = coinbase.vin[0].scriptSig.size();
    [[maybe_unused]] size_t scriptPubKey_size = coinbase.vout[0].scriptPubKey.size();
    size_t output_count = coinbase.vout.size();

    // Compute offset to witness nonce data
    size_t offset = 0;
    offset += 4;                              // version
    offset += 1;                              // marker (0x00)
    offset += 1;                              // flag (0x01)
    offset += VarIntSize(1);                  // input count (always 1)
    offset += 32;                             // prevout txid
    offset += 4;                              // prevout vout
    offset += VarIntSize(scriptSig_size);     // scriptSig length
    offset += scriptSig_size;                 // scriptSig data
    offset += 4;                              // sequence
    offset += VarIntSize(output_count);       // output count

    // Sum all outputs
    for (const auto& output : coinbase.vout) {
        offset += 8;                          // value
        offset += VarIntSize(output.scriptPubKey.size());  // scriptPubKey length
        offset += output.scriptPubKey.size();              // scriptPubKey data
    }

    // Witness section for input 0
    offset += VarIntSize(1);                  // witness item count (1)
    offset += VarIntSize(8);                  // witness item length (8)
    // offset now points to the 8-byte nonce data

    return offset;
}

// Helper: Build flags for a transaction
static BlockAssembler::TxEntryFlags BuildTxFlags(const Transaction& tx, bool is_coinbase, double fee_rate) {
    BlockAssembler::TxEntryFlags flags;
    flags.is_coinbase = is_coinbase;
    flags.fee_class = ClassifyFeeRate(fee_rate);

    // Check for CT outputs
    for (const auto& output : tx.vout) {
        if (output.is_confidential) {
            flags.is_confidential = true;
            break;
        }
    }

    // Witness version (from first witness input, if any)
    for (const auto& input : tx.vin) {
        if (!input.witness.empty()) {
            // SegWit v0 has witness, Taproot would have version byte
            flags.witness_version = 0;  // Assume v0 for now
            break;
        }
    }

    return flags;
}

std::shared_ptr<MiningJob> BlockAssembler::CreateJob(const uint256* explicit_tip_hash) {
    if (!chain_db_) {
        dinero::g_logger.error("BlockAssembler: ChainDB not initialized");
        return nullptr;
    }

    auto job = std::make_shared<MiningJob>();
    job->header.ZeroReserved();

    // Update state before creating job
    UpdateSupplyState();
    UpdateAlgoState();

    // Get chain tip - use explicit hash if provided (avoids race after block acceptance)
    uint256 prev_hash;
    uint32_t parent_height;

    if (explicit_tip_hash) {
        // Explicit tip provided - get height by hash (NO getTip() race!)
        prev_hash = *explicit_tip_hash;

        // Query height directly by hash
        auto height_result = chain_db_->getBlockHeight(prev_hash);
        if (height_result.status() != dinero::Status::Ok) {
            dinero::g_logger.error("BlockAssembler: Failed to get height for explicit hash: " +
                                  prev_hash.GetHex());
            return nullptr;
        }
        parent_height = static_cast<uint32_t>(height_result.value());

        dinero::g_logger.info("BlockAssembler: Using explicit tip - hash=" + prev_hash.GetHex().substr(0, 16) +
                             "... height=" + std::to_string(parent_height));
    } else {
        // No explicit tip - query current tip from ChainDB (default behavior)
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() != dinero::Status::Ok) {
            dinero::g_logger.error("BlockAssembler: Failed to get chain tip from ChainDB");
            return nullptr;
        }
        const auto& tip = tip_result.value();
        prev_hash = tip.hash;
        parent_height = tip.height;
    }

    job->height = parent_height + 1;

    // Full-rules blocks MUST use the validation oracle.
    // Refuse to create jobs that would otherwise produce null utreexo roots.
    if (consensus::FullRulesActive(job->height) && !block_validator_) {
        dinero::g_logger.error("BlockAssembler::CreateJob: BlockValidator not wired at height " +
                               std::to_string(job->height) +
                               " (refusing to create invalid mining job)");
        return nullptr;
    }

    // Initialize header
    job->header.version = 1;

    // Phase 3: BlockHeader.prev_block_hash is uint256, not string
    job->header.prev_block_hash = prev_hash;
    
    // Initialize timestamp: ensure it's > MTP (BIP113) and current
    uint32_t mtp = GetMedianTimePast();
    uint32_t current_time = static_cast<uint32_t>(std::time(nullptr));
    
    // Timestamp must be > MTP and <= current_time + 2 hours
    job->header.timestamp = std::max(current_time, mtp + 1);
    
    // Ensure timestamp is not too far in the future (max 2 hours)
    uint32_t max_future_time = current_time + 7200;  // 2 hours
    if (job->header.timestamp > max_future_time) {
        job->header.timestamp = max_future_time;
    }

    // Synchronize legacy timestamp field (BlockHeader has both 'time' and 'timestamp')
    job->header.timestamp = job->header.timestamp;

    job->header.nonce = 0;
    
    // Calculate difficulty through the shared ASERT context builder.
    algo_state_.current_height = job->height;
    const Consensus consensus = GetConsensusForCurrentNetwork();
    const int64_t currentTime = static_cast<int64_t>(job->header.timestamp);
    job->target_bits = GetNextWorkRequiredWithChainDB(
        static_cast<int32_t>(job->height),
        currentTime,
        consensus,
        chain_db_);
    job->header.difficulty = job->target_bits;

    // DEBUG: Log with proper hex formatting and phase detection
    char next_hex[9];
    std::snprintf(next_hex, sizeof(next_hex), "%08x", job->target_bits);
    const char* phase = (job->height == 0) ? "GENESIS" : (job->height == 1) ? "ANCHOR" : "ASERT";

    dinero::g_logger.info(
        "DEBUG BlockAssembler: H=" + std::to_string(job->height) +
        " nextBits=0x" + std::string(next_hex) +
        " phase=" + std::string(phase)
    );

    // Calculate block reward using canonical subsidy schedule (BITCOIN-CORRECT)
    // Single source of truth: GetBlockSubsidy()
    // No special cases, no injection logic, pure consensus law
    // Phase M.6.2: Extract raw value from AmountUna
    job->block_reward = dinero::ConsensusSubsidy::GetBlockSubsidy(job->height).GetUna();

    // Log warning if subsidy reaches 0 (after all halvings complete)
    if (job->block_reward == 0 && job->height > 0) {
        dinero::g_logger.warning("Block reward is 0 (all halvings complete)");
    }
    
    // Select transactions from mempool
    job->total_fees = 0;
    job->transactions = SelectTransactions(max_block_weight_, job->height, job->total_fees);
    
    // Create coinbase transaction (SegWit with witness nonce for Utreexo compatibility)
    // Uses createCoinbaseTransaction which puts miner entropy in witness (not scriptSig),
    // keeping txid daemon-controlled. Also ensures HasWitness()=true so DINW commitment
    // is correctly added for coinbase-only blocks.
    Transaction coinbase = createCoinbaseTransaction(
        job->height, mining_address_, job->block_reward, job->total_fees);
    if (coinbase.vin.empty()) {
        dinero::g_logger.error("CreateJob: Failed to create coinbase transaction");
        return nullptr;
    }
    
    // Insert coinbase at the beginning
    job->transactions.insert(job->transactions.begin(), coinbase);

    // ═════════════════════════════════════════════════════════════════════════
    // EXPLICIT CONSENSUS GATE: Witness commitment (full-rules only)
    // ═════════════════════════════════════════════════════════════════════════
    // Pre-activation: witness commitment is skipped.
    // Full-rules: witness commitment is required when block has witness data.
    // ═════════════════════════════════════════════════════════════════════════
    if (consensus::FullRulesActive(job->height)) {
        // FULL RULES PATH: Add witness commitment if block has witness data
        bool has_witness = false;
        for (const auto& tx : job->transactions) {
            if (tx.HasWitness()) {
                has_witness = true;
                break;
            }
        }

        if (has_witness) {
            // Build witness commitment for the block
            std::vector<uint8_t> commitment_script =
                consensus::BuildWitnessCommitment(job->transactions);

            if (!commitment_script.empty()) {
                // Add witness commitment as additional output to coinbase
                TxOutput commitment_output;
                commitment_output.value = AmountUna::Zero();  // OP_RETURN has 0 value
                commitment_output.scriptPubKey = commitment_script;

                // Add to coinbase (which is at index 0)
                job->transactions[0].vout.push_back(commitment_output);

                dinero::g_logger.debug("Added witness commitment to coinbase (height " +
                    std::to_string(job->height) + ", DINW magic)");
            }
        }
    } else {
        // PRE-ACTIVATION PATH (height 0-1): No witness commitment allowed
        dinero::g_logger.debug("Skipping witness commitment (height " +
            std::to_string(job->height) + " is pre-activation)");
    }
    // ═════════════════════════════════════════════════════════════════════════

    // ═════════════════════════════════════════════════════════════════════════
    // FILTER COMMITMENT: Add GCS filter hash to coinbase (DNRF magic)
    // Built BEFORE merkle root so commitment is included in the merkle tree.
    // SipHash key = prev_block_hash (avoids circular dependency with block_hash).
    // OP_RETURN outputs excluded (provably unspendable).
    // Spent input scripts resolved via utxo_provider_ for full BIP158 filter.
    // ═════════════════════════════════════════════════════════════════════════
    if (consensus::FullRulesActive(job->height)) {
        std::vector<std::vector<uint8_t>> filter_scripts;

        // Collect output scriptPubKeys (excluding OP_RETURN)
        for (const auto& tx : job->transactions) {
            for (const auto& out : tx.vout) {
                if (!out.scriptPubKey.empty() && out.scriptPubKey[0] != 0x6a) {
                    filter_scripts.push_back(out.scriptPubKey);
                }
            }
        }

        // Collect spent input scriptPubKeys via UTXO lookup
        if (utxo_provider_) {
            for (size_t tx_idx = 0; tx_idx < job->transactions.size(); ++tx_idx) {
                if (tx_idx == 0) continue;  // Skip coinbase
                for (const auto& in : job->transactions[tx_idx].vin) {
                    if (in.prevout.txid.IsNull()) continue;
                    OutPoint op(in.prevout.txid, in.prevout.vout);
                    auto utxo_opt = utxo_provider_->GetUTXO(op);
                    if (utxo_opt.has_value() && !utxo_opt->scriptPubKey.empty()) {
                        filter_scripts.push_back(utxo_opt->scriptPubKey);
                    }
                }
            }
        }

        // SipHash key = prev_block_hash (deterministic before mining)
        auto filter = consensus::GCSFilter::Build(filter_scripts, job->header.prev_block_hash);
        if (!filter.IsEmpty()) {
            uint256 filter_hash = filter.GetHash();
            auto commitment_script = consensus::BuildFilterCommitmentScript(filter_hash);

            if (!commitment_script.empty()) {
                TxOutput filter_output;
                filter_output.value = AmountUna::Zero();
                filter_output.scriptPubKey = commitment_script;
                job->transactions[0].vout.push_back(filter_output);

                dinero::g_logger.debug("Added filter commitment to coinbase (height " +
                    std::to_string(job->height) + ", DNRF magic, " +
                    std::to_string(filter.element_count) + " scripts)");
            }
        }
    }

    // Calculate merkle root (AFTER witness + filter commitments are added)
    job->merkle_root = CalculateMerkleRoot(job->transactions);  // String for job metadata
    // Phase 3: Convert hex string to uint256 for BlockHeader
    uint256::FromHex(job->merkle_root, job->header.merkle_root);

    // ═════════════════════════════════════════════════════════════════════════
    // EXPLICIT CONSENSUS GATE: Utreexo commitment (full-rules only)
    // ═════════════════════════════════════════════════════════════════════════
    // Pre-activation: no Utreexo accumulator updates.
    // Full-rules: MUST compute and include Utreexo commitment.
    //
    // CRITICAL: Use the SAME oracle function as validation to ensure consistency.
    // BlockAssembler must NOT reimplement Utreexo logic - call the oracle.
    // ═════════════════════════════════════════════════════════════════════════
    if (consensus::FullRulesActive(job->height) && block_validator_) {
        // Build temporary block from job transactions for oracle call
        Block temp_block;
        temp_block.vtx = job->transactions;
        temp_block.header = job->header;

        // Call the SAME pure function used by validation (single source of truth)
        uint256 computed_root;
        std::string utreexo_error;
        if (block_validator_->ComputeUtreexoRootPure(temp_block, job->height, computed_root, utreexo_error)) {
            // Convert uint256 to hex string for job metadata
            job->utreexo_root = computed_root.GetHex();
            // Set in header directly
            job->header.utreexo_root = computed_root;

            dinero::g_logger.debug("[BlockAssembler] Utreexo root computed via oracle: " +
                job->utreexo_root.substr(0, 16) + "...");
        } else {
            dinero::g_logger.error("[BlockAssembler] Oracle ComputeUtreexoRootPure failed: " + utreexo_error);
            return nullptr;
        }
    } else if (!consensus::FullRulesActive(job->height)) {
        // PRE-ACTIVATION PATH: no Utreexo commitment
        dinero::g_logger.debug("Skipping Utreexo commitment (height " +
            std::to_string(job->height) + " is pre-activation)");
        // Utreexo root stays as zero/empty for pre-activation blocks
    } else {
        // FullRulesActive but block_validator_ is null — refuse to produce invalid job
        dinero::g_logger.error("❌ [BlockAssembler] FATAL: block_validator_ not set — refusing to produce job with null utreexo root");
        return nullptr;
    }

    if (consensus::FullRulesActive(job->height) && job->header.utreexo_root.IsNull()) {
        dinero::g_logger.error("BlockAssembler::CreateJob: null utreexo root at height " +
                               std::to_string(job->height) + " (aborting)");
        return nullptr;
    }

    // Set timing parameters (ensure consistency)
    job->current_time = job->header.timestamp;
    job->created_time = static_cast<uint32_t>(std::time(nullptr));  // Actual creation time
    job->max_time = job->current_time + max_block_time_;  // Max valid timestamp
    
    // Generate job ID
    job->job_id = GenerateJobId();
    
    // Calculate target hex for mining using our own function
    job->target_hex = BitsToTargetHex(job->target_bits);
    
    // Initialize mining parameters
    job->extra_nonce = 0;
    job->stop_mining.store(false);
    job->is_stale = false;
    
    // Verify job has all required fields populated
    if (job->transactions.empty() || !job->transactions[0].IsCoinbase()) {
        dinero::g_logger.error("Created job missing coinbase transaction");
        return nullptr;
    }
    
    if (job->merkle_root.empty() || job->merkle_root.length() != 64) {
        dinero::g_logger.error("Created job has invalid merkle root");
        return nullptr;
    }

    // Phase 3: prev_block_hash is uint256, check if null instead of string methods
    if (job->header.prev_block_hash.IsNull()) {
        dinero::g_logger.error("Created job has invalid previous block hash");
        return nullptr;
    }
    
    // Update statistics
    jobs_created_++;
    last_job_height_ = job->height;
    last_job_id_ = job->job_id;
    
    // Determine mining phase for logging (subsidy schedule)
    std::string phase_desc;
    if (job->height == 0) {
        phase_desc = "Genesis";
    } else {
        // Height 1+: Calculate halving epoch
        uint32_t halvings = (job->height - 1) / dinero::ConsensusSubsidy::HALVING_INTERVAL;
        if (halvings >= 33) {
            phase_desc = "Halving Complete (0 DIN)";
        } else {
            phase_desc = "Halving Epoch " + std::to_string(halvings) + " (ASERT difficulty)";
        }
    }

    dinero::g_logger.info("Created mining job: " + job->job_id +
                         " height=" + std::to_string(job->height) +
                         " reward=" + std::to_string(job->block_reward / COIN) + " DIN" +
                         " difficulty=0x" + std::to_string(job->target_bits) +
                         " phase=\"" + phase_desc + "\"" +
                         " txs=" + std::to_string(job->transactions.size()));
    
    return job;
}

bool BlockAssembler::RefreshJob(std::shared_ptr<MiningJob> job) {
    if (!job || job->is_stale) {
        return false;
    }
    
    // Check if blockchain has advanced
    // Phase 39: Get tip from ChainDB (ChainManager deleted)
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != dinero::Status::Ok) {
        dinero::g_logger.error("Failed to get chain tip for job refresh");
        return false;
    }
    const auto& tip = tip_result.value();
    uint32_t current_height = tip.height + 1;
    if (current_height != job->height) {
        dinero::g_logger.info("Job " + job->job_id + " is stale (height changed: " +
                             std::to_string(job->height) + " -> " + std::to_string(current_height) + ")");
        job->is_stale = true;
        return false;
    }
    
    // Update timestamp
    uint32_t new_time = static_cast<uint32_t>(std::time(nullptr));
    if (new_time > job->max_time) {
        dinero::g_logger.info("Job " + job->job_id + " expired (max time exceeded)");
        job->is_stale = true;
        return false;
    }
    
    // Update job timing
    job->current_time = new_time;
    job->header.timestamp = new_time;
    
    // Increment extra nonce to avoid duplicate work
    job->extra_nonce++;
    
    // Recalculate merkle root if extra nonce affects coinbase
    // (In a full implementation, this would update the coinbase transaction)
    job->merkle_root = CalculateMerkleRoot(job->transactions);  // String for job metadata
    // Phase 3: Convert hex string to uint256 for BlockHeader
    uint256::FromHex(job->merkle_root, job->header.merkle_root);
    
    // Reset nonce for fresh mining
    job->header.nonce = 0;
    
    jobs_refreshed_++;
    
    dinero::g_logger.debug("Refreshed mining job: " + job->job_id + 
                          " extranonce=" + std::to_string(job->extra_nonce) + 
                          " time=" + std::to_string(new_time));
    
    return true;
}

bool BlockAssembler::UpdateJobTime(std::shared_ptr<MiningJob> job) {
    if (!job || job->is_stale) {
        return false;
    }
    
    uint32_t new_time = static_cast<uint32_t>(std::time(nullptr));
    
    // Don't update if time hasn't changed significantly
    if (new_time <= job->current_time) {
        return true;
    }
    
    // Check max time limit
    if (new_time > job->max_time) {
        job->is_stale = true;
        return false;
    }
    
    job->current_time = new_time;
    job->header.timestamp = new_time;
    
    return true;
}

bool BlockAssembler::ShouldRefreshJob(const std::shared_ptr<MiningJob>& job) const {
    if (!job) return true;
    if (job->is_stale) return true;

    // Check if blockchain has advanced
    // Phase 39: Get tip from ChainDB (ChainManager deleted)
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != dinero::Status::Ok) return false;
    const auto& tip = tip_result.value();
    uint32_t current_height = tip.height + 1;
    if (current_height != job->height) {
        return true;
    }

    // Check age of job (refresh every 30 seconds)
    uint32_t current_time = static_cast<uint32_t>(std::time(nullptr));
    if (current_time - job->created_time > 30) {
        return true;
    }
    
    // Check if max time is approaching
    if (current_time + 60 > job->max_time) {
        return true;
    }
    
    return false;
}

std::string BlockAssembler::GetMiningStats() const {
    std::ostringstream oss;
    
    oss << "BlockAssembler Statistics:\n";
    oss << "  Jobs Created: " << jobs_created_.load() << "\n";
    oss << "  Jobs Refreshed: " << jobs_refreshed_.load() << "\n";
    oss << "  Last Job Height: " << last_job_height_.load() << "\n";
    oss << "  Last Job ID: " << last_job_id_ << "\n";
    
    // Supply state
    // oss << "\n" << GetSupplyStats(supply_state_) << "\n";
    oss << "\n" << "Supply stats: Basic tracking for regtest mode" << "\n";
    
    // Algorithm state
    oss << "\nMining Phase: " << "Basic tracking for regtest mode" << "\n";
    
    return oss.str();
}

// Private methods

std::vector<Transaction> BlockAssembler::SelectTransactions(
    uint32_t max_weight,
    uint32_t target_height,
    uint64_t& total_fees) {
    std::vector<Transaction> selected;
    total_fees = 0;
    
    if (!mempool_) {
        dinero::g_logger.debug("SelectTransactions: Mempool not available, returning empty selection");
        return selected;
    }
    
    // Week 7: Use mempool's built-in selection method (implements fee-rate sorting, weight limits, dependency checking).
    // Pass the actual candidate height so height-gated consensus checks use
    // the rule set this template will be validated under.
    selected = mempool_->selectTransactionsForBlock(
        max_block_weight_ / 4,  // max_block_size (approximate: weight / 4)
        max_weight,              // max_block_weight
        target_height            // target block height
    );

    // Calculate total fees from selected transactions (exact, per-tx summation)
    // This is not an estimate - it's the actual fees this block commits to.
    // Required for: CPFP correctness, RBF accounting, CT fee validation, audit trails.
    total_fees = 0;
    for (const auto& tx : selected) {
        TxId txid = tx.GetTxid();
        auto entry = mempool_->getMempoolEntry(txid.AsUint256());
        if (entry) {
            total_fees += entry->fee;
        } else {
            // Transaction was in selection but not in mempool (race condition)
            // Log but don't fail - this can happen during reorgs
            dinero::g_logger.warning("SelectTransactions: tx " + txid.AsUint256().GetHex().substr(0, 16) +
                                    "... not found in mempool during fee calculation");
        }
    }

    dinero::g_logger.debug("Selected " + std::to_string(selected.size()) +
                          " transactions with exact total fees: " + std::to_string(total_fees));
    
    return selected;
}

std::string BlockAssembler::BuildCoinbaseTransaction(uint32_t height, uint64_t reward,
                                                   uint64_t fees, const std::vector<uint8_t>& payout_script) {
    try {
        auto total_output = dinero::CheckedAddUna(reward, fees);
        if (!total_output) {
            throw std::runtime_error("Coinbase value overflow in BuildCoinbaseTransaction");
        }

        dinero::g_logger.debug("Building coinbase transaction: height=" + std::to_string(height) +
                              " reward=" + std::to_string(reward) +
                              " fees=" + std::to_string(fees) +
                              " total=" + std::to_string(*total_output) +
                              " script_size=" + std::to_string(payout_script.size()));

        // Use HexWriter for clean, endian-safe serialization
        HexWriter w;

        // Transaction version (4 bytes, little-endian)
        w.writeLE(1, 4);

        // Input count (1 byte)
        w.writeLE(1, 1);

        // Coinbase input
        // Previous output hash (32 bytes, all zeros for coinbase)
        w.write(std::string(64, '0'));

        // Previous output index (4 bytes, 0xffffffff for coinbase)
        w.write("ffffffff");

        // Coinbase script (BIP34: include block height using Script number encoding)
        // IMPORTANT: Must use PUSHDATA format (not opcodes) because validator reads raw bytes
        consensus::Script script;
        std::vector<uint8_t> heightBytes = consensus::scriptNumEncode(static_cast<int64_t>(height));
        script.pushData(heightBytes);  // Force PUSHDATA format: <length> <data>

        // Add extra nonce for uniqueness (required: minimum 2 bytes total)
        // This ensures different blocks at same height have different coinbase txids
        uint32_t extraNonce = static_cast<uint32_t>(std::time(nullptr));
        std::vector<uint8_t> extraNonceBytes = {
            static_cast<uint8_t>(extraNonce & 0xff),
            static_cast<uint8_t>((extraNonce >> 8) & 0xff),
            static_cast<uint8_t>((extraNonce >> 16) & 0xff),
            static_cast<uint8_t>((extraNonce >> 24) & 0xff)
        };
        script << extraNonceBytes;  // Pushes with length prefix

        std::vector<uint8_t> scriptSig = script.data();

        // Script length and script
        w.writeLE(scriptSig.size(), 1);
        w.writeBytes(scriptSig);

        // Sequence (4 bytes, 0xffffffff)
        w.write("ffffffff");

        // Output count: Always 1 output (Bitcoin-correct)
        // Subsidy + fees goes to payout script
        uint8_t output_count = 1;
        w.writeLE(output_count, 1);

        // Single coinbase output: subsidy + fees to payout script
        // No special cases - GetBlockSubsidy() handles halving + tail emission
        w.writeLE(*total_output, 8);
        w.writeLE(payout_script.size(), 1);
        w.writeBytes(payout_script);

        // Lock time (4 bytes, 0)
        w.writeLE(0, 4);

        std::string tx_hex = w.str();
        dinero::g_logger.info("Built coinbase transaction: " + tx_hex.substr(0, 64) + "...");

        return tx_hex;

    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to build coinbase transaction: " + std::string(e.what()));
        return "coinbase_tx_error";
    }
}

std::string BlockAssembler::CalculateMerkleRoot(const std::vector<Transaction>& transactions) {
    // Phase 11a.2: Delegate to canonical merkle computation
    // This eliminates all ad-hoc merkle logic and enforces single source of truth
    uint256 root = consensus::ComputeMerkleRoot(transactions);
    return root.GetHex();
}

std::string BlockAssembler::GenerateJobId() {
    // Generate a unique job ID based on timestamp and counter
    auto now = std::chrono::steady_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    
    std::ostringstream oss;
    oss << std::hex << timestamp << "_" << std::hex << (++job_counter_);
    
    return oss.str();
}

std::string BlockAssembler::BitsToTargetHex(uint32_t bits) {
    dinero::g_logger.info("[BitsToTargetHex] INPUT bits=" + std::to_string(bits) + " (0x" + std::to_string(bits) + ")");

    // Canonical Bitcoin compact format decoding
    const uint32_t exp = bits >> 24;
    const uint32_t mant = bits & 0x00ffffff;

    dinero::g_logger.info("[BitsToTargetHex] exp=" + std::to_string(exp) + " mant=" + std::to_string(mant));

    // Create 32-byte big-endian target array
    uint8_t target[32];
    memset(target, 0, 32);

    if (exp <= 3) {
        dinero::g_logger.info("[BitsToTargetHex] Taking exp<=3 branch");
        uint32_t v = mant >> (8 * (3 - exp));
        for (int i = 0; i < 4; ++i) {
            target[31 - i] = (v >> (8 * i)) & 0xff;
        }
    } else {
        dinero::g_logger.info("[BitsToTargetHex] Taking exp>3 branch, off=" + std::to_string(exp - 3));
        const int off = exp - 3;
        int idx0 = 32 - off - 3;
        int idx1 = 32 - off - 2;
        int idx2 = 32 - off - 1;
        dinero::g_logger.info("[BitsToTargetHex] Setting target[" + std::to_string(idx0) + "]=" + std::to_string((mant >> 16) & 0xff));
        dinero::g_logger.info("[BitsToTargetHex] Setting target[" + std::to_string(idx1) + "]=" + std::to_string((mant >> 8) & 0xff));
        dinero::g_logger.info("[BitsToTargetHex] Setting target[" + std::to_string(idx2) + "]=" + std::to_string(mant & 0xff));
        target[idx0] = (mant >> 16) & 0xff;
        target[idx1] = (mant >> 8) & 0xff;
        target[idx2] = mant & 0xff;
    }

    // Output bytes in natural order (0→31) to match array construction
    // The array is built in big-endian order (index 0 = most significant byte)
    // So we output it directly without reversing
    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {  // Natural order: big-endian
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(target[i]);
    }

    std::string result = oss.str();
    dinero::g_logger.info("[BitsToTargetHex-FIXED] bits=0x" + std::to_string(bits) +
                         " target_full=" + result);
    dinero::g_logger.info("[BitsToTargetHex-FIXED] target_start=" + result.substr(0, 16) +
                         " target_end=" + result.substr(result.length() - 16));

    return result;
}

std::vector<uint8_t> BlockAssembler::GetMiningScript() {
    try {
        if (mining_address_.empty()) {
            dinero::g_logger.error("No mining address set for script generation");
            return {};
        }
        
        // Convert address to scriptPubKey
        std::vector<uint8_t> spk;
        std::string error;
        if (!BuildScriptPubKeyFromAddress(mining_address_, spk, error)) {
            dinero::g_logger.error("Failed to create scriptPubKey from mining address: " + mining_address_ + " - " + error);
            return {};
        }

        std::string script_hex = ScriptPubKeyToHex(spk);
        
        // Convert hex string to bytes
        std::vector<uint8_t> script_bytes;
        for (size_t i = 0; i < script_hex.length(); i += 2) {
            std::string byte_str = script_hex.substr(i, 2);
            script_bytes.push_back(static_cast<uint8_t>(std::stoi(byte_str, nullptr, 16)));
        }
        
        dinero::g_logger.debug("Generated mining script for address " + mining_address_ + 
                              ": " + script_hex + " (" + std::to_string(script_bytes.size()) + " bytes)");
        
        return script_bytes;
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to get mining script: " + std::string(e.what()));
        return {};
    }
}

bool BlockAssembler::ValidateJobParameters(const std::shared_ptr<MiningJob>& job) const {
    if (!job) return false;
    
    // Basic validation
    // Phase 3: prev_block_hash is uint256, check if not null instead of comparing to string
    if (job->height == 0 && !job->header.prev_block_hash.IsNull()) {
        return false;  // Genesis block should have null prev hash
    }
    
    if (job->target_bits == 0) {
        return false;  // Invalid difficulty
    }
    
    if (job->block_reward == 0 && job->height > 0) {
        // Only genesis and developer fund blocks can have zero reward
        if (job->height > 1) {
            return false;
        }
    }
    
    if (job->transactions.empty()) {
        return false;  // Must have at least coinbase
    }
    
    return true;
}

uint32_t BlockAssembler::CalculateBlockWeight(const std::vector<Transaction>& transactions) const {
    try {
        uint32_t total_weight = 0;
        
        // Block header weight (128 bytes * 4 = 512 weight units, BlockHeader v1)
        total_weight += 128 * 4;
        
        // Transaction count varint weight
        uint32_t tx_count = transactions.size();
        if (tx_count < 0xfd) {
            total_weight += 1 * 4; // 1 byte varint
        } else if (tx_count <= 0xffff) {
            total_weight += 3 * 4; // 3 bytes varint
        } else if (tx_count <= 0xffffffff) {
            total_weight += 5 * 4; // 5 bytes varint
        } else {
            total_weight += 9 * 4; // 9 bytes varint
        }
        
        // Transaction weights
        for (const auto& tx : transactions) {
            // Estimate transaction weight based on inputs and outputs
            // This is a simplified calculation - in reality, you'd serialize the transaction
            
            // Base transaction structure (version, locktime, etc.)
            uint32_t tx_base_weight = 4 + 4; // version + locktime
            
            // Input count varint
            uint32_t input_count = tx.vin.size();
            if (input_count < 0xfd) {
                tx_base_weight += 1;
            } else if (input_count <= 0xffff) {
                tx_base_weight += 3;
            } else {
                tx_base_weight += 5;
            }
            
            // Output count varint
            uint32_t output_count = tx.vout.size();
            if (output_count < 0xfd) {
                tx_base_weight += 1;
            } else if (output_count <= 0xffff) {
                tx_base_weight += 3;
            } else {
                tx_base_weight += 5;
            }
            
            // Input weights (simplified)
            tx_base_weight += tx.vin.size() * (32 + 4 + 1 + 4 + 25);

            // Output weights (simplified)
            tx_base_weight += tx.vout.size() * (8 + 1 + 25);
            
            // Convert bytes to weight units (multiply by 4)
            total_weight += tx_base_weight * 4;
        }
        
        dinero::g_logger.debug("Calculated block weight: " + std::to_string(total_weight) + 
                              " for " + std::to_string(transactions.size()) + " transactions");
        
        return total_weight;
        
    } catch (const std::exception& e) {
        dinero::g_logger.error("Failed to calculate block weight: " + std::string(e.what()));
        
        // Fallback to simple estimation
        return transactions.size() * 250 * 4; // 250 bytes per transaction * 4 weight units
    }
}

uint32_t BlockAssembler::GetMedianTimePast() const {
    // Week 5: Migrated from dinero::legacy::g_chain_db_direct() global to chain_db_ member (COMPLETE)
    if (!chain_db_) {
        dinero::g_logger.error("GetMedianTimePast: ChainDB not set - this is a bug!");
        throw std::runtime_error("BlockAssembler: ChainDB not injected via constructor");
    }
    
    return dinero::storage::GetMedianTimePast(chain_db_);
}

void BlockAssembler::UpdateSupplyState() {
    // Phase 39: ChainManager check removed (ChainManager deleted)
    if (!chain_db_) return;

    // Get supply state from ChainDB (simplified for regtest)
    // Phase 39: Get tip from ChainDB (ChainManager deleted)
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != dinero::Status::Ok) return;

    // Calculate based on current height
    // uint32_t height = tip_result.value().height;
    // supply_state_.total_coins_mined = 0;
    // 
    // // Calculate total coins (simplified)
    // if (height >= 0) {
    //     supply_state_.total_coins_mined += DineroAlgorithm::GENESIS_REWARD;
    // }
    // if (height >= 1) {
    //     supply_state_.total_coins_mined += DineroAlgorithm::DEVELOPER_FUND;
    // }
    // if (height >= 2) {
    //     uint32_t mining_blocks = height - 1;
    //     uint64_t mining_coins = mining_blocks * DineroAlgorithm::CPU_FRIENDLY_REWARD;
    //     
    //     if (mining_coins > DineroAlgorithm::CPU_FRIENDLY_TARGET) {
    //         mining_coins = DineroAlgorithm::CPU_FRIENDLY_TARGET;
    //         // Add halving phase calculation when needed
    //     }
    //     
    //     supply_state_.total_coins_mined += mining_coins;
    // }
    // 
    // // Update phase flags
    // supply_state_.is_developer_fund_phase = DineroAlgorithm::isDeveloperFundPhase(supply_state_.total_coins_mined);
    // supply_state_.is_cpu_friendly_phase = DineroAlgorithm::isCPUFriendlyPhase(supply_state_.total_coins_mined);
    // supply_state_.is_halving_phase = !supply_state_.is_developer_fund_phase && !supply_state_.is_cpu_friendly_phase;
}

void BlockAssembler::UpdateAlgoState() {
    // Basic algorithm state updates (simplified for regtest)
    // algo_state_.current_height = supply_state_.current_height;
    // algo_state_.total_coins_mined = supply_state_.total_coins_mined;
    // algo_state_.in_cpu_friendly_phase = supply_state_.is_cpu_friendly_phase;
    // algo_state_.in_halving_phase = supply_state_.is_halving_phase;
    // algo_state_.developer_fund_complete = !supply_state_.is_developer_fund_phase;
    // algo_state_.cpu_friendly_complete = supply_state_.is_halving_phase;
}

// ============================================================================
// v0.14.0.1: Bitcoin Core Compatible RPC Mining Interface
// ============================================================================

std::unique_ptr<Block> BlockAssembler::CreateNewBlock(const std::string& coinbase_address) {
    last_template_error_.clear();
    if (!chain_db_) {
        last_template_error_ = "CreateNewBlock: ChainDB not initialized";
        dinero::g_logger.error(last_template_error_);
        return nullptr;
    }

    if (!mempool_) {
        last_template_error_ = "CreateNewBlock: Mempool not initialized";
        dinero::g_logger.error(last_template_error_);
        return nullptr;
    }

    // Get current blockchain state from ChainManager (SINGLE SOURCE OF TRUTH)
    // Phase 39: Get tip from ChainDB (ChainManager deleted)
    auto tip_result = chain_db_->getTip();
    if (tip_result.status() != dinero::Status::Ok) {
        last_template_error_ = "CreateNewBlock: Failed to get chain tip from ChainDB";
        dinero::g_logger.error(last_template_error_);
        return nullptr;
    }
    const auto& tip = tip_result.value();  // Phase 39: Extract tip from Result

    uint32_t height = tip.height + 1;
    const uint256& prev_hash = tip.hash;  // Phase M.0: Keep as uint256

    if (consensus::FullRulesActive(height) && !block_validator_) {
        last_template_error_ = "CreateNewBlock: BlockValidator not wired at height " +
                               std::to_string(height) +
                               " (refusing to create invalid template)";
        dinero::g_logger.error(last_template_error_);
        return nullptr;
    }

    uint32_t mtp = GetMedianTimePast();
    uint32_t current_time = static_cast<uint32_t>(std::time(nullptr));
    uint32_t header_time = std::max(current_time, mtp + 1);
    const Consensus consensus = GetConsensusForCurrentNetwork();
    const int64_t currentTime = static_cast<int64_t>(header_time);
    uint32_t target_bits = GetNextWorkRequiredWithChainDB(
        static_cast<int32_t>(height),
        currentTime,
        consensus,
        chain_db_);
    uint64_t block_subsidy = dinero::ConsensusSubsidy::GetBlockSubsidy(height).GetUna();

    uint64_t total_fees = 0;
    std::vector<std::string> included_txids;
    std::vector<Transaction> selected_txs = selectTransactionsForBlock(
        max_block_weight_,
        height,
        total_fees,
        included_txids
    );

    auto build_candidate = [&](const std::vector<Transaction>& candidate_txs,
                               uint64_t candidate_total_fees,
                               std::string* error_out) -> std::unique_ptr<Block> {
        auto fail = [&](const std::string& msg) -> std::unique_ptr<Block> {
            if (error_out) {
                *error_out = msg;
            }
            return nullptr;
        };

        auto block = std::make_unique<Block>();
        block->header.version = 1;
        block->header.prev_block_hash = prev_hash;
        block->header.timestamp = header_time;
        block->header.timestamp = block->header.timestamp;
        block->header.difficulty = target_bits;
        block->header.difficulty = target_bits;

        Transaction coinbase = createCoinbaseTransaction(
            height,
            coinbase_address,
            block_subsidy,
            candidate_total_fees
        );
        if (coinbase.vin.empty()) {
            const std::string msg = "CreateNewBlock: Failed to create coinbase transaction";
            dinero::g_logger.error(msg);
            return fail(msg);
        }

        block->vtx.reserve(1 + candidate_txs.size());
        block->vtx.push_back(coinbase);
        for (const auto& tx : candidate_txs) {
            block->vtx.push_back(tx);
        }

        if (consensus::FullRulesActive(height)) {
            bool has_witness = false;
            for (const auto& tx : block->vtx) {
                if (tx.HasWitness()) {
                    has_witness = true;
                    break;
                }
            }

            if (has_witness) {
                std::vector<uint8_t> commitment_script =
                    consensus::BuildWitnessCommitment(block->vtx);

                if (!commitment_script.empty()) {
                    TxOutput commitment_output;
                    commitment_output.value = AmountUna::Zero();
                    commitment_output.scriptPubKey = commitment_script;
                    block->vtx[0].vout.push_back(commitment_output);
                }
            }
        }

        if (consensus::FullRulesActive(height)) {
            std::vector<std::vector<uint8_t>> filter_scripts;
            for (const auto& tx : block->vtx) {
                for (const auto& out : tx.vout) {
                    if (!out.scriptPubKey.empty() && out.scriptPubKey[0] != 0x6a) {
                        filter_scripts.push_back(out.scriptPubKey);
                    }
                }
            }

            if (utxo_provider_) {
                for (size_t tx_idx = 0; tx_idx < block->vtx.size(); ++tx_idx) {
                    if (tx_idx == 0) continue;
                    for (const auto& in : block->vtx[tx_idx].vin) {
                        if (in.prevout.txid.IsNull()) continue;
                        OutPoint op(in.prevout.txid, in.prevout.vout);
                        auto utxo_opt = utxo_provider_->GetUTXO(op);
                        if (utxo_opt.has_value() && !utxo_opt->scriptPubKey.empty()) {
                            filter_scripts.push_back(utxo_opt->scriptPubKey);
                        }
                    }
                }
            }

            auto filter = consensus::GCSFilter::Build(filter_scripts, block->header.prev_block_hash);
            if (!filter.IsEmpty()) {
                uint256 filter_hash = filter.GetHash();
                auto commitment_script = consensus::BuildFilterCommitmentScript(filter_hash);

                if (!commitment_script.empty()) {
                    TxOutput filter_output;
                    filter_output.value = AmountUna::Zero();
                    filter_output.scriptPubKey = commitment_script;
                    block->vtx[0].vout.push_back(filter_output);
                }
            }
        }

        std::vector<TxEntryFlags> template_flags;
        template_flags.reserve(block->vtx.size());
        for (size_t i = 0; i < block->vtx.size(); ++i) {
            template_flags.push_back(BuildTxFlags(block->vtx[i], i == 0, 0.0));
        }
        uint256 template_determinism_hash = ComputeTemplateDeterminismHash(block->vtx, template_flags);

        if (utreexo_forest_ && utxo_provider_) {
            consensus::BlockUtreexoData utreexo_data;
            utreexo_data.accumulator_root_before = utreexo_forest_->getCommitment();

            std::vector<consensus::UtreexoHash> proof_targets;
            std::vector<consensus::SpentOutputData> spent_outputs;

            for (size_t i = 0; i < block->vtx.size(); ++i) {
                if (i == 0) {
                    continue;
                }
                const Transaction& tx = block->vtx[i];
                for (const auto& input : tx.vin) {
                    OutPoint outpoint(input.prevout.txid, input.prevout.vout);
                    auto utxo_opt = utxo_provider_->GetUTXO(outpoint);
                    if (!utxo_opt.has_value()) {
                        continue;
                    }
                    const auto& utxo = utxo_opt.value();
                    const uint64_t consensus_value =
                        utxo.is_confidential ? 0 : utxo.value.GetUna();

                    proof_targets.push_back(consensus::HashUTXO(
                        input.prevout.txid.AsUint256(),
                        input.prevout.vout,
                        consensus_value,
                        utxo.scriptPubKey
                    ));

                    spent_outputs.emplace_back(
                        consensus_value,
                        utxo.scriptPubKey,
                        utxo.is_confidential,
                        utxo.commitment
                    );
                }
            }

            consensus::BlockUtreexoProof batch_proof;
            batch_proof.targets = proof_targets;
            batch_proof.proof_hashes = utreexo_forest_->generateBatchProof(proof_targets);
            utreexo_data.spend_proof = batch_proof;
            utreexo_data.spent_outputs = spent_outputs;
            block->utreexo = utreexo_data;

            if (consensus::FullRulesActive(height) && block_validator_) {
                uint256 computed_root;
                std::string utreexo_error;
                if (!block_validator_->ComputeUtreexoRootPure(*block, height, computed_root, utreexo_error)) {
                    const std::string msg =
                        "CreateNewBlock: ComputeUtreexoRootPure failed: " + utreexo_error;
                    dinero::g_logger.error(msg);
                    return fail(msg);
                }
                block->header.utreexo_root = computed_root;
            } else if (!consensus::FullRulesActive(height)) {
                dinero::g_logger.debug("[Utreexo Miner] Skipping commitment (height " +
                    std::to_string(height) + " is pre-activation)");
            } else {
                const std::string msg =
                    "CreateNewBlock: BlockValidator not set for full-rules template";
                dinero::g_logger.error(msg);
                return fail(msg);
            }
        } else if (utreexo_forest_) {
            dinero::g_logger.warning("⚠️  [Utreexo Miner] Forest available but no UTXO provider - skipping proof generation");
        }

        if (consensus::FullRulesActive(height) && block->header.utreexo_root.IsNull()) {
            const std::string msg = "CreateNewBlock: null utreexo root at height " +
                std::to_string(height) + " (aborting)";
            dinero::g_logger.error(msg);
            return fail(msg);
        }

        block->header.merkle_root = uint256::FromHexUnsafe(calculateMerkleRoot(block->vtx));
        block->header.nonce = 0;

        std::vector<TxEntryFlags> final_flags;
        final_flags.reserve(block->vtx.size());
        for (size_t i = 0; i < block->vtx.size(); ++i) {
            final_flags.push_back(BuildTxFlags(block->vtx[i], i == 0, 0.0));
        }
        uint256 final_determinism_hash = ComputeTemplateDeterminismHash(block->vtx, final_flags);

        if (template_determinism_hash != final_determinism_hash) {
            const std::string msg =
                "CreateNewBlock: template determinism hash changed during assembly";
            dinero::g_logger.error(msg);
            return fail(msg);
        }

        if (consensus::FullRulesActive(height) && block_validator_ &&
            !block->header.utreexo_root.IsNull()) {
            uint256 verify_root;
            std::string verify_error;
            if (!block_validator_->ComputeUtreexoRootPure(*block, height, verify_root, verify_error)) {
                const std::string msg =
                    "[GATE3] Cross-check oracle failed: " + verify_error;
                dinero::g_logger.error(msg);
                return fail(msg);
            }
            if (verify_root != block->header.utreexo_root) {
                const std::string msg =
                    "[GATE3] Utreexo root mismatch after template finalization";
                dinero::g_logger.error(msg);
                return fail(msg);
            }
        }

        return block;
    };

    auto fee_for_tx = [&](const Transaction& tx) -> uint64_t {
        auto fee_opt = mempool_->getTransactionFee(tx.GetTxid().AsUint256());
        return fee_opt.has_value() ? fee_opt.value() : 0;
    };

    if (utxo_provider_ && !selected_txs.empty()) {
        std::unordered_set<uint256> deferred_txids;
        auto filtered_txs = FilterChainBackedTemplateTransactions(
            selected_txs,
            [&](const OutPoint& outpoint) {
                return utxo_provider_->GetUTXO(outpoint).has_value();
            },
            &deferred_txids);

        if (filtered_txs.size() != selected_txs.size()) {
            std::unordered_set<uint256> candidate_txids;
            candidate_txids.reserve(selected_txs.size());
            for (const auto& tx : selected_txs) {
                candidate_txids.insert(tx.GetTxid().AsUint256());
            }

            size_t quarantined_deferred = 0;
            for (const auto& tx : selected_txs) {
                const auto txid = tx.GetTxid().AsUint256();
                if (deferred_txids.count(txid) == 0) {
                    continue;
                }

                bool has_package_parent = false;
                for (const auto& input : tx.vin) {
                    if (candidate_txids.count(input.prevout.txid.AsUint256()) != 0) {
                        has_package_parent = true;
                        break;
                    }
                }

                // Only quarantine roots that have neither a confirmed parent
                // nor an in-package parent candidate. Legitimate package
                // children stay retryable; pure poison gets suppressed.
                if (!has_package_parent) {
                    mempool_->excludeFromBlockTemplates(
                        txid,
                        "template deferred after non-chain-backed inputs: parent missing from chain and package");
                    ++quarantined_deferred;
                }
            }

            total_fees = 0;
            included_txids.clear();
            included_txids.reserve(filtered_txs.size());
            for (const auto& tx : filtered_txs) {
                total_fees += fee_for_tx(tx);
                included_txids.push_back(tx.GetTxid().AsUint256().GetHex());
            }
            dinero::g_logger.warning(
                "CreateNewBlock: deferred " +
                std::to_string(selected_txs.size() - filtered_txs.size()) +
                " tx(s) with non-chain-backed inputs until parent confirmations land");
            if (quarantined_deferred > 0) {
                dinero::g_logger.warning(
                    "CreateNewBlock: quarantined " +
                    std::to_string(quarantined_deferred) +
                    " root tx(s) with non-chain-backed inputs from future templates");
            }
            selected_txs = std::move(filtered_txs);
        }
    }

    std::string build_error;
    auto block = build_candidate(selected_txs, total_fees, &build_error);

    if (!block && !selected_txs.empty()) {
        std::string coinbase_only_error;
        auto coinbase_only_block = build_candidate({}, 0, &coinbase_only_error);

        if (!coinbase_only_block) {
            last_template_error_ = "CreateNewBlock: template failed even with coinbase-only candidate: " +
                                   coinbase_only_error;
            dinero::g_logger.error(last_template_error_);
            block_template_stats_.determinism_hash = uint256();
            return nullptr;
        }

        dinero::g_logger.warning("CreateNewBlock: template poisoned by mempool candidate set (" +
                                 std::to_string(selected_txs.size()) +
                                 " txs): " + build_error);

        bool healed_via_missing_prevout = false;
        {
            std::vector<Transaction> narrowed_txs = selected_txs;
            std::string narrowed_error = build_error;
            std::unordered_set<uint256> quarantined_roots;

            for (size_t attempt = 0; attempt < selected_txs.size(); ++attempt) {
                auto missing_prevout = ParseTemplatePoisonMissingPrevout(narrowed_error);
                if (!missing_prevout.has_value()) {
                    break;
                }

                std::unordered_set<uint256> direct_spenders;
                auto removal_set = CollectTemplatePoisonRemovalSet(
                    narrowed_txs,
                    missing_prevout.value(),
                    &direct_spenders);
                if (removal_set.empty()) {
                    break;
                }

                std::vector<Transaction> filtered_txs;
                uint64_t filtered_fees = 0;
                filtered_txs.reserve(narrowed_txs.size());

                for (const auto& tx : narrowed_txs) {
                    const auto txid = tx.GetTxid().AsUint256();
                    if (removal_set.count(txid) != 0) {
                        continue;
                    }
                    filtered_fees += fee_for_tx(tx);
                    filtered_txs.push_back(tx);
                }

                quarantined_roots.insert(direct_spenders.begin(), direct_spenders.end());

                std::string rescue_error;
                auto rescued_block = build_candidate(filtered_txs, filtered_fees, &rescue_error);
                if (rescued_block) {
                    for (const auto& culprit_txid : quarantined_roots) {
                        mempool_->excludeFromBlockTemplates(
                            culprit_txid,
                            "template self-heal after missing pure leaf: " + build_error
                        );
                    }

                    if (!quarantined_roots.empty()) {
                        dinero::g_logger.warning(
                            "CreateNewBlock: quarantined " +
                            std::to_string(quarantined_roots.size()) +
                            " root template-poisoning tx(s) after missing-prevout isolation");
                    }

                    selected_txs = std::move(filtered_txs);
                    included_txids.clear();
                    included_txids.reserve(selected_txs.size());
                    for (const auto& tx : selected_txs) {
                        included_txids.push_back(tx.GetTxid().AsUint256().GetHex());
                    }
                    total_fees = filtered_fees;
                    block = std::move(rescued_block);
                    healed_via_missing_prevout = true;
                    break;
                }

                narrowed_txs = std::move(filtered_txs);
                narrowed_error = rescue_error;
            }
        }

        if (!healed_via_missing_prevout) {
            for (size_t i = selected_txs.size(); i-- > 0;) {
                const uint256 culprit_txid = selected_txs[i].GetTxid().AsUint256();
                std::unordered_set<uint256> removal_set{culprit_txid};

                bool changed = true;
                while (changed) {
                    changed = false;
                    for (const auto& tx : selected_txs) {
                        const uint256 txid = tx.GetTxid().AsUint256();
                        if (removal_set.count(txid) != 0) {
                            continue;
                        }
                        for (const auto& input : tx.vin) {
                            if (removal_set.count(input.prevout.txid.AsUint256()) != 0) {
                                removal_set.insert(txid);
                                changed = true;
                                break;
                            }
                        }
                    }
                }

                std::vector<Transaction> filtered_txs;
                std::vector<std::string> filtered_txids;
                uint64_t filtered_fees = 0;
                filtered_txs.reserve(selected_txs.size());
                filtered_txids.reserve(included_txids.size());

                for (size_t j = 0; j < selected_txs.size(); ++j) {
                    const uint256 txid = selected_txs[j].GetTxid().AsUint256();
                    if (removal_set.count(txid) != 0) {
                        continue;
                    }
                    filtered_txs.push_back(selected_txs[j]);
                    filtered_txids.push_back(included_txids[j]);
                    filtered_fees += fee_for_tx(selected_txs[j]);
                }

                std::string rescue_error;
                auto rescued_block = build_candidate(filtered_txs, filtered_fees, &rescue_error);
                if (!rescued_block) {
                    continue;
                }

                mempool_->excludeFromBlockTemplates(
                    culprit_txid,
                    "template self-heal after block assembly failure: " + build_error
                );

                dinero::g_logger.warning("CreateNewBlock: quarantined template-poisoning tx " +
                                         culprit_txid.GetHex().substr(0, 16) + "... and skipped " +
                                         std::to_string(removal_set.size() - 1) +
                                         " dependent tx(s)");

                selected_txs = std::move(filtered_txs);
                included_txids = std::move(filtered_txids);
                total_fees = filtered_fees;
                block = std::move(rescued_block);
                break;
            }

            if (!block) {
                dinero::g_logger.error(
                    std::string("CreateNewBlock: unable to isolate a single poisoning tx; ") +
                    "falling back to coinbase-only template");
                selected_txs.clear();
                included_txids.clear();
                total_fees = 0;
                block = std::move(coinbase_only_block);
            }
        }
    }

    if (!block) {
        last_template_error_ = "CreateNewBlock: Failed to create block template: " + build_error;
        dinero::g_logger.error(last_template_error_);
        block_template_stats_.determinism_hash = uint256();
        return nullptr;
    }

    last_template_error_.clear();

    std::vector<TxEntryFlags> final_flags;
    final_flags.reserve(block->vtx.size());
    for (size_t i = 0; i < block->vtx.size(); ++i) {
        final_flags.push_back(BuildTxFlags(block->vtx[i], i == 0, 0.0));
    }
    block_template_stats_.determinism_hash = ComputeTemplateDeterminismHash(block->vtx, final_flags);
    block_template_stats_.total_txs = block->vtx.size();
    block_template_stats_.total_fees = total_fees;
    block_template_stats_.block_weight = calculateBlockWeight(block->vtx);
    block_template_stats_.block_size = calculateBlockSize(block->vtx);
    block_template_stats_.mempool_size = mempool_->size();
    block_template_stats_.rejected_txs = mempool_->size() - selected_txs.size();
    block_template_stats_.height = height;
    block_template_stats_.prev_block = prev_hash.GetHex();
    block_template_stats_.witness_nonce_offset = 0;
    block_template_stats_.witness_nonce_size = 0;
    if (!block->vtx.empty() && block->vtx[0].IsCoinbase()) {
        block_template_stats_.witness_nonce_offset = ComputeWitnessNonceOffset(block->vtx[0]);
        block_template_stats_.witness_nonce_size = 8;
    }

    dinero::g_logger.info("CreateNewBlock: height=" + std::to_string(height) +
                         " txs=" + std::to_string(block->vtx.size()) +
                         " fees=" + std::to_string(total_fees / COIN) + " DIN" +
                         " weight=" + std::to_string(block_template_stats_.block_weight) +
                         " size=" + std::to_string(block_template_stats_.block_size));

    return block;
}

std::vector<Transaction> BlockAssembler::selectTransactionsForBlock(
    uint32_t max_weight,
    uint32_t target_height,
    uint64_t& total_fees_out,
    std::vector<std::string>& included_txids_out
) {
    // Phase W.1.3: Use intelligent selection if enabled
    if (use_intelligent_selection_ && block_relay_manager_) {
        return selectTransactionsIntelligent(max_weight, target_height, total_fees_out, included_txids_out);
    }

    // Default: Use standard CPFP-aware selection
    std::vector<Transaction> selected;
    total_fees_out = 0;
    included_txids_out.clear();

    if (!mempool_) {
        return selected;
    }

    // Phase 34: Use mempool's selectTransactionsForBlock() (CPFP-aware)
    // The daemon::Mempool already implements:
    // - Ancestor fee rate calculation (CPFP)
    // - Deterministic sorting (by ancestor feerate)
    // - Package selection (ensures ancestors included before children)
    // - Weight/size limit enforcement
    size_t max_size = max_weight / 4;  // Approximate: weight / 4 = size
    auto candidates = mempool_->selectTransactionsForBlock(max_size, max_weight,
                                                           target_height);

    // Apply CT selection policy if enabled
    size_t ct_count = 0;
    size_t ct_proof_bytes = 0;

    // Phase 8.5: running VWU tally for the miner-policy cap. Starts at 0
    // so the first tx always fits; cap of 0 means "disabled."
    uint64_t template_vwu = 0;
    size_t   vwu_excluded = 0;

    // Phase 8.5 Commit 2: per-scheme P2MR input counters. Belt-and-
    // suspenders against VWU mispricing — even if a scheme's
    // verify_cost_weight undercounts, the input-count ceiling keeps
    // total verify time bounded. Keyed on the P2MR witness blob's
    // scheme_id first byte.
    std::unordered_map<uint8_t, uint32_t> scheme_input_counts;
    size_t scheme_cap_excluded = 0;

    for (const auto& tx : candidates) {
        // Check if CT is disabled
        if (!ct_enabled_ && ct_policy_ && ct_policy_->HasConfidentialOutputs(tx)) {
            continue;  // Skip CT transactions when CT mining is disabled
        }

        // Apply CT policy checks
        if (ct_policy_ && ct_policy_->HasConfidentialOutputs(tx)) {
            auto policy_result = ct_policy_->CheckPolicy(tx, target_height, ct_count, ct_proof_bytes);
            if (!policy_result.acceptable) {
                dinero::g_logger.debug("CT tx rejected by policy: " + policy_result.rejection_reason);
                continue;  // Skip this CT transaction
            }

            // Update CT counters
            auto weight_info = ct_policy_->GetWeightInfo(tx);
            ct_count++;
            ct_proof_bytes += weight_info.proof_bytes;
        }

        // Phase 8.5 Commit 2: per-scheme input-count ceiling. Scan this
        // tx's inputs, count how many are P2MR spends of each scheme,
        // and skip the whole tx if adding it would push any scheme's
        // per-block count past the configured cap. This is a secondary
        // gate — most txs never touch P2MR and pay zero cost here.
        std::unordered_map<uint8_t, uint32_t> tx_scheme_inputs;
        for (const auto& input : tx.vin) {
            if (input.witness.size() != 1 || input.witness[0].empty()) continue;
            // A fresh, non-coinbase input with exactly one canonical
            // witness blob whose first byte is the scheme_id matches
            // our P2MR shape. Unique to Phase 6 P2MR witnesses; legacy
            // Taproot spends have a 64/65-byte sig so they won't
            // collide even if their first byte happens to match.
            const uint8_t scheme_id = input.witness[0][0];
            // Only count schemes the registry recognizes as live.
            if (!dinero::consensus::pq::IsSchemeAcceptedAtHeight(
                    scheme_id, target_height) &&
                !dinero::consensus::pq::IsSchemeDarkReserved(scheme_id)) {
                continue;
            }
            ++tx_scheme_inputs[scheme_id];
        }

        bool scheme_cap_hit = false;
        for (const auto& [scheme_id, add] : tx_scheme_inputs) {
            auto cap_it = max_p2mr_inputs_per_scheme_.find(scheme_id);
            if (cap_it == max_p2mr_inputs_per_scheme_.end()) continue;
            const uint32_t cap = cap_it->second;
            const uint32_t current = scheme_input_counts[scheme_id];
            if (current + add > cap) {
                scheme_cap_hit = true;
                break;
            }
        }
        if (scheme_cap_hit) {
            ++scheme_cap_excluded;
            continue;  // skip — a smaller tx later may still fit
        }

        // Phase 8.5: miner-policy VWU cap. Candidates are already sorted
        // by fee/VWU descending, so skipping on overflow drops lower-value
        // txs first — the cap trims the tail, doesn't reorder. This is
        // deliberately NOT a break: a later smaller tx might still fit
        // under the cap after a larger one was skipped.
        if (max_block_vwu_ > 0) {
            const uint64_t tx_vwu = mempool_->computeVWUForTx(tx);
            if (template_vwu + tx_vwu > max_block_vwu_) {
                ++vwu_excluded;
                continue;
            }
            template_vwu += tx_vwu;
        }

        // Passed both caps — commit the per-scheme counter updates.
        for (const auto& [scheme_id, add] : tx_scheme_inputs) {
            scheme_input_counts[scheme_id] += add;
        }
        selected.push_back(tx);
    }

    if (vwu_excluded > 0) {
        dinero::g_logger.info(
            "selectTransactionsForBlock: miner-policy VWU cap trimmed " +
            std::to_string(vwu_excluded) + " tx(s) (template VWU=" +
            std::to_string(template_vwu) + "/" + std::to_string(max_block_vwu_) + ")");
    }
    if (scheme_cap_excluded > 0) {
        std::string scheme_tally;
        for (const auto& [scheme_id, cnt] : scheme_input_counts) {
            scheme_tally += " scheme_id=0x" +
                std::to_string(static_cast<unsigned>(scheme_id)) +
                ":" + std::to_string(cnt);
        }
        dinero::g_logger.info(
            "selectTransactionsForBlock: per-scheme cap trimmed " +
            std::to_string(scheme_cap_excluded) + " tx(s);" + scheme_tally);
    }

    // Calculate total fees and populate txid list
    for (const auto& tx : selected) {
        // Phase M.4: GetTxid() returns TxId, extract uint256 then GetHex()
        TxId txid = tx.GetTxid();
        std::string txid_hex = txid.AsUint256().GetHex();
        included_txids_out.push_back(txid_hex);

        // Get fee from mempool (Phase M.4: Convert TxId to uint256 for mempool API)
        auto fee_opt = mempool_->getTransactionFee(txid.AsUint256());
        if (fee_opt.has_value()) {
            total_fees_out += fee_opt.value();
        }
    }

    // Apply batch optimization if CT policy is set
    if (ct_policy_ && ct_policy_->ShouldUseBatchVerification(ct_count)) {
        selected = ct_policy_->OptimizeForBatchVerification(std::move(selected));
        dinero::g_logger.debug("Applied CT batch optimization for " + std::to_string(ct_count) + " CT transactions");
    }

    dinero::g_logger.debug("selectTransactionsForBlock: selected " + std::to_string(selected.size()) +
                          " txs (" + std::to_string(ct_count) + " CT) with total fees " + std::to_string(total_fees_out) +
                          " (CPFP-aware ancestor feerate sorting)");

    return selected;
}

void BlockAssembler::SetMaxP2MRInputsPerBlock(uint8_t scheme_id, uint32_t max_inputs) {
    if (max_inputs == 0) {
        max_p2mr_inputs_per_scheme_.erase(scheme_id);
        return;
    }
    max_p2mr_inputs_per_scheme_[scheme_id] = max_inputs;
}

uint32_t BlockAssembler::GetMaxP2MRInputsPerBlock(uint8_t scheme_id) const {
    auto it = max_p2mr_inputs_per_scheme_.find(scheme_id);
    if (it == max_p2mr_inputs_per_scheme_.end()) {
        return 0;  // "no cap set"
    }
    return it->second;
}

Transaction BlockAssembler::createCoinbaseTransaction(
    uint32_t height,
    const std::string& coinbase_address,
    uint64_t block_subsidy,
    uint64_t total_fees
) {
    Transaction coinbase;
    coinbase.version = 1;
    coinbase.lockTime = 0;
    // ========================================================================
    // WITNESS-BASED EXTRANONCE (Utreexo-compatible)
    // ========================================================================
    // Utreexo commits to UTXO outpoints which include coinbase txid.
    // If miners control coinbase scriptSig (where extranonce traditionally lives),
    // they control txid, which breaks Utreexo's invariant.
    //
    // Solution: Move extranonce from scriptSig to witness data.
    // Witness doesn't affect txid (SegWit design), so:
    //   - txid remains daemon-controlled
    //   - Miner gets entropy via witness nonce
    //   - Utreexo commitment stays valid
    // ========================================================================
    coinbase.witness_version = 0;  // Enable SegWit serialization (marker+flag)

    // Create coinbase input
    TxInput coinbase_input;
    // Phase M.4: prevout.txid is TxId, wrap uint256 in TxId constructor
    coinbase_input.prevout.txid = TxId(uint256::FromHexUnsafe(std::string(64, '0')));
    coinbase_input.prevout.vout = 0xffffffff;
    coinbase_input.sequence = 0xffffffff;

    // Coinbase scriptSig: height (BIP34) - Bitcoin Script number encoding
    // IMPORTANT: Must use PUSHDATA format (not opcodes) because validator reads raw bytes
    // NOTE: scriptSig is IMMUTABLE for Utreexo - miner entropy goes in witness only
    consensus::Script script;
    std::vector<uint8_t> heightBytes = consensus::scriptNumEncode(static_cast<int64_t>(height));
    script.pushData(heightBytes);  // Force PUSHDATA format: <length> <data>
    coinbase_input.scriptSig = script.data();

    // ========================================================================
    // WITNESS NONCE: 8-byte placeholder for miner extranonce
    // ========================================================================
    // Single witness item: 8 zero bytes (miners will replace with their extranonce)
    // This provides 2^64 = 18 quintillion unique values - sufficient for any hashrate
    std::vector<uint8_t> witness_nonce(8, 0x00);
    coinbase_input.witness.push_back(witness_nonce);

    coinbase.vin.push_back(coinbase_input);

    // Create single coinbase output: subsidy + fees (Bitcoin-correct)
    auto total_output = dinero::CheckedAddUna(block_subsidy, total_fees);
    if (!total_output) {
        dinero::g_logger.error("createCoinbaseTransaction: coinbase value overflow");
        return Transaction{};
    }

    std::string payout_address = coinbase_address;

    // Build scriptPubKey from address (MUST be Taproot P2TR)
    std::vector<uint8_t> script_pubkey;
    std::string error;
    if (!dinero::BuildScriptPubKeyFromAddress(payout_address, script_pubkey, error)) {
        dinero::g_logger.error("createCoinbaseTransaction: CRITICAL - Failed to build P2TR scriptPubKey: " +
                               payout_address + " - " + error);
        // NO FALLBACK: Return empty transaction to signal failure
        // Mining with invalid address must fail, not create unspendable outputs
        return Transaction{};
    }

    // v7 coinbase policy: accept P2TR (OP_1 + PUSH32) or P2MR (OP_3 + PUSH32),
    // 34 bytes each.
    const bool is_p2tr = script_pubkey.size() == 34 && script_pubkey[0] == 0x51 && script_pubkey[1] == 0x20;
    const bool is_p2mr = script_pubkey.size() == 34 && script_pubkey[0] == 0x53 && script_pubkey[1] == 0x20;
    if (!is_p2tr && !is_p2mr) {
        dinero::g_logger.error("createCoinbaseTransaction: Address produced non-coinbase-eligible script. "
                               "Mining requires Taproot (din1p...) or P2MR (din1r...) addresses.");
        return Transaction{};
    }

    TxOutput coinbase_output;
    // Phase M.6.2: Wrap raw value in AmountUna
    coinbase_output.value = AmountUna::Una(*total_output);
    coinbase_output.scriptPubKey = script_pubkey;

    coinbase.vout.push_back(coinbase_output);

    return coinbase;
}

std::string BlockAssembler::calculateMerkleRoot(const std::vector<Transaction>& transactions) {
    // Reuse existing CalculateMerkleRoot implementation
    return CalculateMerkleRoot(transactions);
}

uint64_t BlockAssembler::calculateBlockWeight(const std::vector<Transaction>& transactions) const {
    uint64_t total_weight = 0;
    for (const auto& tx : transactions) {
        total_weight += tx.GetWeight();
    }
    return total_weight;
}

size_t BlockAssembler::calculateBlockSize(const std::vector<Transaction>& transactions) const {
    size_t total_size = 80;  // Block header size
    for (const auto& tx : transactions) {
        total_size += tx.GetSize();
    }
    return total_size;
}

// CPFP helpers (v0.14.0.2)
double BlockAssembler::calculateAncestorFeerate(const std::string& txid) const {
    // v0.14.0.2: Use mempool's ancestor feerate from MempoolEntry
    if (!mempool_) return 0.0;

    // Phase M.0: Convert hex txid to uint256
    auto entry_opt = mempool_->getMempoolEntry(uint256::FromHexUnsafe(txid));
    if (!entry_opt.has_value()) {
        return 0.0;
    }

    // Return the CT-aware package selection score from mempool.
    if (entry_opt.value().ancestor_adjusted_feerate > 0.0) {
        return entry_opt.value().ancestor_adjusted_feerate;
    }
    return entry_opt.value().adjusted_fee_rate;
}

std::vector<std::string> BlockAssembler::getUnconfirmedAncestors(const std::string& txid) const {
    // v0.14.0.2: Return ancestors from mempool entry
    if (!mempool_) return {};

    // Phase M.0: Convert hex txid to uint256
    auto entry_opt = mempool_->getMempoolEntry(uint256::FromHexUnsafe(txid));
    if (!entry_opt.has_value()) {
        return {};
    }

    // Phase M.0: Convert uint256 set to hex string vector at RPC boundary
    std::vector<std::string> depends_hex;
    for (const auto& dep_txid : entry_opt.value().depends) {
        depends_hex.push_back(dep_txid.GetHex());
    }
    return depends_hex;
}

// ============================================================================
// Phase W.1.3: Intelligent Transaction Selection
// ============================================================================

std::vector<Transaction> BlockAssembler::selectTransactionsIntelligent(
    uint32_t max_weight,
    uint32_t target_height,
    uint64_t& total_fees_out,
    std::vector<std::string>& included_txids_out
) {
    std::vector<Transaction> selected;
    total_fees_out = 0;
    included_txids_out.clear();

    if (!mempool_ || !block_relay_manager_) {
        dinero::g_logger.warning("selectTransactionsIntelligent: Missing mempool or block_relay_manager");
        return selected;
    }

    // Step 1: Get current time
    uint64_t current_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // Step 2: Create BlockAssemblyContext from current network state (Phase W.1.4: use actual last_template_time)
    auto context = BlockAssemblyContext::CreateFromNetworkState(
        mempool_,
        block_relay_manager_,
        last_template_time_ms_
    );

    dinero::g_logger.info("Intelligent selection: sync_phase=" +
                         std::to_string(static_cast<int>(context.sync_phase)) +
                         ", mempool_pressure=" + std::to_string(context.mempool_pressure) +
                         ", compact_success=" + std::to_string(context.compact_success_rate) +
                         ", template_age=" + std::to_string((current_time_ms - last_template_time_ms_) / 1000) + "s");

    // Step 3: Get all transactions from mempool (CPFP-aware sorted)
    size_t max_size = max_weight / 4;  // Approximate: weight / 4 = size
    auto all_txs = mempool_->selectTransactionsForBlock(max_size * 2, max_weight * 2,
                                                        target_height);  // Get more candidates

    if (all_txs.empty()) {
        dinero::g_logger.debug("selectTransactionsIntelligent: No transactions in mempool");
        return selected;
    }

    // Step 4: Prepare transactions for scoring
    std::vector<std::tuple<uint256, uint64_t, uint64_t>> tx_data;
    tx_data.reserve(all_txs.size());

    for (const auto& tx : all_txs) {
        TxId txid_semantic = tx.GetTxid();  // Phase M.4: GetTxid() returns TxId
        uint256 txid = txid_semantic.AsUint256();  // Convert for mempool/scorer API

        // Get fee and entry time from mempool
        auto fee_opt = mempool_->getTransactionFee(txid);
        if (!fee_opt.has_value()) continue;

        uint64_t fee = fee_opt.value();
        size_t tx_size = tx.GetSize();
        if (tx_size == 0) continue;

        // Phase 8 Commit 3: sort-score denominator is VWU, not raw bytes.
        // Keeps the intelligent-selection path coherent with the
        // CPFP-aware mempool selector (which also uses VWU after Commit
        // 2) so a P2MR spend isn't preferentially picked just because
        // its ML-DSA witness bytes got the BIP141 4× discount.
        const uint64_t vwu = mempool_->computeVWUForTx(tx);
        const uint64_t denom = vwu > 0 ? vwu : static_cast<uint64_t>(tx_size);
        uint64_t fee_rate = fee / denom;  // una / VWU

        // Get entry time (stub for now - would need mempool support)
        uint64_t entry_time_ms = current_time_ms - 60000;  // Assume 1 minute ago (conservative)

        tx_data.push_back({txid, fee_rate, entry_time_ms});
    }

    // Step 5: Score all transactions
    TransactionScorer scorer(context);
    auto scored_txs = scorer.ScoreTransactions(tx_data, current_time_ms);

    dinero::g_logger.debug("selectTransactionsIntelligent: Scored " + std::to_string(scored_txs.size()) + " transactions");

    // Step 6: Select top-scored transactions up to weight limit
    uint32_t current_weight = 0;
    std::unordered_set<uint256> selected_set;  // For fast lookup

    for (const auto& scored : scored_txs) {
        // Find the transaction
        auto it = std::find_if(all_txs.begin(), all_txs.end(),
                              [&scored](const Transaction& tx) {
                                  // Phase M.4: GetTxid() returns TxId, convert to uint256 for comparison
                                  return tx.GetTxid().AsUint256() == scored.txid;
                              });

        if (it == all_txs.end()) continue;

        const Transaction& tx = *it;
        uint32_t tx_weight = tx.GetSize() * 4;  // Approximate weight

        if (current_weight + tx_weight > max_weight) {
            break;  // Block full
        }

        // Add transaction
        selected.push_back(tx);
        selected_set.insert(scored.txid);
        current_weight += tx_weight;

        // Track fees and txids
        auto fee_opt = mempool_->getTransactionFee(scored.txid);
        if (fee_opt.has_value()) {
            total_fees_out += fee_opt.value();
        }
        included_txids_out.push_back(scored.txid.GetHex());
    }

    dinero::g_logger.info("selectTransactionsIntelligent: selected " + std::to_string(selected.size()) +
                         " txs with total fees " + std::to_string(total_fees_out) +
                         " (context-aware scoring, weight=" + std::to_string(current_weight) + "/" +
                         std::to_string(max_weight) + ")");

    // Step 7: Track template state for incremental refresh (Phase W.1.4)
    last_template_txids_ = selected_set;
    last_template_time_ms_ = current_time_ms;

    dinero::g_logger.debug("Template snapshot: " + std::to_string(last_template_txids_.size()) +
                          " txids tracked for delta comparison");

    return selected;
}

} // namespace dinero
