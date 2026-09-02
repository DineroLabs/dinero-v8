#include "daemon/mempool.h"
#include "dinero/compat/int128.hpp"
#include "primitives/block.h"  // Block type for onBlockConnected/onBlockDisconnected
#include "storage/chain_db.h"  // ChainDB - single source of truth for blockchain state
#include "daemon/p2p_message.h"
#include "daemon/tx_mempool.h"  // For UTXOView interface
#include "mining/address_validator.h"
#include "privacy/silent_scanner_manager.h"
#include "common/ilogger.h"
#include "policy/rbf_policy.h"  // BIP125 RBF validation
#include "mempool/mempool_persistence.h"  // v0.13.0.2 - Mempool persistence
#include "mempool/fee_estimator.h"  // v0.13.0.3 - Fee estimation
#include "consensus/covenant_activation.h"
#include "consensus/covenants.h"
#include "policy/covenant_relay_policy.h"
#include "consensus/script_interpreter.h"  // Phase L0.4: Script verification with consensus flags
#include "consensus/script.h"              // Phase L0.4: For Script class
#include "consensus/script_validation.h"   // Phase 6 Commit 4: unified ValidateSpend dispatcher (single consensus script validator)
#include "consensus/pq/p2mr_consensus.h"   // Phase 8 Commit 2: ComputeVWU for fee/eviction/RBF ordering
#include "consensus/chainparams.h"
#include "consensus/interfaces/iconsensus_utxo_set.h"
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/shielded_serialization.h"
#include "consensus/shielded/shielded_validation.h"
#include "consensus/shielded/shielded_epoch.h"
#include <algorithm>
#include <chrono>
#include <numeric>
#include <iostream>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

// NOTE:
// Mempool identity and dependency tracking use uint256/OutPoint end-to-end.
// Keep string conversion at logging and RPC boundaries only.

namespace dinero {

namespace {

bool UsesShieldedValueSemantics(const Transaction& tx) {
    return Transaction::IsShieldedVersion(tx.version) ||
           !tx.shielded_bundle_bytes.empty();
}

bool ComputeTransparentValueDelta(uint64_t total_input_value,
                                  uint64_t total_output_value,
                                  uint64_t fee,
                                  int64_t& delta,
                                  std::string& error) {
    using dinero::compat::i128;
    using dinero::compat::i128_zext_u64;
    // i128_zext_u64 mirrors the original `(__int128)uint64_t` zero-extend
    // semantics; i128(uint64_t) would sign-extend on the struct backend
    // and corrupt high-bit-set values.
    const i128 signed_delta =
        i128_zext_u64(total_input_value) -
        i128_zext_u64(total_output_value) -
        i128_zext_u64(fee);
    if (signed_delta < i128(std::numeric_limits<int64_t>::min()) ||
        signed_delta > i128(std::numeric_limits<int64_t>::max())) {
        error = "Shielded transparent value delta out of range";
        return false;
    }
    delta = static_cast<int64_t>(signed_delta);
    return true;
}

const char* ShieldedValidationErrorToString(
    consensus::shielded::ShieldedValidationError err) {
    using consensus::shielded::ShieldedValidationError;
    switch (err) {
        case ShieldedValidationError::Ok:                   return "ok";
        case ShieldedValidationError::NullifierDuplicate:   return "nullifier-duplicate";
        case ShieldedValidationError::AnchorInvalid:        return "anchor-invalid";
        case ShieldedValidationError::ProofInvalid:         return "proof-invalid";
        case ShieldedValidationError::ValueBalanceMismatch: return "value-balance-mismatch";
        case ShieldedValidationError::BindingSigInvalid:    return "binding-sig-invalid";
        case ShieldedValidationError::BundleMalformed:      return "bundle-malformed";
        case ShieldedValidationError::NotActive:            return "shielded-not-active";
        case ShieldedValidationError::BundleTooLarge:       return "bundle-too-large";
        case ShieldedValidationError::RangeProofInvalid:    return "range-proof-invalid";
    }
    return "unknown";
}

} // namespace

// TxRejectCodeToString moved to daemon/interfaces/ingress_types.cpp (Step 5)

// ChainDB-backed UTXO view for mempool validation
// Provides read-only access to chain state for transaction validation
// Phase M.0: Updated to use uint256 txid
class ChainDBUTXOView : public UTXOView {
    ChainDB* chain_db_;  // Non-owning pointer (mempool doesn't own ChainDB)
public:
    explicit ChainDBUTXOView(ChainDB* chain_db) : chain_db_(chain_db) {}

    bool HaveUTXO(const uint256& txid, uint32_t vout) const override {
        if (!chain_db_) return false;

        // Phase M.0: txid is uint256, pass directly
        auto coin_result = chain_db_->getCoinWithConfidentialFallback(txid, vout);
        return coin_result.status() == Status::Ok;
    }

    bool GetUTXO(const uint256& txid, uint32_t vout, uint64_t& value, std::string& script) const override {
        if (!chain_db_) return false;

        // Phase M.0: txid is uint256, pass directly
        auto coin_result = chain_db_->getCoinWithConfidentialFallback(txid, vout);
        if (coin_result.status() != Status::Ok) {
            return false;
        }

        const Coin& coin = coin_result.value();
        value = coin.amount;
        script = coin.script_pubkey;
        return true;
    }

    bool HaveTransaction(const uint256& txid) const override {
        if (!chain_db_) return false;
        auto tx_location = chain_db_->getTxLocation(txid);
        return tx_location.status() == Status::Ok;
    }

    uint32_t GetHeight() const override {
        if (!chain_db_) return 0;

        auto tip_result = chain_db_->getTip();
        if (tip_result.status() != Status::Ok) {
            return 0;
        }

        return static_cast<uint32_t>(tip_result.value().height);
    }

    bool GetConfidentialUTXO(const uint256& txid, uint32_t vout,
                            std::vector<uint8_t>& commitment, bool& is_confidential) const override {
        if (!chain_db_) return false;

        auto coin_result = chain_db_->getCoinWithConfidentialFallback(txid, vout);
        if (coin_result.status() != Status::Ok) {
            return false;
        }

        const Coin& coin = coin_result.value();
        is_confidential = coin.is_confidential;
        commitment = coin.commitment;
        return true;
    }

    bool HaveCommitment(const std::vector<uint8_t>& commitment) const override {
        if (!chain_db_ || commitment.empty()) {
            return false;
        }

        bool found = false;
        chain_db_->forEachUTXO([&](const uint256&, uint32_t, const Coin& coin) {
            if (coin.is_confidential && coin.commitment == commitment) {
                found = true;
                return false;
            }
            return true;
        });
        return found;
    }
};

// ChainDB adapter implementing ChainStateView interface for CoinsViewMemPool
// Phase M.1: CoinsViewMemPool requires ChainStateView, but we have ChainDB
class ChainDBStateView : public consensus::ChainStateView {
    ChainDB* chain_db_;  // Non-owning pointer
public:
    explicit ChainDBStateView(ChainDB* chain_db) : chain_db_(chain_db) {}

    StatusOr<consensus::UTXOEntry> getCoin(const OutPoint& outpoint) const override {
        if (!chain_db_) {
            return Status::NotFound;
        }

        // ChainDB::getCoin returns StatusOr<Coin>
        auto coin_result = chain_db_->getCoinWithConfidentialFallback(outpoint.txid.AsUint256(), outpoint.vout);
        if (coin_result.status() != Status::Ok) {
            return coin_result.status();
        }

        const Coin& coin = coin_result.value();

        // Convert Coin to UTXOEntry
        consensus::UTXOEntry entry;
        entry.value = AmountUna::Una(coin.amount);
        // FIX: Decode hex string to binary bytes (Coin stores scriptPubKey as hex)
        entry.scriptPubKey.clear();
        for (size_t i = 0; i + 1 < coin.script_pubkey.size(); i += 2) {
            uint8_t byte = static_cast<uint8_t>(std::stoi(coin.script_pubkey.substr(i, 2), nullptr, 16));
            entry.scriptPubKey.push_back(byte);
        }
        entry.height = static_cast<uint32_t>(coin.height);
        entry.isCoinbase = coin.coinbase;
        entry.is_confidential = coin.is_confidential;
        entry.commitment = coin.commitment;

        return entry;
    }

    bool hasCoin(const OutPoint& outpoint) const override {
        if (!chain_db_) return false;
        auto coin_result = chain_db_->getCoinWithConfidentialFallback(outpoint.txid.AsUint256(), outpoint.vout);
        return coin_result.status() == Status::Ok;
    }

    uint32_t getHeight() const override {
        if (!chain_db_) return 0;
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() != Status::Ok) {
            return 0;
        }
        return static_cast<uint32_t>(tip_result.value().height);
    }
};

// The in-memory ConsensusUTXOSet is the active chainstate authority used by
// BlockValidator. AssumeUTXO imports live there but intentionally are not
// copied into ChainDB, so mempool admission must query the same view or a
// wallet can prove ownership of a snapshot coin that the mempool then calls
// "not found".
class ConsensusUTXOStateView : public consensus::ChainStateView {
    const consensus::IConsensusUTXOSet* utxo_set_;
    std::unique_ptr<ChainDBStateView> chain_db_fallback_;
public:
    explicit ConsensusUTXOStateView(const consensus::IConsensusUTXOSet* utxo_set,
                                    ChainDB* chain_db_fallback = nullptr)
        : utxo_set_(utxo_set)
        , chain_db_fallback_(chain_db_fallback
              ? std::make_unique<ChainDBStateView>(chain_db_fallback)
              : nullptr) {}

    StatusOr<consensus::UTXOEntry> getCoin(const OutPoint& outpoint) const override {
        if (utxo_set_) {
            if (const auto* coin = utxo_set_->GetCoin(outpoint)) {
                return *coin;
            }
        }
        return chain_db_fallback_
            ? chain_db_fallback_->getCoin(outpoint)
            : StatusOr<consensus::UTXOEntry>(Status::NotFound);
    }

    bool hasCoin(const OutPoint& outpoint) const override {
        return (utxo_set_ && utxo_set_->HaveCoin(outpoint)) ||
               (chain_db_fallback_ && chain_db_fallback_->hasCoin(outpoint));
    }

    uint32_t getHeight() const override {
        return utxo_set_ ? utxo_set_->GetHeight() : 0;
    }
};

// v0.11.0: CoinsViewMemPool-backed UTXO view for policy validation
// Wraps CoinsViewMemPool to provide UTXOView interface for mempool validation
// This allows validation to see unconfirmed outputs created by mempool transactions
// Phase M.0: Updated to use uint256 txid
class MempoolUTXOView : public UTXOView {
    const CoinsViewMemPool* coins_view_;  // Non-owning pointer
    ChainDB* chain_db_;  // For height queries
    const std::unordered_map<uint256, MempoolEntry>* mempool_entries_;  // For conflicted mempool prevouts
public:
    explicit MempoolUTXOView(
        const CoinsViewMemPool* coins_view,
        ChainDB* chain_db,
        const std::unordered_map<uint256, MempoolEntry>* mempool_entries)
        : coins_view_(coins_view), chain_db_(chain_db), mempool_entries_(mempool_entries) {}

    bool GetMempoolPrevout(const uint256& txid, uint32_t vout, consensus::UTXOEntry& out) const {
        if (!mempool_entries_) {
            return false;
        }

        auto it = mempool_entries_->find(txid);
        if (it == mempool_entries_->end() || vout >= it->second.tx.vout.size()) {
            return false;
        }

        const auto& txout = it->second.tx.vout[vout];
        out.value = txout.value;
        out.scriptPubKey = txout.scriptPubKey;
        out.height = it->second.height;
        out.isCoinbase = false;
        out.is_confidential = txout.is_confidential;
        out.commitment = txout.commitment;
        return true;
    }

    bool HaveUTXO(const uint256& txid, uint32_t vout) const override {
        if (!coins_view_) return false;
        // Phase M.4.3-B: OutPoint uses TxId, wrap uint256 explicitly
        OutPoint out{TxId(txid), vout};
        auto result = coins_view_->getCoin(out);
        if (result.status() == Status::Ok) {
            return true;
        }
        consensus::UTXOEntry mempool_prevout;
        return GetMempoolPrevout(txid, vout, mempool_prevout);
    }

    bool GetUTXO(const uint256& txid, uint32_t vout, uint64_t& value, std::string& script) const override {
        if (!coins_view_) return false;
        // Phase M.4.3-B: OutPoint uses TxId, wrap uint256 explicitly
        OutPoint out{TxId(txid), vout};
        auto result = coins_view_->getCoin(out);
        if (result.status() == Status::Ok) {
            const consensus::UTXOEntry& utxo = result.value();
            value = utxo.value.GetUna();
            // Convert vector<uint8_t> to string
            script.assign(utxo.scriptPubKey.begin(), utxo.scriptPubKey.end());
            return true;
        }

        consensus::UTXOEntry mempool_prevout;
        if (!GetMempoolPrevout(txid, vout, mempool_prevout)) {
            return false;
        }
        value = mempool_prevout.value.GetUna();
        script.assign(mempool_prevout.scriptPubKey.begin(), mempool_prevout.scriptPubKey.end());
        return true;
    }

    bool HaveTransaction(const uint256& txid) const override {
        if (!chain_db_) return false;
        auto tx_location = chain_db_->getTxLocation(txid);
        return tx_location.status() == Status::Ok;
    }

    uint32_t GetHeight() const override {
        if (!chain_db_) return 0;
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() != Status::Ok) {
            return 0;
        }
        return static_cast<uint32_t>(tip_result.value().height);
    }

    bool GetConfidentialUTXO(const uint256& txid, uint32_t vout,
                            std::vector<uint8_t>& commitment, bool& is_confidential) const override {
        if (!coins_view_) return false;
        OutPoint out{TxId(txid), vout};
        auto result = coins_view_->getCoin(out);
        if (result.status() == Status::Ok) {
            const consensus::UTXOEntry& utxo = result.value();
            is_confidential = utxo.is_confidential;
            commitment = utxo.commitment;
            return true;
        }

        consensus::UTXOEntry mempool_prevout;
        if (!GetMempoolPrevout(txid, vout, mempool_prevout)) {
            return false;
        }
        is_confidential = mempool_prevout.is_confidential;
        commitment = mempool_prevout.commitment;
        return true;
    }

    bool HaveCommitment(const std::vector<uint8_t>& commitment) const override {
        if (commitment.empty()) {
            return false;
        }

        if (coins_view_ && coins_view_->hasCommitment(commitment)) {
            return true;
        }

        if (!chain_db_) {
            return false;
        }

        bool found = false;
        chain_db_->forEachUTXO([&](const uint256&, uint32_t, const Coin& coin) {
            if (coin.is_confidential && coin.commitment == commitment) {
                found = true;
                return false;
            }
            return true;
        });
        return found;
    }
};

namespace {

size_t GetEffectiveVirtualSize(const MempoolEntry& entry) {
    return entry.effective_vsize > 0 ? entry.effective_vsize : entry.tx_size;
}

double GetPackageSelectionScore(const MempoolEntry& entry) {
    if (entry.ancestor_adjusted_feerate > 0.0) {
        return entry.ancestor_adjusted_feerate;
    }
    if (entry.adjusted_fee_rate > 0.0) {
        return entry.adjusted_fee_rate;
    }
    return entry.fee_rate;
}

void PopulatePrivacyLaneMetrics(MempoolEntry& entry, const mining::CTSelectionConfig& config) {
    entry.is_confidential = false;
    entry.total_proof_bytes = 0;
    entry.effective_vsize = std::max<size_t>(entry.tx.GetVirtualSize(), 1);
    entry.adjusted_fee_rate = entry.fee_rate;
    entry.ancestor_effective_vsize = entry.effective_vsize;
    entry.ancestor_adjusted_feerate = entry.adjusted_fee_rate;

    for (const auto& output : entry.tx.vout) {
        if (output.is_confidential) {
            entry.is_confidential = true;
            break;
        }
    }

    if (entry.tx.HasExplicitFee()) {
        entry.fee = entry.tx.GetExplicitFee();
        entry.fee_rate = entry.tx_size > 0 ? static_cast<double>(entry.fee) / entry.tx_size : 0.0;
    }

    if (entry.is_confidential) {
        mining::CTSelectionPolicy ct_policy(config);
        const auto weight_info = ct_policy.GetWeightInfo(entry.tx);
        entry.total_proof_bytes = weight_info.proof_bytes;
        entry.effective_vsize = static_cast<size_t>((weight_info.total_weight + 3) / 4);
    }

    const uint64_t effective_fee = entry.tx.HasExplicitFee() ? entry.tx.GetExplicitFee() : entry.fee;
    entry.adjusted_fee_rate = entry.effective_vsize > 0
        ? static_cast<double>(effective_fee) / entry.effective_vsize
        : 0.0;
    entry.ancestor_effective_vsize = entry.effective_vsize;
    entry.ancestor_adjusted_feerate = entry.adjusted_fee_rate;
}

void EraseFeeIndexEntry(std::multimap<double, uint256>& fee_index, const uint256& txid, double score) {
    auto range = fee_index.equal_range(score);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == txid) {
            fee_index.erase(it);
            return;
        }
    }

    for (auto it = fee_index.begin(); it != fee_index.end(); ++it) {
        if (it->second == txid) {
            fee_index.erase(it);
            return;
        }
    }
}

}  // namespace

Mempool::Mempool(ChainDB* chain_db,
                 const consensus::IConsensusUTXOSet* consensus_utxo_set,
                 bool allow_chain_db_fallback)
    : chain_db_(chain_db)
    , chain_state_view_(consensus_utxo_set
          ? std::unique_ptr<consensus::ChainStateView>(
                std::make_unique<ConsensusUTXOStateView>(
                    consensus_utxo_set,
                    allow_chain_db_fallback ? chain_db : nullptr))
          : std::unique_ptr<consensus::ChainStateView>(
                std::make_unique<ChainDBStateView>(chain_db)))
    , coins_view_(chain_state_view_.get())  // v0.11.0: Initialize UTXO overlay with ChainStateView adapter
    , m_logger(nullptr)
    , m_max_size(DEFAULT_MAX_SIZE)
    , m_max_age(DEFAULT_MAX_AGE)
    , m_min_fee_rate(DEFAULT_MIN_FEE_RATE)
    , m_sp_scanner_manager(std::make_unique<din::sp::ScannerManager>()) {

    // Initialize BIP125 RBF policy with default configuration
    // RBF default: OFF - preserves payment finality (opt-in via setRBFEnabled)
    policy::RBFPolicy::Config rbf_config;
    rbf_config.enable_rbf = false;  // OFF by default for user trust
    rbf_config.min_relay_fee_rate = 1000;       // 1000 sat/KB
    rbf_config.incremental_relay_fee = 1000;    // 1000 sat/KB for replacements
    rbf_config.max_replacement_count = 100;     // BIP125 rule 5
    m_rbf_policy = std::make_unique<policy::RBFPolicy>(rbf_config);

    // Initialize fee estimator (v0.13.0.3)
    fee_estimator_ = std::make_unique<FeeEstimator>();

    MPLOG_INFO("Mempool initialized with max size: " + std::to_string(m_max_size / (1024*1024)) + "MB, RBF disabled (opt-in), CPFP enabled");
}

void Mempool::setRBFEnabled(bool enabled) {
    if (m_rbf_policy) {
        m_rbf_policy->setEnabled(enabled);
        MPLOG_INFO("RBF " + std::string(enabled ? "enabled" : "disabled"));
    }
}

bool Mempool::isRBFEnabled() const {
    return m_rbf_policy ? m_rbf_policy->isEnabled() : false;
}

Mempool::RBFRuntimeConfig Mempool::getRBFRuntimeConfig() const {
    RBFRuntimeConfig runtime;
    if (!m_rbf_policy) {
        return runtime;
    }

    const auto& config = m_rbf_policy->getConfig();
    runtime.enabled = config.enable_rbf;
    runtime.min_relay_fee_rate = config.min_relay_fee_rate;
    runtime.incremental_relay_fee = config.incremental_relay_fee;
    runtime.max_replacement_count = config.max_replacement_count;
    return runtime;
}

Mempool::~Mempool() {
    clear();
}

// ============================================================================
// Step 3: Structured Transaction Ingress Implementation (Phase G.3)
// ============================================================================

TxAcceptResult Mempool::submitTransaction(const Transaction& tx, const std::string& source, bool relay) {
    // Public API - delegates to internal implementation
    return submitTransactionInternal(tx, source, relay);
}

bool Mempool::addTransaction(const Transaction& tx, bool relay) {
    // Legacy adapter - delegates to internal and returns bool only
    // WARNING: This discards rejection reason. Use submitTransaction() for new code.
    auto result = submitTransactionInternal(tx, "legacy", relay);
    return result.accepted();
}

void Mempool::addUnchecked(const Transaction& tx) {
    if (tx.HasConfidentialOutputs()) {
        throw std::logic_error(
            "addUnchecked() rejects confidential transactions; use canonical ingress");
    }
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    uint256 txid_u256 = tx.GetTxid().AsUint256();
    MempoolEntry entry(tx, /*fee=*/0, /*block_height=*/0);
    for (const auto& input : tx.vin) {
        OutPoint outpoint{input.prevout.txid, input.prevout.vout};
        m_spent_outputs.insert(outpoint);
        entry.spends.push_back(outpoint);
    }
    m_transactions[txid_u256] = entry;
    rebuildCoinsViewLocked();
}

TxAcceptResult Mempool::submitTransactionInternal(
    const Transaction& tx,
    const std::string& source,
    bool relay,
    bool test_only) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    // Phase M.0: GetTxid() returns TxId, use directly
    TxId txid = tx.GetTxid();
    uint256 txid_u256 = txid.AsUint256();
    MPLOG_DEBUG("[" + source + "] Attempting to add transaction to mempool: " + txid_u256.GetHex());

    // Check if transaction already exists
    if (m_transactions.find(txid_u256) != m_transactions.end()) {
        MPLOG_DEBUG("Transaction already in mempool: " + txid_u256.GetHex());
        return TxAcceptResult::Rejected(TxRejectCode::ALREADY_IN_MEMPOOL,
            "Transaction already in mempool", txid_u256);
    }

    // Validate transaction
    std::string error;
    if (!validateTransaction(tx, error)) {
        MPLOG_WARN("Transaction validation failed for " + txid_u256.GetHex() + ": " + error);
        if (!test_only) {
            m_total_tx_rejected.fetch_add(1);
        }
        return TxAcceptResult::Rejected(TxRejectCode::INVALID_TX,
            "Transaction validation failed: " + error, txid_u256);
    }

    // Check for conflicts (double spending)
    // Phase M.0: Use OutPoint struct instead of string concatenation
    // Build list of conflicting transactions
    std::vector<uint256> conflicting_txids;
    for (const auto& input : tx.vin) {
        // Phase M.0: Create OutPoint struct from input
        OutPoint outpoint{input.prevout.txid, input.prevout.vout};
        if (m_spent_outputs.find(outpoint) != m_spent_outputs.end()) {
            // Find which transaction spends this outpoint
            for (const auto& [mempool_txid, entry] : m_transactions) {
                for (const auto& spent : entry.spends) {
                    if (spent == outpoint) {
                        conflicting_txids.push_back(mempool_txid);
                        break;
                    }
                }
            }
        }
    }

    // If conflicts exist, check for RBF replacement
    if (!conflicting_txids.empty()) {
        MPLOG_DEBUG("Transaction " + txid_u256.GetHex() + " conflicts with " +
                     std::to_string(conflicting_txids.size()) + " mempool transaction(s)");

        // Check if RBF is signaled in the replacement transaction
        if (!m_rbf_policy || !m_rbf_policy->isRBFSignaled(tx)) {
            MPLOG_WARN("Double spend rejected (RBF not signaled): " + txid_u256.GetHex());
            if (!test_only) {
                m_total_tx_rejected.fetch_add(1);
            }
            return TxAcceptResult::Rejected(TxRejectCode::DOUBLE_SPEND_NO_RBF,
                "Transaction conflicts with mempool transaction(s) and RBF not signaled", txid_u256);
        }

        // Build mempool entries list for RBF validation
        std::vector<MempoolEntry> mempool_entries;
        for (const auto& [mempool_txid, entry] : m_transactions) {
            mempool_entries.push_back(entry);
        }

        // Build conflict set
        policy::RBFConflictSet conflict_set =
            policy::RBFPolicy::buildConflictSet(tx, mempool_entries, ct_config_);

        std::vector<MempoolEntry> original_entries;
        original_entries.reserve(conflict_set.direct_conflicts.size());
        for (const auto& conflict_txid : conflict_set.direct_conflicts) {
            auto entry_it = m_transactions.find(conflict_txid);
            if (entry_it != m_transactions.end()) {
                original_entries.push_back(entry_it->second);
            }
        }

        // Build set of all mempool txids for rule #2 validation (Phase M.0: uint256)
        std::unordered_set<uint256> mempool_txids_set;
        for (const auto& [mempool_txid, _] : m_transactions) {
            mempool_txids_set.insert(mempool_txid);
        }

        // Calculate replacement fee
        uint64_t replacement_fee = calculateFee(tx);

        // Validate RBF replacement against all BIP125 rules
        std::string rbf_error;
        policy::RBFValidationResult rbf_result = m_rbf_policy->validateReplacement(
            tx, replacement_fee, conflict_set, ct_config_, mempool_txids_set, original_entries, rbf_error);

        if (rbf_result != policy::RBFValidationResult::VALID) {
            MPLOG_WARN("RBF replacement rejected for " + txid_u256.GetHex() + ": " + rbf_error);
            if (!test_only) {
                m_total_tx_rejected.fetch_add(1);
            }
            return TxAcceptResult::Rejected(TxRejectCode::RBF_REJECTED,
                "RBF replacement rejected: " + rbf_error, txid_u256);
        }

        // RBF validation passed - remove all conflicting transactions
        MPLOG_INFO(std::string(test_only ? "RBF replacement test accepted: "
                                        : "RBF replacement accepted: ") +
                   txid_u256.GetHex() + " replaces " +
                   std::to_string(conflict_set.conflict_count) + " transactions");

        // Create a copy of direct and descendant conflicts for removal (Phase M.0: uint256)
        std::vector<uint256> txids_to_remove;
        txids_to_remove.insert(txids_to_remove.end(),
                              conflict_set.direct_conflicts.begin(),
                              conflict_set.direct_conflicts.end());
        txids_to_remove.insert(txids_to_remove.end(),
                              conflict_set.descendant_conflicts.begin(),
                              conflict_set.descendant_conflicts.end());

        // A dry-run must never evict or otherwise mutate existing entries.
        if (!test_only) {
            for (const auto& conflict_txid : txids_to_remove) {
                removeTransactionLocked(conflict_txid);
                MPLOG_DEBUG("Removed conflicting transaction: " + conflict_txid.GetHex());
            }
            rebuildCoinsViewLocked();
        }
    }

    // Calculate fee
    uint64_t fee = calculateFee(tx);
    size_t tx_size = tx.GetSize();             // raw bytes — used for ancestor/descendant size limits
    size_t tx_vsize = tx.GetVirtualSize();     // BIP141 vsize — used for fee rate
    double fee_rate = tx_vsize > 0 ? static_cast<double>(fee) / tx_vsize : 0.0;

    // Check minimum fee rate
    if (fee_rate < m_min_fee_rate) {
        MPLOG_WARN("Transaction fee rate too low: " + std::to_string(fee_rate) +
                        " (min: " + std::to_string(m_min_fee_rate) + ") for " + txid_u256.GetHex());
        if (!test_only) {
            m_total_tx_rejected.fetch_add(1);
        }
        return TxAcceptResult::Rejected(TxRejectCode::INSUFFICIENT_FEE,
            "Transaction fee rate " + std::to_string(fee_rate) + " sat/byte below minimum " +
            std::to_string(m_min_fee_rate) + " sat/byte", txid_u256);
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // Ancestor/Descendant Limit Enforcement (DoS Protection)
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // Bitcoin Core default limits:
    // - Max 25 ancestors
    // - Max 25 descendants
    // - Max 101KB total ancestor size
    // - Max 101KB total descendant size
    //
    // These limits prevent mempool DoS attacks via deep transaction chains.

    // Calculate ancestors (unconfirmed parents)
    uint32_t ancestor_count = 0;
    uint64_t ancestor_size = tx_size;  // Start with this transaction's size
    std::unordered_set<uint256> visited_ancestors;
    std::vector<uint256> to_visit_ancestors;

    // Find all ancestors
    for (const auto& input : tx.vin) {
        TxId parent_txid = input.prevout.txid;
        if (m_transactions.find(parent_txid.AsUint256()) != m_transactions.end()) {
            to_visit_ancestors.push_back(parent_txid.AsUint256());
        }
    }

    // BFS to find all ancestors
    while (!to_visit_ancestors.empty()) {
        uint256 current = to_visit_ancestors.back();
        to_visit_ancestors.pop_back();

        if (visited_ancestors.count(current)) continue;
        visited_ancestors.insert(current);

        auto ancestor_it = m_transactions.find(current);
        if (ancestor_it == m_transactions.end()) continue;

        ancestor_count++;
        ancestor_size += ancestor_it->second.tx_size;

        // Add this ancestor's parents
        for (const auto& input : ancestor_it->second.tx.vin) {
            TxId grandparent_txid = input.prevout.txid;
            if (m_transactions.find(grandparent_txid.AsUint256()) != m_transactions.end() &&
                !visited_ancestors.count(grandparent_txid.AsUint256())) {
                to_visit_ancestors.push_back(grandparent_txid.AsUint256());
            }
        }
    }

    // Check ancestor limits
    constexpr uint32_t MAX_ANCESTORS = 25;
    constexpr uint64_t MAX_ANCESTOR_SIZE = 101 * 1024;  // 101KB

    if (ancestor_count > MAX_ANCESTORS) {
        MPLOG_WARN("Transaction " + txid_u256.GetHex() + " rejected: too many ancestors (" +
                    std::to_string(ancestor_count) + " > " + std::to_string(MAX_ANCESTORS) + ")");
        if (!test_only) {
            m_total_tx_rejected.fetch_add(1);
        }
        return TxAcceptResult::Rejected(TxRejectCode::TOO_MANY_ANCESTORS,
            "Transaction has " + std::to_string(ancestor_count) + " ancestors (limit: " +
            std::to_string(MAX_ANCESTORS) + ")", txid_u256);
    }

    if (ancestor_size > MAX_ANCESTOR_SIZE) {
        MPLOG_WARN("Transaction " + txid_u256.GetHex() + " rejected: ancestor size too large (" +
                    std::to_string(ancestor_size) + " > " + std::to_string(MAX_ANCESTOR_SIZE) + " bytes)");
        if (!test_only) {
            m_total_tx_rejected.fetch_add(1);
        }
        return TxAcceptResult::Rejected(TxRejectCode::ANCESTOR_SIZE_EXCEEDED,
            "Ancestor size " + std::to_string(ancestor_size) + " bytes exceeds limit " +
            std::to_string(MAX_ANCESTOR_SIZE) + " bytes", txid_u256);
    }

    // Calculate descendants (transactions that spend from unconfirmed txs)
    // Note: We check if adding this transaction would cause any existing transaction
    // to exceed descendant limits
    for (const auto& input : tx.vin) {
        TxId parent_txid = input.prevout.txid;

        auto parent_it = m_transactions.find(parent_txid.AsUint256());
        if (parent_it == m_transactions.end()) continue;  // Parent not in mempool

        // Count descendants of this parent (including the new transaction)
        uint32_t descendant_count = 1;  // Count the new transaction
        uint64_t descendant_size = tx_size;
        std::unordered_set<uint256> visited_descendants;
        std::vector<uint256> to_visit_descendants;

        // Find all existing descendants of parent
        for (const auto& [mempool_txid, entry] : m_transactions) {
            if (mempool_txid == parent_txid.AsUint256()) continue;  // Skip the parent itself

            // Check if this transaction spends from the parent
            for (const auto& tx_input : entry.tx.vin) {
                TxId input_parent_txid = tx_input.prevout.txid;
                if (input_parent_txid.AsUint256() == parent_txid.AsUint256()) {
                    to_visit_descendants.push_back(mempool_txid);
                    break;
                }
            }
        }

        // BFS to find all descendants
        while (!to_visit_descendants.empty()) {
            uint256 current = to_visit_descendants.back();
            to_visit_descendants.pop_back();

            if (visited_descendants.count(current)) continue;
            visited_descendants.insert(current);

            auto descendant_it = m_transactions.find(current);
            if (descendant_it == m_transactions.end()) continue;

            descendant_count++;
            descendant_size += descendant_it->second.tx_size;

            // Add this descendant's children
            for (const auto& [mempool_txid, entry] : m_transactions) {
                if (visited_descendants.count(mempool_txid)) continue;

                for (const auto& tx_input : entry.tx.vin) {
                    TxId input_parent_txid = tx_input.prevout.txid;
                    if (input_parent_txid.AsUint256() == current) {
                        to_visit_descendants.push_back(mempool_txid);
                        break;
                    }
                }
            }
        }

        // Check descendant limits for this parent
        constexpr uint32_t MAX_DESCENDANTS = 25;
        constexpr uint64_t MAX_DESCENDANT_SIZE = 101 * 1024;  // 101KB

        if (descendant_count > MAX_DESCENDANTS) {
            MPLOG_WARN("Transaction " + txid_u256.GetHex() + " rejected: would cause parent " +
                        parent_txid.AsUint256().GetHex() + " to exceed descendant limit (" +
                        std::to_string(descendant_count) + " > " + std::to_string(MAX_DESCENDANTS) + ")");
            if (!test_only) {
                m_total_tx_rejected.fetch_add(1);
            }
            return TxAcceptResult::Rejected(TxRejectCode::TOO_MANY_DESCENDANTS,
                "Would cause parent " + parent_txid.AsUint256().GetHex().substr(0, 16) + "... to have " +
                std::to_string(descendant_count) + " descendants (limit: " + std::to_string(MAX_DESCENDANTS) + ")",
                txid_u256);
        }

        if (descendant_size > MAX_DESCENDANT_SIZE) {
            MPLOG_WARN("Transaction " + txid_u256.GetHex() + " rejected: would cause parent " +
                        parent_txid.AsUint256().GetHex() + " to exceed descendant size limit (" +
                        std::to_string(descendant_size) + " > " + std::to_string(MAX_DESCENDANT_SIZE) + " bytes)");
            if (!test_only) {
                m_total_tx_rejected.fetch_add(1);
            }
            return TxAcceptResult::Rejected(TxRejectCode::DESCENDANT_SIZE_EXCEEDED,
                "Would cause parent descendant size " + std::to_string(descendant_size) +
                " bytes (limit: " + std::to_string(MAX_DESCENDANT_SIZE) + " bytes)", txid_u256);
        }
    }

    if (ancestor_count > 0) {
        MPLOG_DEBUG("Transaction " + txid_u256.GetHex() + " has " + std::to_string(ancestor_count) +
                     " ancestors (" + std::to_string(ancestor_size) + " bytes)");
    }

    // All canonical validation and policy checks have passed. A
    // testmempoolaccept request ends here so it cannot alter conflicts,
    // dependency indexes, the UTXO overlay, counters, or relay state.
    if (test_only) {
        return TxAcceptResult::Accepted(txid_u256);
    }

    // Get current blockchain height from ChainDB
    uint32_t current_height = 0;
    if (chain_db_) {
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() == Status::Ok) {
            current_height = static_cast<uint32_t>(tip_result.value().height);
        }
    }
    
    // Create mempool entry
    MempoolEntry entry(tx, fee, current_height);
    PopulatePrivacyLaneMetrics(entry, ct_config_);

    // Phase 8 Commit 2: compute VWU for ordering/eviction/RBF. Done after
    // PopulatePrivacyLaneMetrics so the CT vsize is already settled
    // (though VWU doesn't consume it) and before ancestor rollup so we
    // can include ancestors' VWU in entry.ancestor_vwu.
    entry.vwu = computeVWUForTx(entry.tx);
    if (entry.vwu == 0) {
        // Defensive fallback: ordering denominators can never be zero.
        entry.vwu = std::max<uint64_t>(entry.tx_size, 1);
    }
    // Refresh the per-tx adjusted fee rate onto VWU so eviction + selection
    // of this tx alone (without ancestors) use the same economic metric.
    entry.adjusted_fee_rate =
        static_cast<double>(entry.fee) / static_cast<double>(entry.vwu);

    std::unordered_set<uint256> direct_parents;
    for (const auto& input : tx.vin) {
        TxId parent_txid = input.prevout.txid;
        if (m_transactions.find(parent_txid.AsUint256()) != m_transactions.end()) {
            direct_parents.insert(parent_txid.AsUint256());
        }
    }
    entry.depends.assign(direct_parents.begin(), direct_parents.end());

    for (const auto& parent_txid : entry.depends) {
        m_children_index[parent_txid].insert(txid_u256);
    }

    entry.ancestor_fee = entry.fee;
    entry.ancestor_size = entry.tx_size;
    entry.ancestor_effective_vsize = GetEffectiveVirtualSize(entry);
    entry.ancestor_vwu = entry.vwu;
    for (const auto& ancestor_txid : visited_ancestors) {
        auto ancestor_it = m_transactions.find(ancestor_txid);
        if (ancestor_it == m_transactions.end()) {
            continue;
        }
        entry.ancestor_fee += ancestor_it->second.fee;
        entry.ancestor_size += ancestor_it->second.tx_size;
        entry.ancestor_effective_vsize += GetEffectiveVirtualSize(ancestor_it->second);
        entry.ancestor_vwu += ancestor_it->second.vwu;
    }
    entry.ancestor_feerate = entry.ancestor_size > 0
        ? static_cast<double>(entry.ancestor_fee) / entry.ancestor_size
        : 0.0;
    // Phase 8 Commit 2: ancestor-adjusted feerate now uses VWU (economic
    // truth) rather than BIP141-based effective_vsize (bandwidth-biased
    // proxy). Consumers: Mempool::selectTransactionsForBlock sort;
    // BlockAssembler inherits via the mempool; eviction via
    // GetPackageSelectionScore → adjusted_fee_rate.
    entry.ancestor_adjusted_feerate = entry.ancestor_vwu > 0
        ? static_cast<double>(entry.ancestor_fee) / static_cast<double>(entry.ancestor_vwu)
        : 0.0;

    // Update spent outputs tracking
    for (const auto& input : tx.vin) {
        OutPoint outpoint{input.prevout.txid, input.prevout.vout};
        m_spent_outputs.insert(outpoint);
        entry.spends.push_back(outpoint);
    }

    // Add to main storage
    m_transactions[txid_u256] = entry;

    // v0.11.0: Update mempool UTXO overlay
    // Spend inputs (mark as consumed by mempool)
    for (const auto& input : tx.vin) {
        // Phase M.0: Wallet boundary - convert TxOutPoint (string) to OutPoint (uint256)
        OutPoint out{input.prevout.txid, input.prevout.vout};
        coins_view_.spendCoin(out);
    }

    // Add outputs (created by mempool transaction)
    for (size_t vout = 0; vout < tx.vout.size(); ++vout) {
        OutPoint out{TxId(txid_u256), static_cast<uint32_t>(vout)};
        Coin coin;
        coin.amount = tx.vout[vout].value.GetUna();  // AmountUna → uint64_t
        coin.script_pubkey = std::string(tx.vout[vout].scriptPubKey.begin(),
                                         tx.vout[vout].scriptPubKey.end());
        coin.height = static_cast<int>(current_height);
        coin.coinbase = false;  // Mempool txs are never coinbase

        // Convert Coin to UTXOEntry for CoinsViewMemPool
        consensus::UTXOEntry utxo_entry;
        utxo_entry.value = AmountUna::Una(coin.amount);  // uint64_t → AmountUna
        utxo_entry.scriptPubKey.assign(coin.script_pubkey.begin(), coin.script_pubkey.end());
        utxo_entry.height = static_cast<uint32_t>(coin.height);
        utxo_entry.isCoinbase = coin.coinbase;
        utxo_entry.is_confidential = tx.vout[vout].is_confidential;
        utxo_entry.commitment = tx.vout[vout].commitment;
        coins_view_.addCoin(out, utxo_entry);
    }

    // Add to indices
    m_fee_index.insert({GetPackageSelectionScore(entry), txid_u256});
    m_time_index.insert({entry.time, txid_u256});

    MPLOG_INFO("[" + source + "] Added transaction to mempool: " + txid_u256.GetHex() +
                 " (fee: " + std::to_string(fee) + " sats, " +
                 "rate: " + std::to_string(fee_rate) + " sat/byte)");

    m_total_tx_added.fetch_add(1);

    // v0.13.0.3: Record transaction entry for fee estimation
    if (fee_estimator_ && chain_db_) {
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() == Status::Ok) {
            uint32_t tip_height = tip_result.value().height;
            // Record fee rate as una/vbyte (BIP141-aligned).
            double vbyte_fee_rate = tx_vsize > 0 ? static_cast<double>(fee) / tx_vsize : 0.0;
            fee_estimator_->recordTxEntry(txid_u256, vbyte_fee_rate, tip_height);
        }
    }

    // Scan for Silent Payments
    if (m_sp_scanner_manager) {
        std::string tx_hex = tx.SerializeHex();
        m_sp_scanner_manager->scanMempoolTransaction(tx_hex);
    }

    // Notify wallet watchers of mempool acceptance (NodeCore → Swift events).
    if (m_tx_accepted_callback) {
        m_tx_accepted_callback(tx);
    }

    // Broadcast to network if requested through the canonical callback path.
    if (relay && m_tx_broadcast_callback) {
        lock.unlock();  // Release lock before network call
        broadcastTransaction(txid_u256);
    }

    // Check if mempool needs cleanup
    if (getTotalSizeLocked() > m_max_size) {
        evictTransactionsLocked();
    }

    return TxAcceptResult::Accepted(txid_u256);
}

bool Mempool::removeTransaction(const uint256& txid) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    auto it = m_transactions.find(txid);
    if (it == m_transactions.end()) {
        return false;
    }

    const auto& entry = it->second;

    // Remove from spent outputs tracking
    for (const auto& outpoint : entry.spends) {
        m_spent_outputs.erase(outpoint);
    }

    // Remove from indices
    EraseFeeIndexEntry(m_fee_index, txid, GetPackageSelectionScore(entry));

    auto time_range = m_time_index.equal_range(entry.time);
    for (auto time_it = time_range.first; time_it != time_range.second; ++time_it) {
        if (time_it->second == txid) {
            m_time_index.erase(time_it);
            break;
        }
    }

    // Update children's dependency lists and ancestor metrics
    updateDependencies(txid);

    // Clean up this tx's forward deps from reverse index
    for (const auto& parent_txid : entry.depends) {
        auto parent_children = m_children_index.find(parent_txid);
        if (parent_children != m_children_index.end()) {
            parent_children->second.erase(txid);
            if (parent_children->second.empty()) {
                m_children_index.erase(parent_children);
            }
        }
    }

    // Remove from main storage
    m_template_exclusions.erase(txid);
    m_transactions.erase(it);
    rebuildCoinsViewLocked();

    MPLOG_DEBUG("Removed transaction from mempool: " + txid.GetHex());
    m_total_tx_removed.fetch_add(1);

    return true;
}

bool Mempool::hasTransaction(const uint256& txid) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_transactions.find(txid) != m_transactions.end();
}

TxAcceptResult Mempool::submitTransactionTestOnly(const Transaction& tx, const std::string& source) {
    return submitTransactionInternal(tx, source, false, true);

#if 0
    // STEP 2: TEST_ONLY transaction submission (bypasses signature validation)
    // This enables mempool policy testing without Phase 34 (signing)
    // Only available in regtest mode - never relayed to network

    std::unique_lock<std::shared_mutex> lock(m_mutex);

    // Phase M.0: Get txid early for structured returns
    TxId txid_type = tx.GetTxid();
    uint256 txid_u256 = txid_type.AsUint256();

    // STEP 3.5: Opportunistic expiry (hygiene before policy checks)
    // Remove transactions older than m_max_age (default: 2 weeks)
    // This runs before size checks and eviction to keep mempool clean
    {
        auto now = std::chrono::steady_clock::now();
        auto cutoff = now - m_max_age;

        std::vector<uint256> expired_txids;

        // m_time_index is sorted by time, so iterate until we hit cutoff
        for (auto it = m_time_index.begin();
             it != m_time_index.end() && it->first < cutoff;
             ++it) {
            expired_txids.push_back(it->second);
        }

        // Remove expired transactions
        for (const auto& expired_txid : expired_txids) {
            removeTransactionLocked(expired_txid);
        }

        if (!expired_txids.empty()) {
            MPLOG_INFO("TEST_ONLY: Removed " + std::to_string(expired_txids.size()) +
                      " expired transactions (older than " + std::to_string(m_max_age.count()) + " hours)");
        }
    }

    // Check if transaction already exists
    if (m_transactions.find(txid_u256) != m_transactions.end()) {
        return TxAcceptResult::Rejected(TxRejectCode::ALREADY_IN_MEMPOOL,
            "Transaction already in mempool", txid_u256);
    }

    // Basic structural validation (no signature checks)
    if (tx.vin.empty()) {
        return TxAcceptResult::Rejected(TxRejectCode::INVALID_TX,
            "Transaction has no inputs", txid_u256);
    }

    if (tx.vout.empty()) {
        return TxAcceptResult::Rejected(TxRejectCode::INVALID_TX,
            "Transaction has no outputs", txid_u256);
    }

    // Calculate fee (manual calculation for TEST_ONLY mode)
    // Mempool must act as UTXO overlay: check mempool first, then ChainDB
    uint64_t input_value = 0;
    for (const auto& input : tx.vin) {
        uint64_t coin_value = 0;
        bool found = false;

        // Phase M.0: Wallet boundary - TxId is the semantic type
        TxId input_txid = input.prevout.txid;

        // 1️⃣ Try mempool first (unconfirmed ancestor)
        auto it = m_transactions.find(input_txid.AsUint256());
        if (it != m_transactions.end()) {
            const MempoolEntry& parent = it->second;

            if (input.prevout.vout >= parent.tx.vout.size()) {
                return TxAcceptResult::Rejected(TxRejectCode::INVALID_TX,
                    "Input references invalid output index", txid_u256);
            }

            coin_value = parent.tx.vout[input.prevout.vout].value.GetUna();  // AmountUna → uint64_t
            found = true;
            OutPoint outpoint{input_txid, input.prevout.vout};
            MPLOG_DEBUG("TEST_ONLY: Input from mempool: " + outpoint.ToString() +
                       " value: " + std::to_string(coin_value));
        }

        const OutPoint outpoint{input_txid, input.prevout.vout};

        // 2️⃣ Fallback to the active confirmed UTXO view.
        if (!found) {
            auto coin_result = chain_state_view_->getCoin(outpoint);
            if (coin_result.status() == Status::Ok) {
                coin_value = coin_result.value().value.GetUna();
                found = true;
                MPLOG_DEBUG("TEST_ONLY: Input from active chainstate: " + outpoint.ToString() +
                           " value: " + std::to_string(coin_value));
            }
        }

        // Snapshot-bootstrapped nodes intentionally omit frozen pre-base
        // coins from the active view. Resolve them through the same
        // live-forest-gated path used by canonical validation.
        if (!found && prebase_coin_resolver_) {
            const bool is_frozen_prebase =
                !prebase_coin_predicate_ || prebase_coin_predicate_(outpoint);
            if (is_frozen_prebase) {
                const auto prebase = prebase_coin_resolver_(outpoint);
                if (prebase.has_value()) {
                    coin_value = prebase->value.GetUna();
                    found = true;
                    MPLOG_DEBUG("TEST_ONLY: Input from live pre-base store: " +
                                outpoint.ToString() + " value: " +
                                std::to_string(coin_value));
                }
            }
        }

        if (!found) {
            OutPoint outpoint{input.prevout.txid, input.prevout.vout};
            std::string err = "Cannot find input UTXO: " + outpoint.ToString();
            MPLOG_WARN("TEST_ONLY: " + err);
            return TxAcceptResult::Rejected(TxRejectCode::MISSING_INPUTS, err, txid_u256);
        }

        input_value += coin_value;
    }

    uint64_t fee = 0;

    // For confidential transactions, use the explicit fee field
    // (output values are 0 for CT outputs, so inputs-outputs is wrong)
    if (tx.HasConfidentialOutputs() && tx.HasExplicitFee()) {
        fee = tx.GetExplicitFee();
    } else {
        uint64_t output_value = 0;
        for (const auto& output : tx.vout) {
            output_value += output.value.GetUna();
        }

        if (input_value <= output_value) {
            return TxAcceptResult::Rejected(TxRejectCode::INVALID_TX,
                "Invalid transaction: inputs (" + std::to_string(input_value) +
                ") <= outputs (" + std::to_string(output_value) + ")", txid_u256);
        }

        fee = input_value - output_value;
    }

    size_t tx_size = tx.GetSize();
    size_t tx_vsize = tx.GetVirtualSize();
    double fee_rate = tx_vsize > 0 ? static_cast<double>(fee) / tx_vsize : 0.0;

    // Check minimum fee rate
    if (fee_rate < m_min_fee_rate) {
        return TxAcceptResult::Rejected(TxRejectCode::INSUFFICIENT_FEE,
            "Fee rate too low: " + std::to_string(fee_rate) +
            " (min: " + std::to_string(m_min_fee_rate) + ")", txid_u256);
    }

    // STEP 3.7: Check for conflicts and handle RBF (Replace-By-Fee)
    std::unordered_set<uint256> conflicting_txids;
    std::vector<const MempoolEntry*> conflicting_entries;

    for (const auto& input : tx.vin) {
        OutPoint outpoint{input.prevout.txid, input.prevout.vout};

        // Check if this input is already spent by a mempool transaction
        auto spent_it = m_spent_outputs.find(outpoint);
        if (spent_it != m_spent_outputs.end()) {
            // Find the conflicting transaction
            for (const auto& [mempool_txid, mempool_entry] : m_transactions) {
                for (const auto& spent_outpoint : mempool_entry.spends) {
                    if (spent_outpoint == outpoint) {
                        if (conflicting_txids.insert(mempool_txid).second) {
                            conflicting_entries.push_back(&mempool_entry);
                        }
                        break;
                    }
                }
            }
        }
    }

    // If conflicts exist, check if RBF is allowed
    if (!conflicting_entries.empty()) {
        // Phase 8 Commit 2: RBF feerate comparison moves to VWU. For
        // conflicting entries already in mempool we use their cached
        // entry.vwu; for the replacement candidate we call computeVWUForTx
        // so ML-DSA witness bytes don't get the BIP141 4× discount that
        // would let a cheap-looking P2MR replacement displace better txs.
        auto vwu_for = [&](const Transaction& candidate) -> uint64_t {
            uint64_t v = computeVWUForTx(candidate);
            return v > 0 ? v : static_cast<uint64_t>(std::max<size_t>(candidate.GetVirtualSize(), 1));
        };

        // BIP125 Rule 1: All original transactions must signal RBF (nSequence < 0xfffffffe)
        bool rbf_signaled = true;
        for (const auto* conflicting : conflicting_entries) {
            bool tx_signals_rbf = false;
            for (const auto& input : conflicting->tx.vin) {
                if (input.sequence < 0xfffffffe) {
                    tx_signals_rbf = true;
                    break;
                }
            }
            if (!tx_signals_rbf) {
                rbf_signaled = false;
                break;
            }
        }

        if (!rbf_signaled) {
            std::string err = "Double spend rejected (original transaction does not signal RBF)";
            MPLOG_WARN("TEST_ONLY: Rejected " + txid_u256.GetHex() + " - " + err);
            return TxAcceptResult::Rejected(TxRejectCode::DOUBLE_SPEND_NO_RBF, err, txid_u256);
        }

        // BIP125 Rule 2: Replacement must have higher absolute fee
        uint64_t original_fees = 0;
        uint64_t original_vwu_sum = 0;
        for (const auto* conflicting : conflicting_entries) {
            original_fees += conflicting->fee;
            original_vwu_sum += conflicting->vwu > 0
                                   ? conflicting->vwu
                                   : static_cast<uint64_t>(conflicting->tx_size);
        }

        if (fee <= original_fees) {
            std::string err = "RBF rejected (replacement fee " + std::to_string(fee) +
                       " not higher than original " + std::to_string(original_fees) + ")";
            MPLOG_WARN("TEST_ONLY: Rejected " + txid_u256.GetHex() + " - " + err);
            return TxAcceptResult::Rejected(TxRejectCode::RBF_REJECTED, err, txid_u256);
        }

        // BIP125 Rule 3: Replacement must have higher feerate — in VWU now.
        const uint64_t replacement_vwu = vwu_for(tx);
        double original_feerate = original_vwu_sum > 0
            ? static_cast<double>(original_fees) / static_cast<double>(original_vwu_sum)
            : 0.0;
        const double replacement_effective_feerate = replacement_vwu > 0
            ? static_cast<double>(fee) / static_cast<double>(replacement_vwu)
            : 0.0;

        if (replacement_effective_feerate <= original_feerate) {
            std::string err = "RBF rejected (replacement feerate " +
                       std::to_string(replacement_effective_feerate) +
                       " not higher than original " + std::to_string(original_feerate) + ")";
            MPLOG_WARN("TEST_ONLY: Rejected " + txid_u256.GetHex() + " - " + err);
            return TxAcceptResult::Rejected(TxRejectCode::RBF_REJECTED, err, txid_u256);
        }

        // BIP125 Rule 4: Replacement must pay for bandwidth
        // Incremental fee must cover relay cost of the evicted set plus
        // replacement — bandwidth floor scaled by VWU (same denominator
        // as the feerate comparison above, keeps the economics coherent).
        const uint64_t total_relay_vwu = original_vwu_sum + replacement_vwu;
        uint64_t min_fee = original_fees + total_relay_vwu;

        if (fee < min_fee) {
            std::string err = "RBF rejected (insufficient fee to cover bandwidth: " +
                       std::to_string(fee) + " < " + std::to_string(min_fee) + ")";
            MPLOG_WARN("TEST_ONLY: Rejected " + txid_u256.GetHex() + " - " + err);
            return TxAcceptResult::Rejected(TxRejectCode::RBF_REJECTED, err, txid_u256);
        }

        // BIP125 Rule 5: No more than 100 transactions replaced (anti-DoS)
        if (conflicting_txids.size() > 100) {
            std::string err = "RBF rejected (too many transactions replaced: " +
                       std::to_string(conflicting_txids.size()) + " > 100)";
            MPLOG_WARN("TEST_ONLY: Rejected " + txid_u256.GetHex() + " - " + err);
            return TxAcceptResult::Rejected(TxRejectCode::RBF_REJECTED, err, txid_u256);
        }

        // RBF accepted - remove conflicting transactions before adding replacement
        MPLOG_INFO("TEST_ONLY: RBF replacement accepted - removing " +
                  std::to_string(conflicting_txids.size()) + " conflicting transaction(s), " +
                  "fee increase: " + std::to_string(fee - original_fees) + " sats");

        for (const auto& conflicting_txid : conflicting_txids) {
            removeTransactionLocked(conflicting_txid);
        }
    }

    // Get current blockchain height from ChainDB
    uint32_t current_height = 0;
    if (chain_db_) {
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() == Status::Ok) {
            current_height = static_cast<uint32_t>(tip_result.value().height);
        }
    }

    // Create mempool entry
    MempoolEntry entry(tx, fee, current_height);
    PopulatePrivacyLaneMetrics(entry, ct_config_);

    // Track parent dependencies (for ancestor/descendant accounting)
    std::unordered_set<uint256> parents;


    // Original dependency tracking logic continues...
    for (const auto& input : tx.vin) {
        TxId parent_txid = input.prevout.txid;
        auto parent_it = m_transactions.find(parent_txid.AsUint256());
        if (parent_it != m_transactions.end()) {
            parents.insert(parent_txid.AsUint256());
        }
    }

    // Populate depends field
    entry.depends.assign(parents.begin(), parents.end());

    // Populate reverse dependency index (parent → children)
    for (const auto& parent_txid : entry.depends) {
        m_children_index[parent_txid].insert(txid_u256);
    }

    // STEP 3.2: Enforce ancestor limit (max 25 ancestors)
    // Count all ancestors recursively
    std::unordered_set<uint256> all_ancestors;
    std::vector<uint256> to_check(entry.depends.begin(), entry.depends.end());

    while (!to_check.empty()) {
        uint256 parent_txid = to_check.back();
        to_check.pop_back();

        if (all_ancestors.find(parent_txid) != all_ancestors.end()) {
            continue; // Already counted
        }

        all_ancestors.insert(parent_txid);

        // Add this parent's ancestors to the queue
        auto parent_it = m_transactions.find(parent_txid);
        if (parent_it != m_transactions.end()) {
            for (const auto& ancestor : parent_it->second.depends) {
                if (all_ancestors.find(ancestor) == all_ancestors.end()) {
                    to_check.push_back(ancestor);
                }
            }
        }
    }

    // Bitcoin Core default: max 25 in ancestor set (transaction + ancestors)
    // So max 24 ancestors + itself = 25 total
    const size_t MAX_ANCESTOR_COUNT = 24;  // Not including the transaction itself

    size_t ancestor_count = all_ancestors.size();

    // Check ancestor limits
    if (ancestor_count > MAX_ANCESTOR_COUNT) {
        std::string err = "too-long-mempool-chain (ancestor count: " + std::to_string(ancestor_count + 1) +
                    " including self, limit: 25)";
        MPLOG_WARN("TEST_ONLY: Rejected " + txid_u256.GetHex() + " - " + err);
        return TxAcceptResult::Rejected(TxRejectCode::TOO_MANY_ANCESTORS, err, txid_u256);
    }

    // STEP 3.3: Enforce descendant limits
    // Check if adding this transaction would cause any parent to exceed descendant limits
    const size_t MAX_DESCENDANT_COUNT = 24;  // Not including the parent itself
    const size_t MAX_DESCENDANT_SIZE = 101 * 1024;  // 101KB

    for (const auto& input : tx.vin) {
        uint256 parent_txid = input.prevout.txid.AsUint256();

        auto parent_it = m_transactions.find(parent_txid);
        if (parent_it == m_transactions.end()) continue;  // Parent not in mempool

        // Count descendants of this parent (including the new transaction)
        size_t descendant_count = 1;  // Count the new transaction
        size_t descendant_size = tx_size;
        std::unordered_set<uint256> visited_descendants;
        std::vector<uint256> to_visit_descendants;

        // Find all existing descendants of parent
        for (const auto& [mempool_txid, mempool_entry] : m_transactions) {
            if (mempool_txid == parent_txid) continue;  // Skip the parent itself

            // Check if this transaction spends from the parent
            for (const auto& tx_input : mempool_entry.tx.vin) {
                TxId input_parent_txid = tx_input.prevout.txid;
                if (input_parent_txid.AsUint256() == parent_txid) {
                    to_visit_descendants.push_back(mempool_txid);
                    break;
                }
            }
        }

        // BFS to find all descendants
        while (!to_visit_descendants.empty()) {
            uint256 current = to_visit_descendants.back();
            to_visit_descendants.pop_back();

            if (visited_descendants.count(current)) continue;
            visited_descendants.insert(current);

            auto descendant_it = m_transactions.find(current);
            if (descendant_it == m_transactions.end()) continue;

            descendant_count++;
            descendant_size += descendant_it->second.tx_size;

            // Add this descendant's children
            for (const auto& [mempool_txid, mempool_entry] : m_transactions) {
                if (visited_descendants.count(mempool_txid)) continue;

                for (const auto& tx_input : mempool_entry.tx.vin) {
                    TxId input_parent_txid = tx_input.prevout.txid;
                    if (input_parent_txid.AsUint256() == current) {
                        to_visit_descendants.push_back(mempool_txid);
                        break;
                    }
                }
            }
        }

        // Check descendant count limit
        if (descendant_count > MAX_DESCENDANT_COUNT) {
            std::string err = "too-long-mempool-chain (would cause parent " + parent_txid.GetHex().substr(0, 16) +
                        "... to have " + std::to_string(descendant_count + 1) +
                        " descendants including itself, limit: 25)";
            MPLOG_WARN("TEST_ONLY: Rejected " + txid_u256.GetHex() + " - " + err);
            return TxAcceptResult::Rejected(TxRejectCode::TOO_MANY_DESCENDANTS, err, txid_u256);
        }

        // Check descendant size limit
        if (descendant_size > MAX_DESCENDANT_SIZE) {
            std::string err = "too-long-mempool-chain (would cause parent " + parent_txid.GetHex().substr(0, 16) +
                        "... to exceed descendant size limit: " + std::to_string(descendant_size) +
                        " > " + std::to_string(MAX_DESCENDANT_SIZE) + " bytes)";
            MPLOG_WARN("TEST_ONLY: Rejected " + txid_u256.GetHex() + " - " + err);
            return TxAcceptResult::Rejected(TxRejectCode::DESCENDANT_SIZE_EXCEEDED, err, txid_u256);
        }
    }

    // STEP 3.6: Calculate package feerate (CPFP support)
    // Traverse all ancestors to compute total package fee and size
    // This enables Child-Pays-For-Parent: high-fee children boost low-fee parents
    {
        // Start with transaction itself
        entry.ancestor_fee = fee;
        entry.ancestor_size = tx_size;
        entry.ancestor_effective_vsize = GetEffectiveVirtualSize(entry);

        // BFS traverse all ancestors
        std::unordered_set<uint256> visited_ancestors;
        std::vector<uint256> to_visit(entry.depends.begin(), entry.depends.end());

        while (!to_visit.empty()) {
            uint256 ancestor_txid = to_visit.back();
            to_visit.pop_back();

            if (visited_ancestors.count(ancestor_txid)) continue;
            visited_ancestors.insert(ancestor_txid);

            auto ancestor_it = m_transactions.find(ancestor_txid);
            if (ancestor_it == m_transactions.end()) continue;  // Ancestor not in mempool

            const auto& ancestor_entry = ancestor_it->second;
            entry.ancestor_fee += ancestor_entry.fee;
            entry.ancestor_size += ancestor_entry.tx_size;
            entry.ancestor_effective_vsize += GetEffectiveVirtualSize(ancestor_entry);

            // Add this ancestor's parents to the queue
            for (const auto& parent_txid : ancestor_entry.depends) {
                if (!visited_ancestors.count(parent_txid)) {
                    to_visit.push_back(parent_txid);
                }
            }
        }

        // Calculate package feerate
        entry.ancestor_feerate = entry.ancestor_size > 0
            ? static_cast<double>(entry.ancestor_fee) / entry.ancestor_size
            : 0.0;
        entry.ancestor_adjusted_feerate = entry.ancestor_effective_vsize > 0
            ? static_cast<double>(entry.ancestor_fee) / entry.ancestor_effective_vsize
            : 0.0;

        MPLOG_DEBUG("TEST_ONLY: Package feerate for " + txid_u256.GetHex() + ": " +
                   std::to_string(entry.ancestor_adjusted_feerate) + " sat/vB-effective " +
                   "(package: " + std::to_string(entry.ancestor_fee) + " sats / " +
                   std::to_string(entry.ancestor_effective_vsize) + " vB-effective, raw " +
                   std::to_string(entry.ancestor_size) + " bytes, " +
                   "ancestors: " + std::to_string(visited_ancestors.size()) + ")");
    }

    // Update spent outputs tracking
    for (const auto& input : tx.vin) {
        OutPoint outpoint{input.prevout.txid, input.prevout.vout};
        m_spent_outputs.insert(outpoint);
        entry.spends.push_back(outpoint);
    }

    // Add to main storage
    m_transactions[txid_u256] = entry;

    // Add to indices (must be done before eviction for proper fee-based eviction)
    // Use package selection score so CT proof-heavy packages pay for effective vsize.
    m_fee_index.insert({GetPackageSelectionScore(entry), txid_u256});
    m_time_index.insert({entry.time, txid_u256});

    // STEP 3.4: Enforce mempool size limit
    // Check if mempool exceeds maximum size after adding this transaction
    // NOTE: Calculate inline to avoid deadlock (we already hold m_mutex lock)
    size_t total_size = 0;
    for (const auto& pair : m_transactions) {
        total_size += pair.second.tx_size;
    }

    if (total_size > m_max_size) {
        MPLOG_WARN("⚠️ EVICTION PATH ENTERED: mempool size=" + std::to_string(total_size) +
                   " bytes (" + std::to_string(total_size / (1024*1024)) + "MB)" +
                   " max=" + std::to_string(m_max_size) +
                   " bytes (" + std::to_string(m_max_size / (1024*1024)) + "MB)" +
                   " tx_count=" + std::to_string(m_transactions.size()));

        // Evict lowest fee transactions until under limit
        while (total_size > m_max_size && !m_fee_index.empty()) {
            auto lowest_fee_it = m_fee_index.begin();
            uint256 txid_to_remove = lowest_fee_it->second;

            // If we're about to evict the transaction we just added, reject it instead
            if (txid_to_remove == txid_u256) {
                removeTransactionLocked(txid_u256);

                std::string err = "mempool full (package feerate too low: " + std::to_string(GetPackageSelectionScore(entry)) +
                           " sat/vB-effective, could not evict lower-feerate packages)";
                MPLOG_WARN("TEST_ONLY: Rejected " + txid_u256.GetHex() + " - " + err);
                return TxAcceptResult::Rejected(TxRejectCode::MEMPOOL_FULL, err, txid_u256);
            }

            MPLOG_DEBUG("TEST_ONLY: Evicting low-feerate package: " + txid_to_remove.GetHex() +
                       " (package feerate: " + std::to_string(lowest_fee_it->first) + " sat/byte)");

            removeTransactionLocked(txid_to_remove);

            // Recalculate total size for next iteration
            total_size = 0;
            for (const auto& pair : m_transactions) {
                total_size += pair.second.tx_size;
            }
        }

        MPLOG_INFO("TEST_ONLY: Eviction complete, new mempool size: " + std::to_string(total_size));
    }

    // Update mempool UTXO overlay
    for (const auto& input : tx.vin) {
        // Phase M.0: Wallet boundary - convert string txid to uint256
        OutPoint out{input.prevout.txid, input.prevout.vout};
        coins_view_.spendCoin(out);
    }

    for (size_t vout = 0; vout < tx.vout.size(); ++vout) {
        OutPoint out{TxId(txid_u256), static_cast<uint32_t>(vout)};
        Coin coin;
        coin.amount = tx.vout[vout].value.GetUna();  // AmountUna → uint64_t
        coin.script_pubkey = std::string(tx.vout[vout].scriptPubKey.begin(),
                                         tx.vout[vout].scriptPubKey.end());
        coin.height = static_cast<int>(current_height);
        coin.coinbase = false;

        // Convert Coin to UTXOEntry for CoinsViewMemPool
        consensus::UTXOEntry utxo_entry;
        utxo_entry.value = AmountUna::Una(coin.amount);  // uint64_t → AmountUna
        utxo_entry.scriptPubKey.assign(coin.script_pubkey.begin(), coin.script_pubkey.end());
        utxo_entry.height = static_cast<uint32_t>(coin.height);
        utxo_entry.isCoinbase = coin.coinbase;
        utxo_entry.is_confidential = tx.vout[vout].is_confidential;
        utxo_entry.commitment = tx.vout[vout].commitment;
        coins_view_.addCoin(out, utxo_entry);
    }

    MPLOG_INFO("[" + source + "] TEST_ONLY: Added transaction to mempool: " + txid_u256.GetHex() +
               " (fee: " + std::to_string(fee) + " sats, " +
               "rate: " + std::to_string(fee_rate) + " sat/byte)");

    m_total_tx_added.fetch_add(1);

    return TxAcceptResult::Accepted(txid_u256);
#endif
}

// STEP 3: Check if output is spent in mempool (for wallet coin selection)
// Phase M.0: Updated to use OutPoint directly
bool Mempool::isOutputSpentInMempool(const OutPoint& outpoint) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_spent_outputs.find(outpoint) != m_spent_outputs.end();
}

std::shared_ptr<Transaction> Mempool::getTransaction(const uint256& txid) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    auto it = m_transactions.find(txid);
    if (it != m_transactions.end()) {
        return std::make_shared<Transaction>(it->second.tx);
    }
    return nullptr;
}

std::optional<MempoolEntry> Mempool::getMempoolEntry(const uint256& txid) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    auto it = m_transactions.find(txid);
    if (it != m_transactions.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<uint64_t> Mempool::getTransactionFee(const uint256& txid) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    auto it = m_transactions.find(txid);
    if (it != m_transactions.end()) {
        return it->second.fee;
    }
    return std::nullopt;
}

std::optional<double> Mempool::getTransactionFeeRate(const uint256& txid) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    auto it = m_transactions.find(txid);
    if (it != m_transactions.end()) {
        return it->second.fee_rate;
    }
    return std::nullopt;
}

void Mempool::forEachEntry(const std::function<void(const MempoolEntry&)>& fn) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    for (const auto& [txid, entry] : m_transactions) {
        fn(entry);
    }
}

std::vector<Transaction> Mempool::getAllTransactions() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    
    std::vector<Transaction> transactions;
    transactions.reserve(m_transactions.size());
    
    for (const auto& pair : m_transactions) {
        transactions.push_back(pair.second.tx);
    }
    
    return transactions;
}

std::vector<Transaction> Mempool::getTransactionsByFeeRate(size_t max_count) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    
    std::vector<Transaction> transactions;
    transactions.reserve(std::min(max_count, m_transactions.size()));
    
    // Iterate from highest fee rate to lowest
    for (auto it = m_fee_index.rbegin(); 
         it != m_fee_index.rend() && transactions.size() < max_count; 
         ++it) {
        
        auto tx_it = m_transactions.find(it->second);
        if (tx_it != m_transactions.end()) {
            transactions.push_back(tx_it->second.tx);
        }
    }
    
    return transactions;
}

std::vector<uint256> Mempool::getTransactionIds() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    std::vector<uint256> txids;
    txids.reserve(m_transactions.size());

    for (const auto& pair : m_transactions) {
        txids.push_back(pair.first);
    }

    return txids;
}

std::vector<Transaction> Mempool::getTransactionsForAddress(const std::string& address) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    std::vector<Transaction> result;

    mining::AddressInfo decoded;
    if (!mining::DecodeAddress(address, decoded)) {
        MPLOG_WARN("getTransactionsForAddress rejected invalid address: " + address);
        return result;
    }

    const std::vector<uint8_t> target_script = mining::BuildScriptPubKey(decoded);
    if (target_script.empty()) {
        MPLOG_WARN("getTransactionsForAddress could not derive script for address: " + address);
        return result;
    }

    // Scan all mempool transactions for outputs to this script or inputs spending it.
    for (const auto& pair : m_transactions) {
        const Transaction& tx = pair.second.tx;
        bool involves_address = false;

        // Check outputs.
        for (const auto& output : tx.vout) {
            if (output.scriptPubKey == target_script) {
                involves_address = true;
                break;
            }
        }

        // Check inputs (spent prevouts).
        // Resolve prevouts against mempool parents first, then the base chain.
        // CoinsViewMemPool intentionally returns NotFound for outputs already
        // spent by mempool transactions, so using it here would hide
        // source-address spends from address-indexed RPC queries.
        if (!involves_address) {
            for (const auto& input : tx.vin) {
                bool matched_input = false;

                auto parent_it = m_transactions.find(input.prevout.txid.AsUint256());
                if (parent_it != m_transactions.end() &&
                    input.prevout.vout < parent_it->second.tx.vout.size() &&
                    parent_it->second.tx.vout[input.prevout.vout].scriptPubKey == target_script) {
                    matched_input = true;
                }

                if (!matched_input) {
                    OutPoint spent_out{input.prevout.txid, input.prevout.vout};
                    auto spent_coin = chain_state_view_ ? chain_state_view_->getCoin(spent_out)
                                                        : StatusOr<consensus::UTXOEntry>(Status::NotFound);
                    if (spent_coin.status() == Status::Ok &&
                        spent_coin.value().scriptPubKey == target_script) {
                        matched_input = true;
                    }
                }

                if (matched_input) {
                    involves_address = true;
                    break;
                }
            }
        }

        if (involves_address) {
            result.push_back(tx);
        }
    }

    MPLOG_DEBUG("Found " + std::to_string(result.size()) + " mempool transactions for address: " + address);
    return result;
}

size_t Mempool::size() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_transactions.size();
}

uint64_t Mempool::getTotalFees() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return getTotalFeesLocked();
}

size_t Mempool::getTotalSize() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return getTotalSizeLocked();
}

void Mempool::excludeFromBlockTemplates(const uint256& txid,
                                        const std::string& reason,
                                        std::chrono::seconds duration) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_template_exclusions[txid] = TemplateExclusion{
        std::chrono::steady_clock::now() + duration,
        reason
    };

    MPLOG_WARN("Template exclusion armed for " + txid.GetHex().substr(0, 16) +
               "... for " + std::to_string(duration.count()) + "s: " + reason);
}

bool Mempool::isExcludedFromBlockTemplates(const uint256& txid, std::string* reason) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return isTemplateExcludedLocked(txid, std::chrono::steady_clock::now(), reason);
}

bool Mempool::isTemplateExcludedLocked(const uint256& txid,
                                       const std::chrono::steady_clock::time_point& now,
                                       std::string* reason) const {
    auto it = m_template_exclusions.find(txid);
    if (it == m_template_exclusions.end()) {
        return false;
    }
    if (it->second.expires_at <= now) {
        return false;
    }
    if (reason) {
        *reason = it->second.reason;
    }
    return true;
}

std::vector<Transaction> Mempool::selectTransactionsForBlock(
    size_t max_block_size, uint64_t max_block_weight,
    uint32_t next_block_height) const {

    std::shared_lock<std::shared_mutex> lock(m_mutex);

    // Re-validate height-gated proof rules at template-selection time.
    // A shielded tx admitted before an activation boundary may no longer be
    // valid for the block currently being assembled.
    std::unordered_map<uint256, bool> selectable_cache;
    std::unordered_map<uint256, std::string> selectable_reason_cache;
    size_t height_rule_excluded = 0;
    auto selectable_at_height = [&](const uint256& txid,
                                    const MempoolEntry& entry,
                                    std::string* reason) -> bool {
        auto cached = selectable_cache.find(txid);
        if (cached != selectable_cache.end()) {
            if (reason) {
                auto reason_it = selectable_reason_cache.find(txid);
                *reason = reason_it != selectable_reason_cache.end()
                    ? reason_it->second
                    : std::string{};
            }
            return cached->second;
        }

        std::string local_reason;
        const bool selectable =
            isSelectableAtHeightLocked(entry, next_block_height, &local_reason);
        selectable_cache.emplace(txid, selectable);
        if (!selectable) {
            selectable_reason_cache.emplace(txid, local_reason);
            ++height_rule_excluded;
            MPLOG_WARN("selectTransactionsForBlock: excluding tx " +
                txid.GetHex() + " at target height " +
                std::to_string(next_block_height) + ": " + local_reason);
        }
        if (reason) {
            *reason = local_reason;
        }
        return selectable;
    };

    // Total timeout for block template transaction selection.
    // If the entire selection takes longer than 2 seconds, stop adding
    // transactions and return what we have. This prevents the block template
    // RPC (getblocktemplate / generatetoaddress) from hanging when the mempool
    // contains slow-to-process transactions (e.g., consolidation txs with 30+
    // inputs whose ancestor-graph BFS is expensive).
    static constexpr auto SELECTION_TIMEOUT = std::chrono::milliseconds(2000);
    const auto selectionStart = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();

    // ═══════════════════════════════════════════════════════════════════════════
    // CPFP (Child-Pays-For-Parent) Package Selection Algorithm
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // Instead of selecting transactions by individual fee rate, we use ancestor
    // score (total fee / total size of transaction + all unconfirmed ancestors).
    // This allows high-fee child transactions to "pay for" low-fee parents.
    //
    // Algorithm:
    // 1. Calculate ancestor score for each transaction
    // 2. Sort by ancestor score (highest first)
    // 3. Select transactions ensuring all ancestors are included
    // 4. Respect block size and weight limits

    // Structure to hold transaction with its ancestor score
    struct TxWithAncestorScore {
        uint256 txid;
        const MempoolEntry* entry;
        uint64_t ancestor_fee;         // Total fee of tx + all ancestors
        size_t ancestor_size;          // Total size of tx + all ancestors
        size_t ancestor_effective_vsize; // Total CT-aware vsize of tx + ancestors
        uint64_t ancestor_vwu;         // Phase 8 Commit 2: total VWU of tx + ancestors
        double ancestor_fee_rate;      // ancestor_fee / ancestor_vwu (VWU-based now)
        uint64_t ancestor_fee_rate_int;  // Integer fee rate (una per 1000 VWU)
        std::vector<uint256> ancestors;  // List of ancestor txids
    };

    std::vector<TxWithAncestorScore> scored_txs;
    scored_txs.reserve(m_transactions.size());
    size_t template_excluded = 0;
    size_t excluded_descendants = 0;

    // ───────────────────────────────────────────────────────────────────────────
    // Phase 1: Calculate ancestor scores for all transactions
    // ───────────────────────────────────────────────────────────────────────────

    bool scoring_timed_out = false;
    size_t freeze_fork_excluded = 0;
    for (const auto& [txid, entry] : m_transactions) {
        if (isTemplateExcludedLocked(txid, now, nullptr)) {
            ++template_excluded;
            continue;
        }
        if (!selectable_at_height(txid, entry, nullptr)) {
            continue;
        }

        // Check timeout: if scoring is taking too long, stop scoring remaining txs
        if (std::chrono::steady_clock::now() - selectionStart > SELECTION_TIMEOUT) {
            MPLOG_WARN("selectTransactionsForBlock: timeout during ancestor scoring after " +
                std::to_string(scored_txs.size()) + " of " +
                std::to_string(m_transactions.size()) + " txs");
            scoring_timed_out = true;
            break;
        }

        TxWithAncestorScore score;
        score.txid = txid;
        score.entry = &entry;
        score.ancestor_fee = entry.fee;
        score.ancestor_size = entry.tx_size;
        score.ancestor_effective_vsize = GetEffectiveVirtualSize(entry);
        score.ancestor_vwu = entry.vwu > 0 ? entry.vwu : entry.tx_size;
        bool excluded_ancestor = false;

        // Find all unconfirmed ancestors (mempool parents)
        std::unordered_set<uint256> visited;
        std::vector<uint256> to_visit;

        // Start with direct parents
        for (const auto& input : entry.tx.vin) {
            TxId parent_txid = input.prevout.txid;

            if (isTemplateExcludedLocked(parent_txid.AsUint256(), now, nullptr)) {
                excluded_ancestor = true;
                break;
            }

            // Only consider parents in mempool (unconfirmed)
            auto parent_it = m_transactions.find(parent_txid.AsUint256());
            if (parent_it != m_transactions.end()) {
                if (!selectable_at_height(parent_it->first, parent_it->second, nullptr)) {
                    excluded_ancestor = true;
                    break;
                }
                to_visit.push_back(parent_it->first);
            }
        }

        if (excluded_ancestor) {
            ++excluded_descendants;
            continue;
        }

        // BFS to find all ancestors
        while (!to_visit.empty()) {
            uint256 current = to_visit.back();
            to_visit.pop_back();

            if (visited.count(current)) continue;
            visited.insert(current);

            if (isTemplateExcludedLocked(current, now, nullptr)) {
                excluded_ancestor = true;
                break;
            }

            auto ancestor_it = m_transactions.find(current);
            if (ancestor_it == m_transactions.end()) continue;

            const auto& ancestor_entry = ancestor_it->second;
            if (!selectable_at_height(current, ancestor_entry, nullptr)) {
                excluded_ancestor = true;
                break;
            }

            score.ancestors.push_back(current);
            score.ancestor_fee += ancestor_entry.fee;
            score.ancestor_size += ancestor_entry.tx_size;
            score.ancestor_effective_vsize += GetEffectiveVirtualSize(ancestor_entry);
            score.ancestor_vwu += ancestor_entry.vwu > 0
                                      ? ancestor_entry.vwu
                                      : static_cast<uint64_t>(ancestor_entry.tx_size);

            // Add this ancestor's parents to visit list
            for (const auto& input : ancestor_entry.tx.vin) {
                TxId grandparent_txid = input.prevout.txid;
                if (isTemplateExcludedLocked(grandparent_txid.AsUint256(), now, nullptr)) {
                    excluded_ancestor = true;
                    break;
                }
                if (m_transactions.find(grandparent_txid.AsUint256()) != m_transactions.end() &&
                    !visited.count(grandparent_txid.AsUint256())) {
                    to_visit.push_back(grandparent_txid.AsUint256());
                }
            }
            if (excluded_ancestor) {
                break;
            }
        }

        if (excluded_ancestor) {
            ++excluded_descendants;
            continue;
        }

        // Phase 8 Commit 2: switch selection feerate denominator from BIP141
        // effective_vsize to VWU. VWU is the economic truth — it weights
        // ML-DSA-65 witness bytes without the 4× BIP141 discount that
        // would otherwise make P2MR spends look artificially cheap and
        // make a signature-spam attack viable.
        const uint64_t denom_vwu = score.ancestor_vwu > 0
                                      ? score.ancestor_vwu
                                      : static_cast<uint64_t>(score.ancestor_size);
        score.ancestor_fee_rate = denom_vwu > 0
            ? static_cast<double>(score.ancestor_fee) / static_cast<double>(denom_vwu)
            : 0.0;

        // Integer fee rate (una per 1000 VWU) for deterministic sorting.
        score.ancestor_fee_rate_int = denom_vwu > 0
            ? (score.ancestor_fee * 1000) / denom_vwu
            : 0;

        scored_txs.push_back(score);
    }

    // ───────────────────────────────────────────────────────────────────────────
    // Phase 2: Sort by ancestor fee rate (highest first)
    // BT3/BT4 FIX: Use integer comparison for determinism, txid for tie-breaking
    // ───────────────────────────────────────────────────────────────────────────

    std::sort(scored_txs.begin(), scored_txs.end(),
              [](const TxWithAncestorScore& a, const TxWithAncestorScore& b) {
                  // BT3: Use integer fee rate to avoid floating-point non-determinism
                  if (a.ancestor_fee_rate_int != b.ancestor_fee_rate_int) {
                      return a.ancestor_fee_rate_int > b.ancestor_fee_rate_int;
                  }
                  // BT4: Tie-breaker - smaller txid first for deterministic ordering
                  return a.txid < b.txid;
              });

    // ───────────────────────────────────────────────────────────────────────────
    // Phase 3: Select transactions with ancestors
    // ───────────────────────────────────────────────────────────────────────────

    std::vector<Transaction> selected;
    std::unordered_set<uint256> included;  // Phase M.0: uint256
    size_t current_size = 0;
    uint64_t current_weight = 0;

    for (const auto& score : scored_txs) {
        // Check timeout: stop adding transactions if we've exceeded the budget
        if (std::chrono::steady_clock::now() - selectionStart > SELECTION_TIMEOUT) {
            MPLOG_WARN("selectTransactionsForBlock: timeout during selection after " +
                std::to_string(selected.size()) + " txs included, returning partial template");
            break;
        }

        // Skip if already included
        if (included.count(score.txid)) {
            continue;
        }
        if (!selectable_at_height(score.txid, *score.entry, nullptr)) {
            continue;
        }

        // Calculate total size if we include this transaction + all ancestors
        size_t package_size = score.entry->tx_size;
        for (const auto& ancestor_txid : score.ancestors) {
            if (!included.count(ancestor_txid)) {
                auto ancestor_it = m_transactions.find(ancestor_txid);
                if (ancestor_it != m_transactions.end()) {
                    package_size += ancestor_it->second.tx_size;
                }
            }
        }

        uint64_t package_weight = package_size * 4;  // Simplified weight

        // Check if package fits in block
        if (current_size + package_size > max_block_size ||
            current_weight + package_weight > max_block_weight) {
            continue;  // Skip this package - doesn't fit
        }

        // Include all ancestors first (in correct order)
        bool package_valid = true;
        for (const auto& ancestor_txid : score.ancestors) {
            if (!included.count(ancestor_txid)) {
                auto ancestor_it = m_transactions.find(ancestor_txid);
                if (ancestor_it != m_transactions.end()) {
                    if (!selectable_at_height(ancestor_txid, ancestor_it->second, nullptr)) {
                        package_valid = false;
                        break;
                    }
                    selected.push_back(ancestor_it->second.tx);
                    included.insert(ancestor_txid);
                    current_size += ancestor_it->second.tx_size;
                    current_weight += ancestor_it->second.tx_size * 4;
                }
            }
        }
        if (!package_valid) {
            continue;
        }

        // Include the transaction itself
        selected.push_back(score.entry->tx);
        included.insert(score.txid);
        current_size += score.entry->tx_size;
        current_weight += score.entry->tx_size * 4;

        MPLOG_DEBUG("CPFP: Selected " + score.txid.GetHex() + " with " +
                     std::to_string(score.ancestors.size()) + " ancestors, " +
                     "ancestor fee rate: " + std::to_string(score.ancestor_fee_rate) + " sat/vB-effective");
    }

    MPLOG_INFO("CPFP block template: Selected " + std::to_string(selected.size()) +
                 " transactions (" + std::to_string(current_size) + " bytes, " +
                 std::to_string(current_weight) + " weight units, excluded=" +
                 std::to_string(template_excluded) + ", excluded_descendants=" +
                 std::to_string(excluded_descendants) + ", height_rule_excluded=" +
                 std::to_string(height_rule_excluded) + ")");

    return selected;
}

bool Mempool::isSelectableAtHeightLocked(const MempoolEntry& entry,
                                         uint32_t next_block_height,
                                         std::string* reason) const {
    const Transaction& tx = entry.tx;
    if (next_block_height == 0) {
        return true;
    }

    // Transparent transactions are admitted under the consensus rules for
    // the then-next block (entry.height + 1). Reorgs can move the next block
    // across Taproot or covenant activation boundaries while leaving the
    // transaction in the mempool. Re-run canonical validation only when the
    // relevant activation state changed; otherwise avoid adding a full script
    // verification pass to every block-template build.
    if (!UsesShieldedValueSemantics(tx)) {
        const uint32_t admission_height =
            entry.height == std::numeric_limits<uint32_t>::max()
                ? entry.height
                : entry.height + 1;
        const auto& params = dinero::Params();
        const bool script_path_state_changed =
            consensus::CovenantActivationParams::IsScriptPathActive(
                admission_height, params) !=
            consensus::CovenantActivationParams::IsScriptPathActive(
                next_block_height, params);
        const bool covenant_state_changed =
            consensus::CovenantActivationParams::CovenantFlags(
                admission_height, params) !=
            consensus::CovenantActivationParams::CovenantFlags(
                next_block_height, params);

        if (!script_path_state_changed && !covenant_state_changed) {
            return true;
        }

        std::string validation_error;
        if (!validateTransaction(
                tx, validation_error, next_block_height)) {
            if (reason) {
                *reason =
                    "height-gated script validation failed: " +
                    validation_error;
            }
            return false;
        }
        return true;
    }

    // Shielded epoch reset wall: the cutover block must be shielded-empty, so no
    // shielded tx is selectable into block H. ConnectBlock enforces this at
    // consensus; excluding it from the template keeps a well-behaved miner from
    // building a block that would be rejected. The tx stays in the mempool (it
    // becomes selectable again once the tip is past H, subject to the usual
    // anchor check against the fresh post-reset pool).
    const uint32_t shielded_reset_height =
        dinero::Params().shielded_epoch_reset_height;
    if (consensus::shielded::IsShieldedEpochResetHeight(next_block_height,
            shielded_reset_height,
            dinero::Params().shielded_spend_auth_epoch_reset_height)) {
        if (reason) {
            *reason = "shielded tx not allowed at the epoch reset height";
        }
        return false;
    }

    const uint32_t binding_activation =
        dinero::Params().shielded_input_binding_activation_height;
    if (next_block_height < binding_activation) {
        return true;
    }

    auto set_reason = [&](const std::string& msg) {
        if (reason) {
            *reason = msg;
        }
    };

    if (!Transaction::IsShieldedVersion(tx.version) ||
        tx.shielded_bundle_bytes.empty()) {
        set_reason("malformed shielded transaction");
        return false;
    }

    if (!shielded_tree_ || !shielded_nullifiers_) {
        set_reason("shielded state unavailable");
        return false;
    }
    if (!chain_db_) {
        set_reason("chain state unavailable");
        return false;
    }

    consensus::shielded::ShieldedBundle bundle;
    const auto decode = consensus::shielded::DeserializeShieldedBundle(
        tx.shielded_bundle_bytes, &bundle);
    if (decode != consensus::shielded::BundleDecodeError::Ok) {
        set_reason("shielded bundle decode failed (code " +
                   std::to_string(static_cast<int>(decode)) + ")");
        return false;
    }

    uint64_t total_input_value = 0;
    MempoolUTXOView utxo_view(&coins_view_, chain_db_, &m_transactions);
    for (const auto& input : tx.vin) {
        const uint256 input_txid = input.prevout.txid.AsUint256();
        const uint32_t input_vout = input.prevout.vout;

        uint64_t value = 0;
        std::string spk_str;
        if (!utxo_view.GetUTXO(input_txid, input_vout, value, spk_str)) {
            OutPoint outpoint{input.prevout.txid, input.prevout.vout};
            if (auto recovered = recoverConflictedInputUTXO(outpoint)) {
                value = recovered->value.GetUna();
                if (recovered->is_confidential) {
                    set_reason("legacy private lane removed");
                    return false;
                }
            } else {
                set_reason("input UTXO not found: " + input_txid.GetHex() +
                           ":" + std::to_string(input_vout));
                return false;
            }
        } else {
            std::vector<uint8_t> commitment;
            bool is_confidential = false;
            if (utxo_view.GetConfidentialUTXO(input_txid, input_vout,
                                              commitment, is_confidential) &&
                is_confidential) {
                set_reason("legacy private lane removed");
                return false;
            }
        }
        total_input_value += value;
    }

    uint64_t total_output_value = 0;
    for (const auto& output : tx.vout) {
        total_output_value += output.value.GetUna();
    }

    uint64_t fee = 0;
    if (tx.HasExplicitFee()) {
        fee = tx.GetExplicitFee();
    } else if (total_output_value > total_input_value) {
        set_reason("outputs exceed inputs (negative fee)");
        return false;
    } else {
        fee = total_input_value - total_output_value;
    }

    int64_t transparent_delta = 0;
    std::string error;
    if (!ComputeTransparentValueDelta(total_input_value, total_output_value,
                                      fee, transparent_delta, error)) {
        set_reason(error);
        return false;
    }

    auto ctx = consensus::shielded::BuildShieldedValidationContext(
        tx,
        shielded_nullifiers_,
        shielded_tree_,
        next_block_height,
        transparent_delta,
        dinero::Params().shielded_activation_height,
        /*anchor_history=*/nullptr,
        binding_activation,
        dinero::Params().shielded_cv_binding_activation_height,
        dinero::Params().shielded_spend_auth_activation_height);
    const auto validation = consensus::shielded::ValidateShieldedBundle(bundle, ctx);
    if (validation != consensus::shielded::ShieldedValidationError::Ok) {
        set_reason("shielded validation failed: " +
                   std::string(ShieldedValidationErrorToString(validation)));
        return false;
    }

    return true;
}

void Mempool::removeConfirmedTransactions(const std::vector<uint256>& confirmed_txids) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    // v0.13.0.3: Get current height for fee estimation
    uint32_t confirmation_height = 0;
    if (chain_db_) {
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() == Status::Ok) {
            confirmation_height = static_cast<uint32_t>(tip_result.value().height);
        }
    }

    size_t removed_count = 0;
    for (const auto& txid : confirmed_txids) {
        // v0.13.0.3: Record confirmation for fee estimation (before removal)
        // Phase M.1.A: FeeEstimator now accepts uint256 directly (no GetHex conversion)
        if (fee_estimator_ && confirmation_height > 0) {
            fee_estimator_->recordTxConfirmation(txid, confirmation_height);
        }

        if (removeTransaction(txid)) {
            removed_count++;
        }
    }

    if (removed_count > 0) {
        MPLOG_INFO("Removed " + std::to_string(removed_count) +
                     " confirmed transactions from mempool");

        // v0.11.0: Clear and rebuild UTXO overlay after block acceptance
        // ChainDB has been updated with the new block, so mempool view must be rebuilt
        coins_view_.clear();

        // Rebuild overlay from remaining mempool transactions
        uint32_t current_height = 0;
        if (chain_db_) {
            auto tip_result = chain_db_->getTip();
            if (tip_result.status() == Status::Ok) {
                current_height = static_cast<uint32_t>(tip_result.value().height);
            }
        }

        for (const auto& [txid, entry] : m_transactions) {
            // Spend inputs
            for (const auto& input : entry.tx.vin) {
                // Phase M.0: Wallet boundary - convert string txid to uint256
                OutPoint out{input.prevout.txid, input.prevout.vout};
                coins_view_.spendCoin(out);
            }

            // Add outputs
            for (size_t vout = 0; vout < entry.tx.vout.size(); ++vout) {
                OutPoint out{TxId(txid), static_cast<uint32_t>(vout)};
                Coin coin;
                coin.amount = entry.tx.vout[vout].value.GetUna();
                coin.script_pubkey = std::string(entry.tx.vout[vout].scriptPubKey.begin(),
                                                 entry.tx.vout[vout].scriptPubKey.end());
                coin.height = static_cast<int>(current_height);
                coin.coinbase = false;

                // Convert Coin to UTXOEntry for CoinsViewMemPool
                consensus::UTXOEntry utxo_entry;
                utxo_entry.value = AmountUna::Una(coin.amount);
                utxo_entry.scriptPubKey.assign(coin.script_pubkey.begin(), coin.script_pubkey.end());
                utxo_entry.height = static_cast<uint32_t>(coin.height);
                utxo_entry.isCoinbase = coin.coinbase;
                utxo_entry.is_confidential = entry.tx.vout[vout].is_confidential;
                utxo_entry.commitment = entry.tx.vout[vout].commitment;
                coins_view_.addCoin(out, utxo_entry);
            }
        }

        MPLOG_DEBUG("Rebuilt mempool UTXO overlay with " +
                     std::to_string(coins_view_.spentCount()) + " spent + " +
                     std::to_string(coins_view_.createdCount()) + " created outputs");
    }
}

void Mempool::removeExpiredTransactions() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - m_max_age;

    std::vector<uint256> expired_txids;

    // Find expired transactions
    for (auto it = m_time_index.begin();
         it != m_time_index.end() && it->first < cutoff;
         ++it) {
        expired_txids.push_back(it->second);
    }
    
    // Remove expired transactions
    for (const auto& txid : expired_txids) {
        removeTransaction(txid);
    }

    if (!expired_txids.empty()) {
        MPLOG_INFO("Removed " + std::to_string(expired_txids.size()) +
                     " expired transactions from mempool");
    }
}

void Mempool::limitMempoolSize() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    
    if (getTotalSize() <= m_max_size) {
        return;
    }
    
    evictTransactions();
}

void Mempool::clear() {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    size_t count = m_transactions.size();
    m_transactions.clear();
    m_spent_outputs.clear();
    m_fee_index.clear();
    m_time_index.clear();
    m_template_exclusions.clear();
    coins_view_.clear();  // v0.11.0: Clear mempool UTXO overlay

    MPLOG_INFO("Cleared mempool (" + std::to_string(count) + " transactions)");
}

Mempool::MempoolStats Mempool::getStats() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    
    MempoolStats stats{};
    stats.tx_count = m_transactions.size();
    stats.total_size = getTotalSizeLocked();
    stats.total_fees = getTotalFeesLocked();
    
    if (!m_transactions.empty()) {
        // Calculate average and median fee rate. Fee rates are denominated in
        // una/vB; keep this as a raw economic metric for dashboard/RPC callers.
        double total_fee_rate = 0.0;
        double min_fee_rate = std::numeric_limits<double>::max();
        double max_fee_rate = 0.0;
        std::vector<double> fee_rates;
        fee_rates.reserve(m_transactions.size());
        
        auto oldest_time = std::chrono::steady_clock::now();
        
        for (const auto& pair : m_transactions) {
            const auto& entry = pair.second;
            total_fee_rate += entry.fee_rate;
            fee_rates.push_back(entry.fee_rate);
            min_fee_rate = std::min(min_fee_rate, entry.fee_rate);
            max_fee_rate = std::max(max_fee_rate, entry.fee_rate);
            oldest_time = std::min(oldest_time, entry.time);
        }
        std::sort(fee_rates.begin(), fee_rates.end());
        
        stats.avg_fee_rate = total_fee_rate / stats.tx_count;
        stats.median_fee_rate = fee_rates[fee_rates.size() / 2];
        stats.min_fee_rate = static_cast<size_t>(min_fee_rate);
        stats.max_fee_rate = static_cast<size_t>(max_fee_rate);
        
        auto now = std::chrono::steady_clock::now();
        stats.oldest_tx_age = std::chrono::duration_cast<std::chrono::seconds>(now - oldest_time);
    }
    
    // Utreexo proof staleness stats
    for (const auto& [txid, entry] : m_transactions) {
        if (entry.is_proof_stale) stats.stale_tx_count++;
    }
    stats.last_connected_height = current_block_height_;
    stats.stale_evicted_total = m_stale_evicted_total.load(std::memory_order_relaxed);
    stats.refresh_attempted_total = m_refresh_attempted_total.load(std::memory_order_relaxed);
    stats.refresh_succeeded_total = m_refresh_succeeded_total.load(std::memory_order_relaxed);
    stats.refresh_dropped_budget_total = m_refresh_dropped_budget_total.load(std::memory_order_relaxed);

    return stats;
}

// ============================================================================
// Utreexo Proof Staleness (CSN Mode)
// ============================================================================

bool Mempool::removeTransactionLocked(const uint256& txid) {
    // Same as removeTransaction() but caller must hold m_mutex
    auto it = m_transactions.find(txid);
    if (it == m_transactions.end()) {
        return false;
    }

    const auto& entry = it->second;

    // Remove from spent outputs tracking
    for (const auto& outpoint : entry.spends) {
        m_spent_outputs.erase(outpoint);
    }

    // Remove from indices
    EraseFeeIndexEntry(m_fee_index, txid, GetPackageSelectionScore(entry));

    auto time_range = m_time_index.equal_range(entry.time);
    for (auto time_it = time_range.first; time_it != time_range.second; ++time_it) {
        if (time_it->second == txid) {
            m_time_index.erase(time_it);
            break;
        }
    }

    updateDependencies(txid);

    for (const auto& parent_txid : entry.depends) {
        auto parent_children = m_children_index.find(parent_txid);
        if (parent_children != m_children_index.end()) {
            parent_children->second.erase(txid);
            if (parent_children->second.empty()) {
                m_children_index.erase(parent_children);
            }
        }
    }

    m_template_exclusions.erase(txid);
    m_transactions.erase(it);
    m_total_tx_removed.fetch_add(1);
    return true;
}

void Mempool::rebuildCoinsViewLocked() {
    coins_view_.clear();

    for (const auto& [txid, entry] : m_transactions) {
        for (size_t vout = 0; vout < entry.tx.vout.size(); ++vout) {
            OutPoint out{TxId(txid), static_cast<uint32_t>(vout)};
            consensus::UTXOEntry utxo_entry;
            utxo_entry.value = entry.tx.vout[vout].value;
            utxo_entry.scriptPubKey = entry.tx.vout[vout].scriptPubKey;
            utxo_entry.height = entry.height;
            utxo_entry.isCoinbase = false;
            coins_view_.addCoin(out, utxo_entry);
        }
    }

    for (const auto& [txid, entry] : m_transactions) {
        for (const auto& input : entry.tx.vin) {
            OutPoint out{input.prevout.txid, input.prevout.vout};
            coins_view_.spendCoin(out);
        }
    }
}

size_t Mempool::onBlockConnected(const Block& block, uint32_t height,
                                  const std::vector<uint8_t>& new_root) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    // 1. Collect all inputs spent in this block
    std::unordered_set<OutPoint> block_spends;
    for (const auto& tx : block.vtx) {
        if (tx.IsCoinbase()) continue;
        for (const auto& input : tx.vin) {
            block_spends.insert(OutPoint{input.prevout.txid, input.prevout.vout});
        }
    }

    // 2. Collect confirmed txids (all transactions in the block)
    std::unordered_set<uint256> confirmed_txids;
    for (const auto& tx : block.vtx) {
        confirmed_txids.insert(tx.GetTxid().AsUint256());
    }

    // 3. Find conflicting mempool TXs (double-spends) and confirmed TXs
    std::vector<uint256> to_remove;
    for (const auto& [txid, entry] : m_transactions) {
        // Remove if confirmed in this block
        if (confirmed_txids.count(txid) > 0) {
            to_remove.push_back(txid);
            continue;
        }
        // Remove if any input conflicts with block
        for (const auto& spent : entry.spends) {
            if (block_spends.count(spent) > 0) {
                to_remove.push_back(txid);
                break;
            }
        }
    }

    // 4. Remove conflicting and confirmed TXs
    size_t evicted = 0;
    size_t confirmed = 0;
    for (const auto& txid : to_remove) {
        bool was_confirmed = confirmed_txids.count(txid) > 0;
        if (removeTransactionLocked(txid)) {
            if (was_confirmed) confirmed++;
            else evicted++;
        }
    }
    if (confirmed > 0 || evicted > 0) {
        rebuildCoinsViewLocked();
    }

    // 5. Mark ALL remaining TXs as proof-stale (accumulator root changed)
    if (!new_root.empty()) {
        for (auto& [txid, entry] : m_transactions) {
            if (!entry.validated_at_root.empty()) {
                entry.is_proof_stale = true;
                entry.cached_utxotx_payload.clear();
                entry.cached_utxotx_payload.shrink_to_fit();
            }
        }
        current_accumulator_root_ = new_root;
    }
    current_block_height_ = height;

    if (confirmed > 0 || evicted > 0) {
        MPLOG_INFO("Block " + std::to_string(height) + " connected: " +
                   std::to_string(confirmed) + " confirmed, " +
                   std::to_string(evicted) + " evicted");
    }

    return evicted;
}

void Mempool::onBlockDisconnected(const Block& block, uint32_t height) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    // All mempool TXs become stale — the accumulator root changed backward
    for (auto& [txid, entry] : m_transactions) {
        if (!entry.validated_at_root.empty()) {
            entry.is_proof_stale = true;
            entry.cached_utxotx_payload.clear();
            entry.cached_utxotx_payload.shrink_to_fit();
        }
    }

    MPLOG_INFO("Block " + std::to_string(height) + " disconnected: " +
               std::to_string(m_transactions.size()) + " TXs marked stale");
}

size_t Mempool::getStaleCount() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    size_t count = 0;
    for (const auto& [txid, entry] : m_transactions) {
        if (entry.is_proof_stale) count++;
    }
    return count;
}

std::vector<uint256> Mempool::getStaleTxIds() const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    std::vector<uint256> result;
    for (const auto& [txid, entry] : m_transactions) {
        if (entry.is_proof_stale) result.push_back(txid);
    }
    return result;
}

std::vector<uint256> Mempool::selectStaleForRefresh(
    uint32_t chain_height,
    size_t max_refresh_batch,
    uint32_t max_proof_age_blocks,
    uint32_t max_refresh_attempts,
    size_t stale_overload_threshold
) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    std::vector<uint256> stale_txids;
    stale_txids.reserve(m_transactions.size());
    for (const auto& [txid, entry] : m_transactions) {
        if (entry.is_proof_stale) {
            stale_txids.push_back(txid);
        }
    }

    if (stale_txids.empty()) {
        return {};
    }

    // Overload guard: under proof-churn pressure, drop stale transactions
    // instead of triggering repeated proof refresh storms.
    if (stale_overload_threshold > 0 && stale_txids.size() >= stale_overload_threshold) {
        size_t removed = 0;
        for (const auto& txid : stale_txids) {
            if (removeTransactionLocked(txid)) {
                removed++;
            }
        }
        if (removed > 0) {
            rebuildCoinsViewLocked();
        }
        if (removed > 0) {
            m_stale_evicted_total.fetch_add(static_cast<uint64_t>(removed), std::memory_order_relaxed);
            m_refresh_dropped_budget_total.fetch_add(static_cast<uint64_t>(removed), std::memory_order_relaxed);
            MPLOG_WARN("[ProofChurnGuard] Bulk-evicted " + std::to_string(removed) +
                       " stale mempool TXs (threshold=" + std::to_string(stale_overload_threshold) + ")");
        }
        return {};
    }

    std::vector<uint256> to_evict;
    std::vector<uint256> refresh_candidates;
    refresh_candidates.reserve(std::min(max_refresh_batch, stale_txids.size()));

    for (const auto& txid : stale_txids) {
        auto it = m_transactions.find(txid);
        if (it == m_transactions.end()) {
            continue;
        }
        auto& entry = it->second;

        const bool too_old =
            entry.validated_at_height > 0 &&
            chain_height > entry.validated_at_height &&
            (chain_height - entry.validated_at_height) > max_proof_age_blocks;
        const bool attempts_exhausted = entry.proof_refresh_attempts >= max_refresh_attempts;

        if (too_old || attempts_exhausted) {
            to_evict.push_back(txid);
            continue;
        }

        if (refresh_candidates.size() < max_refresh_batch) {
            entry.proof_refresh_attempts++;
            refresh_candidates.push_back(txid);
            m_refresh_attempted_total.fetch_add(1, std::memory_order_relaxed);
        }
    }

    size_t removed = 0;
    for (const auto& txid : to_evict) {
        if (removeTransactionLocked(txid)) {
            removed++;
        }
    }
    if (removed > 0) {
        rebuildCoinsViewLocked();
    }
    if (removed > 0) {
        m_stale_evicted_total.fetch_add(static_cast<uint64_t>(removed), std::memory_order_relaxed);
        MPLOG_INFO("[ProofChurnGuard] Evicted " + std::to_string(removed) +
                   " stale TXs (age/attempt policy)");
    }

    return refresh_candidates;
}

bool Mempool::refreshProof(const uint256& txid, const std::vector<uint8_t>& new_root,
                            uint32_t new_height) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_transactions.find(txid);
    if (it == m_transactions.end()) return false;

    const bool was_stale = it->second.is_proof_stale;
    it->second.validated_at_root = new_root;
    it->second.validated_at_height = new_height;
    it->second.is_proof_stale = false;
    it->second.proof_refresh_attempts = 0;
    if (was_stale) {
        m_refresh_succeeded_total.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool Mempool::setCachedUtxoTxPayload(const uint256& txid, std::vector<uint8_t> payload) {
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_transactions.find(txid);
    if (it == m_transactions.end()) return false;
    it->second.cached_utxotx_payload = std::move(payload);
    return true;
}

std::optional<std::vector<uint8_t>> Mempool::getCachedUtxoTxPayload(const uint256& txid) const {
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    auto it = m_transactions.find(txid);
    if (it == m_transactions.end()) return std::nullopt;
    if (it->second.is_proof_stale) return std::nullopt;
    if (it->second.cached_utxotx_payload.empty()) return std::nullopt;
    return it->second.cached_utxotx_payload;
}

void Mempool::broadcastTransaction(const uint256& txid) {
    // Try callback first (P2PService wired via MempoolService)
    // Callback only needs txid - it will send INV and let peer request full tx
    if (m_tx_broadcast_callback) {
        MPLOG_INFO("Broadcasting transaction via callback: " + txid.GetHex());
        m_tx_broadcast_callback(txid);
        return;
    }

    MPLOG_WARN("Cannot broadcast transaction: no tx broadcast callback set");
}

bool Mempool::validateTransaction(
    const Transaction& tx,
    std::string& error,
    std::optional<uint32_t> target_height) const {
    const bool has_shielded_bundle = UsesShieldedValueSemantics(tx);

    // Basic transaction validation
    if (!Transaction::IsShieldedVersion(tx.version) &&
        !tx.shielded_bundle_bytes.empty()) {
        error = "Non-shielded transaction carries shielded bundle";
        return false;
    }

    if (Transaction::IsShieldedVersion(tx.version) &&
        tx.shielded_bundle_bytes.empty()) {
        error = "Shielded transaction missing shielded bundle";
        return false;
    }

    if (!has_shielded_bundle && tx.vin.empty()) {
        error = "Transaction has no inputs";
        return false;
    }

    if (!has_shielded_bundle && tx.vout.empty()) {
        error = "Transaction has no outputs";
        return false;
    }

    // Check transaction size
    size_t tx_size = tx.Serialize().size() / 2; // Hex string size / 2 = bytes
    if (tx_size > 100000) { // 100KB limit per transaction
        error = "Transaction too large: " + std::to_string(tx_size) + " bytes";
        return false;
    }

    if (tx.HasConfidentialOutputs()) {
        error = "legacy private lane removed";
        return false;
    }

    // Phase L0.4: Script verification with consensus flags
    // CRITICAL: Mempool MUST use same flags as block validation to prevent chain splits

    using namespace consensus;  // For ScriptExecutionContext, ScriptError, etc.

    // Helper struct for script validation
    struct InputUTXO {
        uint256 txid;
        uint32_t vout;
        std::vector<uint8_t> spk;  // scriptPubKey as bytes
        uint64_t value;
        bool is_confidential = false;
        std::vector<uint8_t> commitment;
    };

    // Collect all input UTXOs (needed for BIP341 Taproot sighash)
    std::vector<InputUTXO> input_utxos;
    input_utxos.reserve(tx.vin.size());
    uint64_t total_input_value = 0;

    // v0.11.0: Use MempoolUTXOView to see both confirmed + mempool UTXOs
    MempoolUTXOView utxo_view(&coins_view_, chain_db_, &m_transactions);

    for (size_t i = 0; i < tx.vin.size(); i++) {
        const auto& input = tx.vin[i];

        // Look up UTXO in either confirmed set or mempool
        InputUTXO utxo;
        utxo.txid = input.prevout.txid.AsUint256();
        utxo.vout = input.prevout.vout;

        // Frozen pre-base records require a live-forest authorization before
        // consulting the auxiliary UTXO map. Stateless forward-connect can
        // leave already-spent pre-base rows in that map; accepting one here
        // would bypass the forest gate and admit a double spend.
        std::string spk_str;
        OutPoint outpoint{input.prevout.txid, input.prevout.vout};
        const bool is_frozen_prebase =
            prebase_coin_predicate_ && prebase_coin_predicate_(outpoint);
        if (is_frozen_prebase) {
            const auto prebase = prebase_coin_resolver_
                ? prebase_coin_resolver_(outpoint)
                : std::nullopt;
            if (!prebase.has_value()) {
                error = "Input UTXO not found: " +
                        input.prevout.txid.AsUint256().GetHex() + ":" +
                        std::to_string(input.prevout.vout);
                return false;
            }
            utxo.value = prebase->value.GetUna();
            spk_str.assign(prebase->scriptPubKey.begin(), prebase->scriptPubKey.end());
            utxo.is_confidential = prebase->is_confidential;
            utxo.commitment = prebase->commitment;
        } else if (!utxo_view.GetUTXO(utxo.txid, utxo.vout, utxo.value, spk_str)) {
            if (auto recovered = recoverConflictedInputUTXO(outpoint)) {
                utxo.value = recovered->value.GetUna();
                spk_str.assign(recovered->scriptPubKey.begin(), recovered->scriptPubKey.end());
                utxo.is_confidential = recovered->is_confidential;
                utxo.commitment = recovered->commitment;
            } else if (prebase_coin_resolver_) {
                const auto prebase = prebase_coin_resolver_(outpoint);
                if (!prebase.has_value()) {
                    error = "Input UTXO not found: " + input.prevout.txid.AsUint256().GetHex() + ":" + std::to_string(input.prevout.vout);
                    return false;
                }
                utxo.value = prebase->value.GetUna();
                spk_str.assign(prebase->scriptPubKey.begin(), prebase->scriptPubKey.end());
                utxo.is_confidential = prebase->is_confidential;
                utxo.commitment = prebase->commitment;
            } else {
                error = "Input UTXO not found: " + input.prevout.txid.AsUint256().GetHex() + ":" + std::to_string(input.prevout.vout);
                return false;
            }
        } else {
            std::vector<uint8_t> commitment;
            bool is_confidential = false;
            if (utxo_view.GetConfidentialUTXO(utxo.txid, utxo.vout, commitment, is_confidential) &&
                is_confidential) {
                error = "legacy private lane removed";
                return false;
            }
        }

        // Convert string to vector<uint8_t> for Script
        utxo.spk.assign(spk_str.begin(), spk_str.end());
        total_input_value += utxo.value;
        input_utxos.push_back(utxo);
    }

    // Build execution context with all input amounts and scriptPubKeys (for BIP341)
    std::vector<uint64_t> all_amounts;
    std::vector<std::vector<uint8_t>> all_scriptpubkeys;
    std::vector<uint8_t> all_confidential_flags;
    std::vector<std::vector<uint8_t>> all_input_commitments;

    for (const auto& utxo : input_utxos) {
        all_amounts.push_back(utxo.value);
        all_scriptpubkeys.push_back(utxo.spk);
        all_confidential_flags.push_back(utxo.is_confidential ? 1 : 0);
        all_input_commitments.push_back(utxo.commitment);
    }

    if (std::any_of(all_confidential_flags.begin(), all_confidential_flags.end(),
                    [](uint8_t flag) { return flag != 0; })) {
        error = "legacy private lane removed";
        return false;
    }

    // Phase 6 Commit 4: unified consensus script validation.
    //
    // Per architectural rule: there must be exactly ONE script validation
    // logic in consensus. The block validator runs every spend through
    // dinero::consensus::ValidateSpend (explicit per-script-type dispatch
    // in src/consensus/script_validation.cpp); the mempool now does the
    // same. Previously the mempool ran the opcode-based VerifyScript path
    // which diverged from ValidateSpend (e.g. it didn't recognize witness-
    // v3 P2MR outputs) — any such divergence is a potential chain-split
    // surface.
    //
    // Height sourcing: scripts are validated against the height of the
    // block the tx would enter, which is tip+1. If getTip() fails, reject
    // outright — don't silently fake a height, that would bypass
    // height-gated rules like PQSchemeRegistry activation.
    uint32_t next_block_height_for_scripts = 0;
    if (target_height.has_value()) {
        next_block_height_for_scripts = *target_height;
    } else {
        if (!chain_db_) {
            error = "Script validation unavailable: ChainDB not initialized";
            return false;
        }
        auto tip_result = chain_db_->getTip();
        if (tip_result.status() != Status::Ok) {
            error = "Script validation unavailable: ChainDB tip not loaded";
            return false;
        }
        next_block_height_for_scripts =
            static_cast<uint32_t>(tip_result.value().height) + 1;
    }

    // Consensus must retain each extension's pre-activation NOP/OP_SUCCESS
    // behavior for historical validation. Relay/mining policy must not: a
    // transaction signed under active covenant rules could otherwise become
    // anyone-can-spend after a deep reorg below an independently configured
    // opcode boundary.
    if (!policy::IsCovenantRelayStandard(
            tx,
            all_scriptpubkeys,
            next_block_height_for_scripts,
            dinero::Params(),
            &error)) {
        return false;
    }

    // Build the consensus::UTXOEntry vector that ValidateSpend consumes.
    // ValidateSpend reads scriptPubKey, value, is_confidential, commitment
    // from each entry; it does NOT use height/isCoinbase for script
    // verification (coinbase maturity is enforced in checkDependencies).
    std::vector<consensus::UTXOEntry> utxo_entries;
    utxo_entries.reserve(input_utxos.size());
    for (const auto& u : input_utxos) {
        consensus::UTXOEntry e;
        e.value          = AmountUna::Una(u.value);
        e.scriptPubKey   = u.spk;
        e.is_confidential = u.is_confidential;
        e.commitment     = u.commitment;
        e.height         = 0;      // unused by script validation
        e.isCoinbase     = false;  // unused by script validation
        utxo_entries.push_back(std::move(e));
    }

    const consensus::PrecomputedTransactionData
        covenant_precomputed(tx, utxo_entries);
    for (size_t i = 0; i < tx.vin.size(); i++) {
        const consensus::ScriptValidationResult result = consensus::ValidateSpend(
            tx, i, utxo_entries[i], next_block_height_for_scripts,
            utxo_entries, &covenant_precomputed);

        if (result != consensus::ScriptValidationResult::OK) {
            const char* reason = "unknown";
            switch (result) {
                case consensus::ScriptValidationResult::INVALID_SIGNATURE:
                    reason = "invalid signature";               break;
                case consensus::ScriptValidationResult::INVALID_SCRIPT:
                    reason = "invalid script format";           break;
                case consensus::ScriptValidationResult::UNSUPPORTED_SCRIPT:
                    reason = "unsupported script type";         break;
                case consensus::ScriptValidationResult::EXTRACT_FAILED:
                    reason = "failed to extract sig/pubkey";    break;
                default: break;
            }
            error = "Script validation failed for input " + std::to_string(i) + ": " + reason;
            return false;
        }
    }

    if (has_shielded_bundle) {
        if (!shielded_tree_ || !shielded_nullifiers_) {
            error = "Shielded state unavailable in mempool";
            return false;
        }
        if (!chain_db_) {
            error = "Shielded validation unavailable: ChainDB not initialized";
            return false;
        }

        auto tip_result = chain_db_->getTip();
        if (tip_result.status() != Status::Ok) {
            error = "Shielded validation unavailable: ChainDB tip not loaded";
            return false;
        }
        const uint32_t next_block_height =
            target_height.value_or(
                static_cast<uint32_t>(tip_result.value().height) + 1);

        consensus::shielded::ShieldedBundle bundle;
        auto decode = consensus::shielded::DeserializeShieldedBundle(
            tx.shielded_bundle_bytes, &bundle);
        if (decode != consensus::shielded::BundleDecodeError::Ok) {
            error = "Shielded bundle decode failed (code " +
                    std::to_string(static_cast<int>(decode)) + ")";
            return false;
        }

        for (const auto& spend : bundle.spends) {
            for (const auto& [mempool_txid, entry] : m_transactions) {
                if (entry.tx.shielded_bundle_bytes.empty()) {
                    continue;
                }
                consensus::shielded::ShieldedBundle mempool_bundle;
                if (consensus::shielded::DeserializeShieldedBundle(
                        entry.tx.shielded_bundle_bytes, &mempool_bundle) !=
                    consensus::shielded::BundleDecodeError::Ok) {
                    continue;
                }
                for (const auto& mempool_spend : mempool_bundle.spends) {
                    if (mempool_spend.nullifier == spend.nullifier) {
                        error = "Shielded nullifier conflict with mempool transaction " +
                                mempool_txid.GetHex();
                        return false;
                    }
                }
            }
        }

        uint64_t total_output_value = 0;
        for (const auto& output : tx.vout) {
            total_output_value += output.value.GetUna();
        }
        uint64_t fee = 0;
        if (tx.HasExplicitFee()) {
            fee = tx.GetExplicitFee();
        } else if (total_output_value > total_input_value) {
            error = "Outputs exceed inputs (negative fee)";
            return false;
        } else {
            fee = total_input_value - total_output_value;
        }

        int64_t transparent_delta = 0;
        if (!ComputeTransparentValueDelta(total_input_value, total_output_value,
                                          fee, transparent_delta, error)) {
            return false;
        }

        // Single-helper construction; matches block_validation.cpp +
        // reindexer.cpp. anchor_history=nullptr is intentional here:
        // mempool only validates against the current tip's root, no
        // historical-anchor window. The block validator owns the
        // AnchorHistory.
        auto ctx = consensus::shielded::BuildShieldedValidationContext(
            tx,
            shielded_nullifiers_,
            shielded_tree_,
            next_block_height,
            transparent_delta,
            dinero::Params().shielded_activation_height,
            /*anchor_history=*/nullptr,
            dinero::Params().shielded_input_binding_activation_height,
            dinero::Params().shielded_cv_binding_activation_height,
            dinero::Params().shielded_spend_auth_activation_height);
        const auto validation = consensus::shielded::ValidateShieldedBundle(bundle, ctx);
        if (validation != consensus::shielded::ShieldedValidationError::Ok) {
            error = "Shielded validation failed: " +
                    std::string(ShieldedValidationErrorToString(validation));
            return false;
        }
    }

    return true;
}

uint64_t Mempool::computeVWUForTx(const Transaction& tx) const {
    // Coinbase / no-input shortcut: ComputeVWU returns stripped_size directly
    // and doesn't need prevouts. Hand it straight through.
    if (tx.IsCoinbase() || tx.vin.empty()) {
        auto vwu = consensus::ComputeVWU(tx, {});
        return vwu.value_or(0);
    }

    // Resolve prevouts via the same MempoolUTXOView the validator uses. If
    // any lookup fails we fall back to treating the tx as non-P2MR (implicit
    // byte_weight=1, verify_cost=0). That under-prices malformed P2MR txs
    // but they can't pass validation anyway; this keeps computeVWUForTx
    // deterministic and safe to call on any well-formed tx.
    MempoolUTXOView utxo_view(&coins_view_, chain_db_, &m_transactions);

    std::vector<consensus::UTXOEntry> prevouts;
    prevouts.reserve(tx.vin.size());
    for (const auto& input : tx.vin) {
        consensus::UTXOEntry pout;
        uint64_t amount = 0;
        std::string spk_str;
        const uint256 txid_u = input.prevout.txid.AsUint256();
        if (utxo_view.GetUTXO(txid_u, input.prevout.vout, amount, spk_str)) {
            pout.value = AmountUna::Una(amount);
            pout.scriptPubKey.assign(spk_str.begin(), spk_str.end());
        } else if (auto recovered = recoverConflictedInputUTXO(
                       OutPoint{input.prevout.txid, input.prevout.vout})) {
            pout = *recovered;
        } else {
            // Prevout unresolvable — keep scriptPubKey empty. ComputeVWU's
            // IsP2MRScript check returns false on empty, so the input is
            // priced as non-P2MR. Safe fallback.
        }
        prevouts.push_back(std::move(pout));
    }

    auto vwu = consensus::ComputeVWU(tx, prevouts);
    if (vwu) return *vwu;

    // ComputeVWU refused (malformed P2MR witness or Reserved scheme). Fall
    // back to an implicit-weight calculation so the mempool always has a
    // defined denominator. Validation will catch the real problem.
    uint64_t fallback = tx.GetBaseSize();
    for (const auto& input : tx.vin) {
        for (const auto& item : input.witness) {
            fallback += item.size();
        }
    }
    return fallback;
}

bool Mempool::checkDoubleSpend(const Transaction& tx) const {
    for (const auto& input : tx.vin) {
        OutPoint outpoint{input.prevout.txid, input.prevout.vout};
        if (m_spent_outputs.find(outpoint) != m_spent_outputs.end()) {
            return true; // Double spend detected
        }
    }
    return false;
}

bool Mempool::checkDependencies(const Transaction& tx) const {
    // Check if all input UTXOs are available (either in mempool or ChainDB)
    for (const auto& input : tx.vin) {
        // Phase M.1: OutPoint with uint256, getCoin returns StatusOr<UTXOEntry>
        OutPoint out{input.prevout.txid, input.prevout.vout};
        auto coin_result = coins_view_.getCoin(out);

        if (coin_result.status() != Status::Ok) {
            // UTXO not found in mempool or blockchain
            // Phase M.0: Convert uint256 to hex for logging
            MPLOG_DEBUG("Dependency check failed for " + tx.GetTxid().AsUint256().GetHex() +
                          " - UTXO " + out.ToString() +
                          " not found in mempool or blockchain");
            return false;
        }
    }
    return true;
}

uint64_t Mempool::calculateFee(const Transaction& tx) const {
    // Calculate input value by looking up UTXOs from CoinsViewMemPool overlay
    // This allows calculating fees for transactions that spend mempool outputs
    uint64_t input_value = 0;

    for (const auto& input : tx.vin) {
        // Phase M.1: OutPoint with uint256, getCoin returns StatusOr<UTXOEntry>
        OutPoint out{input.prevout.txid, input.prevout.vout};
        auto coin_result = coins_view_.getCoin(out);

        if (coin_result.status() != Status::Ok) {
            if (auto recovered = recoverConflictedInputUTXO(out)) {
                input_value += recovered->value.GetUna();
                MPLOG_DEBUG("Recovered conflicting input " + out.ToString() +
                            " for replacement fee calculation");
            } else if (prebase_coin_resolver_) {
                // Frozen pre-base coins are absent from the active UTXO view
                // by design. The resolver is live-forest-gated by
                // ChainstateService, so it rejects stale/spent auxiliary rows.
                const bool is_frozen_prebase =
                    !prebase_coin_predicate_ || prebase_coin_predicate_(out);
                const auto prebase = is_frozen_prebase
                    ? prebase_coin_resolver_(out)
                    : std::nullopt;
                if (!prebase.has_value()) {
                    MPLOG_WARN("UTXO not found for input: " + out.ToString() +
                               " - cannot calculate fee");
                    return 0;
                }
                input_value += prebase->value.GetUna();
                MPLOG_DEBUG("Resolved live pre-base input " + out.ToString() +
                            " for fee calculation: " +
                            std::to_string(prebase->value.GetUna()) + " una");
            } else {
                MPLOG_WARN("UTXO not found for input: " + out.ToString() + " - cannot calculate fee");
                return 0;
            }
            continue;
        }

        const consensus::UTXOEntry& utxo = coin_result.value();
        input_value += utxo.value.GetUna();  // AmountUna → uint64_t
        MPLOG_DEBUG("Input " + out.ToString() +
                   " value: " + std::to_string(utxo.value.GetUna()) + " una");
    }

    // Confidential / shielded transactions carry an explicit fee because
    // inputs and/or outputs no longer reveal the whole fee equation directly.
    if (tx.HasExplicitFee() &&
        (tx.HasConfidentialOutputs() || tx.IsShielded())) {
        uint64_t explicit_fee = tx.GetExplicitFee();
        MPLOG_DEBUG("Explicit-fee transaction " + tx.GetTxid().AsUint256().GetHex() +
                   " fee: " + std::to_string(explicit_fee) + " una");
        return explicit_fee;
    }

    // Calculate output value (transparent only)
    uint64_t output_value = 0;
    for (const auto& output : tx.vout) {
        output_value += output.value.GetUna();
    }

    // Fee = inputs - outputs
    if (input_value > output_value) {
        uint64_t fee = input_value - output_value;
        MPLOG_DEBUG("Transaction " + tx.GetTxid().AsUint256().GetHex() +" fee: " + std::to_string(fee) + " una");
        return fee;
    } else {
        MPLOG_WARN("Transaction " + tx.GetTxid().AsUint256().GetHex() +" has negative fee (inputs=" +
                  std::to_string(input_value) + ", outputs=" + std::to_string(output_value) + ")");
        return 0;
    }
}

std::optional<consensus::UTXOEntry> Mempool::recoverConflictedInputUTXO(const OutPoint& outpoint) const {
    if (m_spent_outputs.find(outpoint) == m_spent_outputs.end()) {
        return std::nullopt;
    }

    auto parent_it = m_transactions.find(outpoint.txid.AsUint256());
    if (parent_it != m_transactions.end() && outpoint.vout < parent_it->second.tx.vout.size()) {
        const auto& output = parent_it->second.tx.vout[outpoint.vout];
        consensus::UTXOEntry recovered;
        recovered.value = output.value;
        recovered.scriptPubKey = output.scriptPubKey;
        recovered.height = parent_it->second.height;
        recovered.isCoinbase = false;
        recovered.is_confidential = output.is_confidential;
        recovered.commitment = output.commitment;
        return recovered;
    }

    if (!chain_state_view_) {
        return std::nullopt;
    }

    auto chain_coin = chain_state_view_->getCoin(outpoint);
    if (chain_coin.status() != Status::Ok) {
        return std::nullopt;
    }

    return chain_coin.value();
}

void Mempool::updateDependencies(const uint256& removed_txid) {
    // Update children that depended on the removed transaction
    auto children_it = m_children_index.find(removed_txid);
    if (children_it == m_children_index.end()) return;

    for (const auto& child_txid : children_it->second) {
        auto child_it = m_transactions.find(child_txid);
        if (child_it == m_transactions.end()) continue;

        auto& child = child_it->second;

        // Remove parent from depends list
        child.depends.erase(
            std::remove(child.depends.begin(), child.depends.end(), removed_txid),
            child.depends.end()
        );

        // Recalculate ancestor metrics for CPFP
        const double old_score = GetPackageSelectionScore(child);
        recalcAncestorMetrics(child);
        EraseFeeIndexEntry(m_fee_index, child_txid, old_score);
        m_fee_index.insert({GetPackageSelectionScore(child), child_txid});
    }

    // Remove reverse index entry for removed tx
    m_children_index.erase(children_it);
}

void Mempool::recalcAncestorMetrics(MempoolEntry& entry) {
    uint64_t anc_fee = entry.fee;
    uint64_t anc_size = entry.tx_size;
    size_t anc_effective_vsize = GetEffectiveVirtualSize(entry);

    // BFS over remaining ancestors
    std::unordered_set<uint256> visited;
    std::vector<uint256> to_visit(entry.depends.begin(), entry.depends.end());

    while (!to_visit.empty()) {
        uint256 ancestor_txid = to_visit.back();
        to_visit.pop_back();

        if (visited.count(ancestor_txid)) continue;
        visited.insert(ancestor_txid);

        auto ancestor_it = m_transactions.find(ancestor_txid);
        if (ancestor_it == m_transactions.end()) continue;

        anc_fee += ancestor_it->second.fee;
        anc_size += ancestor_it->second.tx_size;
        anc_effective_vsize += GetEffectiveVirtualSize(ancestor_it->second);

        for (const auto& parent_txid : ancestor_it->second.depends) {
            if (!visited.count(parent_txid)) {
                to_visit.push_back(parent_txid);
            }
        }
    }

    entry.ancestor_fee = anc_fee;
    entry.ancestor_size = anc_size;
    entry.ancestor_feerate = (anc_size > 0) ? static_cast<double>(anc_fee) / anc_size : 0.0;
    entry.ancestor_effective_vsize = anc_effective_vsize;
    entry.ancestor_adjusted_feerate = anc_effective_vsize > 0
        ? static_cast<double>(anc_fee) / anc_effective_vsize
        : 0.0;
}

void Mempool::evictTransactions() {
    // Evict lowest fee rate transactions until under size limit
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    evictTransactionsLocked();
}

void Mempool::evictTransactionsLocked() {
    // Caller must hold m_mutex.
    bool removed_any = false;
    while (getTotalSizeLocked() > m_max_size && !m_fee_index.empty()) {
        // Remove lowest fee rate transaction
        auto lowest_fee_it = m_fee_index.begin();
        uint256 txid_to_remove = lowest_fee_it->second;

        MPLOG_DEBUG("Evicting low-fee transaction: " + txid_to_remove.GetHex());
        removed_any = removeTransactionLocked(txid_to_remove) || removed_any;
    }
    if (removed_any) {
        rebuildCoinsViewLocked();
    }
}

uint64_t Mempool::getTotalFeesLocked() const {
    uint64_t total = 0;
    for (const auto& pair : m_transactions) {
        total += pair.second.fee;
    }
    return total;
}

size_t Mempool::getTotalSizeLocked() const {
    size_t total = 0;
    for (const auto& pair : m_transactions) {
        total += pair.second.tx_size;
    }
    return total;
}

// ═══════════════════════════════════════════════════════════════════════════
// Mempool Persistence (v0.13.0.2 - STEP B/C)
// ═══════════════════════════════════════════════════════════════════════════

std::string Mempool::getDefaultMempoolPath() const {
    // If chain_db_ is available, use its data directory
    // Otherwise use current directory
    // Configuration-level path wiring is handled by the daemon layer.
    return "./mempool.dat";
}

bool Mempool::saveToDisk(const std::string& filepath) {
    // STEP B: Save mempool on shutdown
    // Rules:
    // - Best effort only (never throw)
    // - Never block shutdown forever
    // - Atomic write (.tmp → rename)

    try {
        std::lock_guard<std::shared_mutex> lock(m_mutex);

        // Convert m_transactions map to vector for MempoolPersistence
        std::vector<MempoolEntry> entries;
        entries.reserve(m_transactions.size());

        for (const auto& pair : m_transactions) {
            entries.push_back(pair.second);
        }

        MPLOG_INFO("Saving mempool with " + std::to_string(entries.size()) + " transactions");

        // Delegate to MempoolPersistence (defined in Step A)
        return MempoolPersistence::save(entries, filepath);

    } catch (const std::exception& e) {
        // Never throw - best effort only
        MPLOG_ERR("Exception saving mempool: " + std::string(e.what()));
        return false;
    }
}

bool Mempool::loadFromDisk(const std::string& filepath) {
    // STEP C: Load mempool on startup
    // Rules:
    // - Deserialize each transaction
    // - Revalidate against current policy
    // - Drop if expired/conflicts/invalid
    // - Persistence does NOT bypass policy

    try {
        MPLOG_INFO("Loading mempool from " + filepath);

        // Delegate to MempoolPersistence::load()
        std::vector<MempoolPersistence::PersistedEntry> persisted = MempoolPersistence::load(filepath);

        if (persisted.empty()) {
            MPLOG_INFO("No transactions loaded (empty file or error)");
            return true;  // Not an error - empty mempool is valid
        }

        size_t loaded = 0;
        size_t rejected = 0;

        struct PendingReloadTx {
            Transaction tx;
            std::string txid_hex;
        };

        std::vector<PendingReloadTx> pending;
        pending.reserve(persisted.size());

        // Deserialize first so we can replay persisted transactions in
        // dependency-safe passes instead of assuming file order is valid.
        for (const auto& entry : persisted) {
            Transaction tx;
            if (!TransactionSerializer::Deserialize(tx, entry.tx_bytes)) {
                rejected++;
                MPLOG_WARN("Failed to deserialize transaction from mempool file");
                continue;
            }
            pending.push_back(PendingReloadTx{tx, tx.GetTxid().AsUint256().GetHex()});
        }

        size_t pass = 0;
        while (!pending.empty()) {
            pass++;
            size_t loaded_this_pass = 0;
            std::vector<PendingReloadTx> retry;
            retry.reserve(pending.size());

            for (auto& pending_tx : pending) {
                // Revalidate via canonical ingress. false = don't relay because
                // this is restart recovery, not new network admission.
                auto result = submitTransactionInternal(pending_tx.tx, "disk-load", false);
                if (result.accepted()) {
                    loaded++;
                    loaded_this_pass++;
                } else {
                    retry.push_back(std::move(pending_tx));
                }
            }

            if (retry.empty()) {
                break;
            }

            if (loaded_this_pass == 0) {
                rejected += retry.size();
                for (const auto& pending_tx : retry) {
                    MPLOG_DEBUG("Rejected transaction " + pending_tx.txid_hex +
                                " during mempool load (policy/expired/conflict)");
                }
                break;
            }

            MPLOG_DEBUG("Mempool disk-load pass " + std::to_string(pass) +
                        ": loaded " + std::to_string(loaded_this_pass) +
                        ", retrying " + std::to_string(retry.size()) + " dependent transaction(s)");
            pending = std::move(retry);
        }

        MPLOG_INFO("Loaded " + std::to_string(loaded) + " transactions, rejected " + std::to_string(rejected));
        return true;

    } catch (const std::exception& e) {
        // Never throw - return false on failure
        MPLOG_ERR("Exception loading mempool: " + std::string(e.what()));
        return false;
    }
}

// ============================================================================
// Reorg Reconciliation
// ============================================================================

size_t Mempool::ReconcileAfterReorg(
    const std::vector<Transaction>& disconnected_txs,
    const std::vector<Transaction>& connected_txs
) {
    MPLOG_INFO("Reconciling mempool after reorg: " +
              std::to_string(disconnected_txs.size()) + " disconnected, " +
              std::to_string(connected_txs.size()) + " connected");

    // Build set of txids in connected blocks (to filter out)
    std::unordered_set<uint256> connected_txids;
    for (const auto& tx : connected_txs) {
        connected_txids.insert(tx.GetTxid().AsUint256());
    }

    // Statistics
    size_t restored_count = 0;
    size_t skipped_connected = 0;
    size_t skipped_coinbase = 0;
    size_t skipped_already_in_mempool = 0;
    size_t validation_failed = 0;

    for (const auto& tx : disconnected_txs) {
        uint256 txid = tx.GetTxid().AsUint256();

        // Skip coinbase transactions (can't go back to mempool)
        if (tx.IsCoinbase()) {
            skipped_coinbase++;
            continue;
        }

        // Skip if transaction is in the new connected chain
        if (connected_txids.count(txid) > 0) {
            skipped_connected++;
            continue;
        }

        // Skip if already in mempool
        if (hasTransaction(txid)) {
            skipped_already_in_mempool++;
            continue;
        }

        // Re-validate and add to mempool
        // false = don't relay (these are not new transactions)
        auto result = submitTransactionInternal(tx, "reorg-restore", false);
        if (result.accepted()) {
            restored_count++;
        } else {
            validation_failed++;
            MPLOG_DEBUG("Reorg restore failed for " + txid.GetHex() +
                       ": " + result.message);
        }
    }

    // Log reconciliation summary
    MPLOG_INFO("Mempool reorg reconciliation complete:");
    MPLOG_INFO("  Restored: " + std::to_string(restored_count));
    MPLOG_INFO("  Skipped (in new chain): " + std::to_string(skipped_connected));
    MPLOG_INFO("  Skipped (coinbase): " + std::to_string(skipped_coinbase));
    MPLOG_INFO("  Skipped (already in mempool): " + std::to_string(skipped_already_in_mempool));
    MPLOG_INFO("  Validation failed: " + std::to_string(validation_failed));

    return restored_count;
}

} // namespace dinero
