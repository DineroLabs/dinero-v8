/**
 * Phase G.3.3: Consensus Validation (Expensive, Deterministic)
 *
 * Pure consensus evaluation - script execution, signature checks, but NO state mutation.
 *
 * Design Principles:
 * - Purely functional (read-only UTXO snapshot)
 * - Deterministic (same input → same result forever)
 * - NO side effects
 * - NO chainstate writes
 * - NO UTXO set updates
 * - NO mempool insertion
 * - NO disk I/O
 *
 * What This IS:
 * - Script execution
 * - Signature verification
 * - Locktime/sequence checks
 * - Coinbase rules
 * - UTXO spending validation
 *
 * What This Is NOT:
 * - State mutation
 * - Fork-choice
 * - Mempool admission
 * - Disk writes
 * - Peer announcements
 */

//=============================================================================
// ⚠️ ARCHITECTURAL GUARDRAIL — DO NOT VIOLATE ⚠️
//=============================================================================
//
// THIS MODULE MUST REMAIN PURE AND READ-ONLY.
//
// This module answers ONLY ONE QUESTION:
//   "Given a hypothetical UTXO snapshot and consensus rules, is this object valid?"
//
// This module MUST NEVER answer:
//   ❌ "Should we accept this into mempool?"
//   ❌ "Does this conflict with the current chain?"
//   ❌ "Should this be mined?"
//   ❌ "Which fork is better?"
//   ❌ "Should we update anything?"
//
// ANY STATE MUTATION BELONGS TO PHASE G.3.4 (State Transition Layer).
//
// If you are tempted to add ANY of the following to this module, STOP:
//   ❌ Writing UTXOs
//   ❌ Marking inputs spent
//   ❌ Updating balances
//   ❌ Disk persistence
//   ❌ Reorg logic
//   ❌ Chainwork comparison
//   ❌ Fork selection
//   ❌ Mempool conflict checks
//   ❌ Network announcements
//   ❌ Block index updates
//
// The separation between G.3.3 (pure evaluation) and G.3.4 (state transition)
// is a CRITICAL architectural boundary. Bitcoin Core maintains this separation
// in CheckTransaction()/CheckInputs() vs ConnectBlock().
//
// Violating this boundary creates:
//   - Non-deterministic validation
//   - Reorg bugs
//   - Mempool inconsistencies
//   - Consensus failures
//
// This module is FROZEN as of Phase G.3.3 completion.
// Extensions require explicit architectural review.
//
//=============================================================================
//
// ⚠️ REQUIRED EXTENSION (Identified in G.3.4 Design Review)
//
// BEFORE G.3.4 (State Transition) can be implemented, this module MUST add:
//
//   ✅ Coinbase subsidy validation
//      - validateBlock() checks: coinbase_value <= GetBlockSubsidy(height) + fees
//      - This is a consensus rule and MUST be validated in G.3.3 (pure evaluation)
//      - NOT in G.3.4 (state mutation)
//
// This extension is approved under architectural review (2024-12-17).
// It maintains the pure evaluation contract (read-only, deterministic).
//
// Implementation status:
//   ✅ API designed (BlockValidationResult, TxValidationResult, validateBlock)
//   ✅ Tests written (test_subsidy_validation.cpp - 8 comprehensive tests)
//   ❌ Implementation NOT YET ADDED
//
// Required before: G.3.4 authorization
// Next step: Implement validateBlock() and validateTxExtended() to make tests pass
//
//=============================================================================

#pragma once

#include "consensus/outpoint.h"  // Phase M.0: Single canonical OutPoint
#include "inventory.h"
#include <vector>
#include <string>
#include <map>
#include <optional>
#include <cstdint>

namespace dinero {
namespace p2p {

//=============================================================================
// Transaction Types
//=============================================================================

// Phase M.0: OutPoint now defined in consensus/outpoint.h (uint256-based)
// Old Hash256-based definition removed to use canonical uint256 type

struct TxOut {
    uint64_t value;
    std::vector<uint8_t> scriptPubKey;

    TxOut() : value(0) {}
};

struct TxIn {
    OutPoint prevout;
    std::vector<uint8_t> scriptSig;
    uint32_t sequence;

    TxIn() : sequence(0xFFFFFFFF) {}
};

struct Transaction {
    uint32_t version;
    std::vector<TxIn> inputs;
    std::vector<TxOut> outputs;
    uint32_t locktime;

    Transaction() : version(1), locktime(0) {}

    bool isCoinbase() const {
        return inputs.size() == 1 && inputs[0].prevout.IsNull();
    }
};

struct Block {
    std::vector<Transaction> transactions;

    Block() = default;
};

//=============================================================================
// UTXO Snapshot Interface (Read-Only)
//=============================================================================

struct IUTXOSnapshot {
    virtual ~IUTXOSnapshot() = default;

    virtual std::optional<TxOut> getUTXO(const OutPoint& outpoint) const = 0;
    virtual bool hasUTXO(const OutPoint& outpoint) const = 0;
};

//=============================================================================
// Consensus Parameters
//=============================================================================

struct ConsensusParams {
    // BIP activation heights (placeholder)
    uint32_t bip16_height = 0;
    uint32_t bip34_height = 0;
    uint32_t bip65_height = 0;
    uint32_t bip66_height = 0;
    uint32_t bip113_height = 0;

    // Consensus limits
    uint64_t max_money = 21000000ULL * 100000000ULL; // 21M coins

    ConsensusParams() = default;
};

//=============================================================================
// Validation Results
//=============================================================================

struct ConsensusValidationResult {
    bool ok;
    std::string error;

    ConsensusValidationResult() : ok(true), error("") {}

    static ConsensusValidationResult Ok() {
        return ConsensusValidationResult();
    }

    static ConsensusValidationResult Fail(const std::string& err) {
        ConsensusValidationResult result;
        result.ok = false;
        result.error = err;
        return result;
    }
};

// Extended result for transaction validation (includes fee information)
struct TxValidationResult {
    bool ok;
    uint64_t total_in;   // Sum of input values
    uint64_t total_out;  // Sum of output values
    uint64_t fee;        // total_in - total_out (0 for coinbase)
    std::string error;

    TxValidationResult() : ok(true), total_in(0), total_out(0), fee(0), error("") {}

    static TxValidationResult Ok(uint64_t in, uint64_t out, uint64_t f) {
        TxValidationResult result;
        result.ok = true;
        result.total_in = in;
        result.total_out = out;
        result.fee = f;
        return result;
    }

    static TxValidationResult Fail(const std::string& err) {
        TxValidationResult result;
        result.ok = false;
        result.error = err;
        return result;
    }
};

// Block-level validation result (includes subsidy and fee information)
struct BlockValidationResult {
    bool ok;
    uint64_t total_fees;        // Sum of all non-coinbase tx fees
    uint64_t coinbase_value;    // Sum of coinbase outputs
    uint64_t subsidy;           // GetBlockSubsidy(height)
    std::string error;

    BlockValidationResult() : ok(true), total_fees(0), coinbase_value(0), subsidy(0), error("") {}

    static BlockValidationResult Ok(uint64_t fees, uint64_t cb_value, uint64_t sub) {
        BlockValidationResult result;
        result.ok = true;
        result.total_fees = fees;
        result.coinbase_value = cb_value;
        result.subsidy = sub;
        return result;
    }

    static BlockValidationResult Fail(const std::string& err) {
        BlockValidationResult result;
        result.ok = false;
        result.error = err;
        return result;
    }
};

//=============================================================================
// Subsidy Schedule (Phase E - Economics)
//=============================================================================

// Get block subsidy for given height
// This function is defined in Economics layer but used by consensus validation
uint64_t GetBlockSubsidy(uint32_t height, const ConsensusParams& params);

//=============================================================================
// ConsensusValidator: Pure Consensus Evaluation
//=============================================================================

class ConsensusValidator {
public:
    ConsensusValidator() = default;

    // Validate transaction consensus rules (basic version)
    ConsensusValidationResult validateTx(
        const Transaction& tx,
        const IUTXOSnapshot& utxo_view,
        const ConsensusParams& params
    );

    // Validate transaction consensus rules (extended with fee information)
    TxValidationResult validateTxExtended(
        const Transaction& tx,
        const IUTXOSnapshot& utxo_view,
        const ConsensusParams& params
    );

    // Validate coinbase transaction
    ConsensusValidationResult validateCoinbase(
        const Transaction& tx,
        uint32_t height,
        const ConsensusParams& params
    );

    // Validate entire block (all transactions + coinbase economics)
    // This is the authoritative block-level validation that G.3.4 depends on
    BlockValidationResult validateBlock(
        const Block& block,
        uint32_t height,
        const IUTXOSnapshot& utxo_view,
        const ConsensusParams& params
    );

private:
    // Helper: Check for duplicate inputs
    bool hasDuplicateInputs(const Transaction& tx) const;

    // Helper: Check transaction sanity
    ConsensusValidationResult checkTransactionSanity(const Transaction& tx) const;

    // Helper: Verify inputs against UTXO set
    ConsensusValidationResult verifyInputs(
        const Transaction& tx,
        const IUTXOSnapshot& utxo_view
    ) const;
};

} // namespace p2p
} // namespace dinero
