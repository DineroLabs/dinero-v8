/**
 * Phase 29: Covenant Wallet Integration Layer
 *
 * Provides wallet-level support for covenant scripts:
 * - CTV template tracking and spending
 * - CSFS delegated signing
 * - TXHASH introspection constraints
 * - Contract state management (CCV)
 * - Covenant metadata persistence
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * CRITICAL BOUNDARY RULE (Phase C.1)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * This wallet layer ONLY constructs covenant transactions.
 * It NEVER validates covenant consensus rules.
 *
 * Validation happens EXCLUSIVELY in consensus::ScriptInterpreter.
 *
 * Allowed wallet operations:
 *   ✅ createCTVTemplate()     - Build templates, compute hashes
 *   ✅ createCTVSpendingTx()   - Construct transactions matching templates
 *   ✅ estimateCovenantFee()   - Fee estimation
 *   ✅ analyzeScript()         - Script introspection for UX
 *   ✅ getSpendInfo()          - What data is needed to spend
 *
 * Forbidden wallet operations:
 *   ❌ validateCovenantTx()    - Validate covenant consensus rules
 *   ❌ Call consensus::VerifyCTV() or consensus::ComputeCTVHash() for validation
 *   ❌ Return "valid/invalid" based on covenant rules
 *   ❌ Duplicate consensus validation logic
 *
 * Rationale: Consensus validation MUST have a single source of truth.
 * Wallet validation would create a second implementation that can diverge,
 * leading to wallet accepting transactions that consensus rejects.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 */

#pragma once

#include "consensus/covenants.h"
#include "wallet/transaction.h"
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>
#include <array>
#include <memory>
#include <sqlite3.h>

namespace dinero {
namespace wallet {

// ============================================================================
// Covenant Type Enumeration
// ============================================================================

enum class CovenantType : uint8_t {
    NONE = 0,           // Not a covenant output
    CTV = 1,            // CheckTemplateVerify (BIP-119)
    CSFS = 2,           // CheckSigFromStack
    TXHASH = 3,         // Transaction introspection
    CCV = 4,            // CheckContractVerify (stateful)
    VAULT = 5,          // Time-locked vault pattern
    CHANNEL = 6,        // Payment channel
    COMPOSITE = 7       // Multiple covenant types combined
};

// ============================================================================
// CTV Template - Committed Transaction Template
// ============================================================================

/**
 * @brief CTV Template for BIP-119 covenant outputs
 *
 * Stores the committed template that constrains how this output can be spent.
 * The template hash commits to outputs, locktime, version, and sequences.
 */
struct CTVTemplate {
    std::string template_id;                    // Unique identifier
    std::array<uint8_t, 32> template_hash;      // BIP-119 template hash

    // Committed transaction structure
    int32_t version;                            // Transaction version
    uint32_t locktime;                          // Transaction locktime
    std::vector<uint32_t> input_sequences;      // Required input sequences

    // Committed outputs
    struct CommittedOutput {
        uint64_t value;                         // Output value in una
        std::vector<uint8_t> script_pubkey;     // Output scriptPubKey
        std::string address;                    // Decoded address (if known)
    };
    std::vector<CommittedOutput> outputs;

    // Metadata
    std::string label;                          // User-friendly label
    uint64_t created_at;                        // Creation timestamp
    bool is_spent;                              // Has this template been satisfied?
    std::string spending_txid;                  // TXID that spent this covenant

    // Helpers
    std::string toJSON() const;
    static CTVTemplate fromJSON(const std::string& json);
    std::vector<uint8_t> serialize() const;
    static CTVTemplate deserialize(const std::vector<uint8_t>& data);
};

// ============================================================================
// CSFS Delegation - Signature From Stack Parameters
// ============================================================================

/**
 * @brief CSFS delegation parameters for off-chain signing
 *
 * Allows signing arbitrary messages that can be verified on-chain.
 * Useful for oracle attestations, delegation, and state channels.
 */
struct CSFSDelegation {
    std::string delegation_id;                  // Unique identifier
    std::vector<uint8_t> pubkey;                // 32-byte x-only pubkey
    std::vector<uint8_t> message;               // Message to be signed
    std::vector<uint8_t> signature;             // 64-byte Schnorr signature (if signed)

    // Delegation metadata
    std::string purpose;                        // "oracle", "delegation", "channel", etc.
    std::string label;                          // User label
    uint64_t created_at;
    uint64_t expires_at;                        // 0 = no expiration
    bool is_signed;
    bool is_used;                               // Has this delegation been used?

    std::string toJSON() const;
    static CSFSDelegation fromJSON(const std::string& json);
};

// ============================================================================
// Contract Instance - Stateful CCV Contract
// ============================================================================

/**
 * @brief Stateful contract instance for CCV opcodes
 *
 * Tracks contract state transitions for on-chain state machines.
 */
struct ContractInstance {
    std::string contract_id;                    // Unique identifier
    std::array<uint8_t, 32> code_hash;          // Immutable contract code hash
    uint32_t state_counter;                     // Current state counter
    std::vector<uint8_t> state_data;            // Current state data
    std::array<uint8_t, 32> state_hash;         // Computed state hash

    // UTXO binding
    std::string current_txid;                   // TXID holding current state
    uint32_t current_vout;                      // Output index
    uint64_t locked_value;                      // Value locked in contract

    // History
    struct StateTransition {
        uint32_t from_counter;
        uint32_t to_counter;
        std::string txid;
        uint64_t timestamp;
        std::vector<uint8_t> old_data;
        std::vector<uint8_t> new_data;
    };
    std::vector<StateTransition> history;

    // Metadata
    std::string label;
    std::string contract_type;                  // "escrow", "vault", "auction", etc.
    uint64_t created_at;
    bool is_active;

    std::string toJSON() const;
    static ContractInstance fromJSON(const std::string& json);
};

// ============================================================================
// Covenant UTXO - Extended UTXO with Covenant Metadata
// ============================================================================

/**
 * @brief Extended UTXO tracking for covenant outputs
 *
 * Adds covenant-specific metadata to standard UTXO tracking.
 */
struct CovenantUTXO {
    // Standard UTXO fields
    std::string txid;
    uint32_t vout;
    uint64_t value;
    std::vector<uint8_t> script_pubkey;
    uint32_t height;
    bool is_spent;

    // Covenant fields
    CovenantType covenant_type;
    std::string covenant_id;                    // Reference to template/delegation/contract

    // CTV specific
    std::optional<std::array<uint8_t, 32>> ctv_hash;

    // CSFS specific
    std::optional<std::vector<uint8_t>> required_pubkey;

    // Contract specific
    std::optional<std::array<uint8_t, 32>> contract_state_hash;

    // Spending constraints
    bool requires_template_match;               // CTV: must match template
    bool requires_signature;                    // CSFS: needs off-chain signature
    bool requires_state_transition;             // CCV: must advance state

    // Fee estimation hints
    uint32_t estimated_witness_size;            // For fee calculation
    uint32_t spending_tx_outputs;               // Number of required outputs

    std::string toJSON() const;
    static CovenantUTXO fromJSON(const std::string& json);
};

// ============================================================================
// Covenant Spending Info - How to Spend a Covenant Output
// ============================================================================

/**
 * @brief Information needed to spend a covenant-constrained output
 */
struct CovenantSpendInfo {
    std::string utxo_txid;
    uint32_t utxo_vout;
    CovenantType type;

    // Required transaction structure (CTV)
    std::optional<Transaction> required_tx_template;

    // Required signature data (CSFS)
    std::optional<std::vector<uint8_t>> required_message;
    std::optional<std::vector<uint8_t>> required_signature;

    // Required state transition (CCV)
    std::optional<consensus::ContractState> required_new_state;

    // Validation
    bool canSpend() const;                      // Check if all requirements met
    std::string getMissingRequirement() const;  // What's needed to spend

    std::string toJSON() const;
};

// ============================================================================
// Covenant Fee Estimation
// ============================================================================

/**
 * @brief Fee estimation for covenant transactions
 */
struct CovenantFeeEstimate {
    uint64_t base_fee;                          // Base transaction fee
    uint64_t witness_fee;                       // Additional witness data fee
    uint64_t total_fee;                         // Total estimated fee

    uint32_t tx_vsize;                          // Virtual size
    uint32_t tx_weight;                         // Weight units

    // Breakdown
    uint32_t input_count;
    uint32_t output_count;
    uint32_t witness_vbytes;

    // Fee rate used
    uint64_t fee_rate_una_vb;                   // Una per vbyte
};

// ============================================================================
// Covenant Wallet Manager
// ============================================================================

/**
 * @brief Main covenant wallet integration class
 *
 * Manages covenant UTXOs, templates, delegations, and contracts.
 * Provides methods for creating and spending covenant outputs.
 */
class CovenantWallet {
public:
    explicit CovenantWallet(const std::string& wallet_db_path);
    ~CovenantWallet();

    // ─────────────────────────────────────────────────────────────────────────
    // Initialization
    // ─────────────────────────────────────────────────────────────────────────

    bool initialize();
    void shutdown();
    bool isInitialized() const { return initialized_; }

    // ─────────────────────────────────────────────────────────────────────────
    // CTV Template Management
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Create a new CTV template
     * @param outputs Required outputs for the template
     * @param locktime Transaction locktime
     * @param label User label
     * @return Template ID
     */
    std::string createCTVTemplate(
        const std::vector<CTVTemplate::CommittedOutput>& outputs,
        uint32_t locktime = 0,
        const std::string& label = "");

    /**
     * @brief Get CTV template by ID
     */
    std::optional<CTVTemplate> getCTVTemplate(const std::string& template_id) const;

    /**
     * @brief Get CTV template by hash
     */
    std::optional<CTVTemplate> getCTVTemplateByHash(
        const std::array<uint8_t, 32>& hash) const;

    /**
     * @brief List all CTV templates
     */
    std::vector<CTVTemplate> listCTVTemplates(bool include_spent = false) const;

    /**
     * @brief Generate script for CTV output
     * @param template_hash The 32-byte CTV hash
     * @return P2WSH or P2TR script containing CTV
     */
    std::vector<uint8_t> generateCTVScript(
        const std::array<uint8_t, 32>& template_hash) const;

    /**
     * @brief Create transaction to fund a CTV template
     */
    Transaction createCTVFundingTx(
        const std::string& template_id,
        uint64_t total_value,
        const std::vector<CovenantUTXO>& funding_utxos);

    /**
     * @brief Create transaction that spends a CTV output
     */
    Transaction createCTVSpendingTx(
        const std::string& template_id,
        uint32_t input_index = 0);

    // ─────────────────────────────────────────────────────────────────────────
    // CSFS Delegation Management
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Create a CSFS delegation request
     */
    std::string createCSFSDelegation(
        const std::vector<uint8_t>& pubkey,
        const std::vector<uint8_t>& message,
        const std::string& purpose = "delegation",
        uint64_t expires_at = 0);

    /**
     * @brief Sign a CSFS delegation (requires wallet unlock)
     */
    bool signCSFSDelegation(
        const std::string& delegation_id,
        const std::vector<uint8_t>& private_key);

    /**
     * @brief Add an external signature to delegation
     */
    bool addCSFSSignature(
        const std::string& delegation_id,
        const std::vector<uint8_t>& signature);

    /**
     * @brief Get delegation by ID
     */
    std::optional<CSFSDelegation> getCSFSDelegation(
        const std::string& delegation_id) const;

    /**
     * @brief List all delegations
     */
    std::vector<CSFSDelegation> listCSFSDelegations(
        bool include_used = false,
        bool include_expired = false) const;

    // ─────────────────────────────────────────────────────────────────────────
    // Contract Management (CCV)
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Register a new contract instance
     */
    std::string registerContract(
        const std::vector<uint8_t>& code,
        const std::vector<uint8_t>& initial_data,
        const std::string& contract_type,
        const std::string& label = "");

    /**
     * @brief Get contract by ID
     */
    std::optional<ContractInstance> getContract(
        const std::string& contract_id) const;

    /**
     * @brief List all contracts
     */
    std::vector<ContractInstance> listContracts(bool include_inactive = false) const;

    /**
     * @brief Advance contract state
     */
    bool advanceContractState(
        const std::string& contract_id,
        const std::vector<uint8_t>& new_data,
        const Transaction& state_transition_tx);

    /**
     * @brief Get contract state transition requirements
     */
    CovenantSpendInfo getContractSpendInfo(
        const std::string& contract_id) const;

    // ─────────────────────────────────────────────────────────────────────────
    // Covenant UTXO Tracking
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Add a covenant UTXO to tracking
     */
    bool addCovenantUTXO(const CovenantUTXO& utxo);

    /**
     * @brief Get covenant UTXO
     */
    std::optional<CovenantUTXO> getCovenantUTXO(
        const std::string& txid, uint32_t vout) const;

    /**
     * @brief List all covenant UTXOs
     */
    std::vector<CovenantUTXO> listCovenantUTXOs(
        CovenantType type = CovenantType::NONE,
        bool include_spent = false) const;

    /**
     * @brief Mark covenant UTXO as spent
     */
    bool markCovenantSpent(
        const std::string& txid, uint32_t vout,
        const std::string& spending_txid);

    /**
     * @brief Get spending info for covenant UTXO
     */
    CovenantSpendInfo getSpendInfo(
        const std::string& txid, uint32_t vout) const;

    /**
     * @brief Detect covenant type from scriptPubKey
     */
    static CovenantType detectCovenantType(
        const std::vector<uint8_t>& script_pubkey);

    // ─────────────────────────────────────────────────────────────────────────
    // Fee Estimation
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Estimate fee for spending a covenant output
     */
    CovenantFeeEstimate estimateCovenantFee(
        const std::string& txid, uint32_t vout,
        uint64_t fee_rate_una_vb = 1) const;

    /**
     * @brief Estimate fee for CTV template spending
     */
    CovenantFeeEstimate estimateCTVFee(
        const std::string& template_id,
        uint64_t fee_rate_una_vb = 1) const;

    // ─────────────────────────────────────────────────────────────────────────
    // Debugging & Analysis
    // ─────────────────────────────────────────────────────────────────────────

    /**
     * @brief Analyze a script for covenant constraints
     */
    struct ScriptAnalysis {
        CovenantType type;
        bool has_ctv;
        bool has_csfs;
        bool has_txhash;
        bool has_ccv;
        std::optional<std::array<uint8_t, 32>> ctv_hash;
        std::optional<std::vector<uint8_t>> csfs_pubkey;
        std::vector<std::string> warnings;
        std::string human_readable;
    };
    static ScriptAnalysis analyzeScript(const std::vector<uint8_t>& script);

    /**
     * @brief Decode CTV hash to template info (if known)
     */
    std::optional<CTVTemplate> decodeCTVHash(
        const std::array<uint8_t, 32>& hash) const;

    // ─────────────────────────────────────────────────────────────────────────
    // REMOVED: validateCovenantTx() - Phase C.1 Boundary Violation Fix
    // ─────────────────────────────────────────────────────────────────────────
    //
    // The validateCovenantTx() method was removed because it violated the
    // consensus/wallet boundary by calling consensus::ComputeCTVHash() to
    // validate covenant rules. This is forbidden.
    //
    // Validation happens ONLY in consensus::ScriptInterpreter.
    // Wallet must NEVER return "valid/invalid" based on covenant rules.
    //
    // For UX purposes, use:
    //   - analyzeScript() to detect covenant types
    //   - getSpendInfo() to check if spending requirements are met
    //   - createCTVSpendingTx() to build transactions (consensus will validate)
    // ─────────────────────────────────────────────────────────────────────────

    // ─────────────────────────────────────────────────────────────────────────
    // Statistics
    // ─────────────────────────────────────────────────────────────────────────

    struct CovenantStats {
        uint64_t total_ctv_templates;
        uint64_t active_ctv_templates;
        uint64_t total_delegations;
        uint64_t signed_delegations;
        uint64_t total_contracts;
        uint64_t active_contracts;
        uint64_t covenant_utxos;
        uint64_t covenant_value_locked;
    };
    CovenantStats getStats() const;

private:
    sqlite3* db_;
    std::string db_path_;
    bool initialized_;

    // Prepared statements
    sqlite3_stmt* stmt_insert_template_;
    sqlite3_stmt* stmt_get_template_;
    sqlite3_stmt* stmt_insert_delegation_;
    sqlite3_stmt* stmt_insert_contract_;
    sqlite3_stmt* stmt_insert_covenant_utxo_;

    // Schema management
    bool createSchema();
    bool migrateSchema(int from_version, int to_version);

    // Helper methods
    std::string generateUUID() const;
    bool prepareStatements();
    void finalizeStatements();
};

// ============================================================================
// Covenant Transaction Builder
// ============================================================================

/**
 * @brief Builder for covenant-constrained transactions
 */
class CovenantTxBuilder {
public:
    CovenantTxBuilder();

    // CTV building
    CovenantTxBuilder& withCTVTemplate(const CTVTemplate& tmpl);
    CovenantTxBuilder& withCTVHash(const std::array<uint8_t, 32>& hash);

    // CSFS building
    CovenantTxBuilder& withCSFSSignature(
        const std::vector<uint8_t>& pubkey,
        const std::vector<uint8_t>& message,
        const std::vector<uint8_t>& signature);

    // Contract building
    CovenantTxBuilder& withContractTransition(
        const ContractInstance& contract,
        const std::vector<uint8_t>& new_state_data);

    // Standard transaction fields
    CovenantTxBuilder& setVersion(int32_t version);
    CovenantTxBuilder& setLocktime(uint32_t locktime);
    CovenantTxBuilder& addInput(const std::string& txid, uint32_t vout,
                                 uint32_t sequence = 0xfffffffe);
    CovenantTxBuilder& addOutput(uint64_t value,
                                  const std::vector<uint8_t>& script_pubkey);
    CovenantTxBuilder& addOutput(uint64_t value, const std::string& address);

    // Build
    Transaction build() const;
    std::vector<uint8_t> buildRaw() const;

    // Validation
    bool validate() const;
    std::vector<std::string> getErrors() const;

private:
    Transaction tx_;
    std::optional<CTVTemplate> ctv_template_;
    std::vector<std::tuple<std::vector<uint8_t>, std::vector<uint8_t>, std::vector<uint8_t>>> csfs_signatures_;
    std::optional<ContractInstance> contract_;
    std::optional<std::vector<uint8_t>> new_contract_state_;
    mutable std::vector<std::string> errors_;
};

} // namespace wallet
} // namespace dinero
