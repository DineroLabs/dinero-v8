#include "consensus/tx_validation.h"
#include "consensus/coins_db.h"
#include "consensus/covenant_activation.h"
#include "consensus/covenants.h"
#include "consensus/script_interpreter.h"
#include "consensus/script_verify.h"
#include <set>
#include <algorithm>

namespace dinero {
namespace consensus {

// ============================================================================
// Phase 23.1: Transaction Validator Implementation
// ============================================================================

bool isCoinbase(const Transaction& tx) {
    // Use Transaction's built-in IsCoinbase() method
    return tx.IsCoinbase();
}

bool hasDuplicateInputs(const Transaction& tx) {
    std::set<OutPoint> seen;

    for (const auto& input : tx.vin) {
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);

        if (seen.count(outpoint)) {
            return true;  // Duplicate found
        }

        seen.insert(outpoint);
    }

    return false;
}

TxValidationResult validateOutputs(const Transaction& tx, AmountUna& total_out) {
    total_out = AmountUna::Zero();

    if (tx.vout.empty()) {
        return TxValidationResult::TX_EMPTY;
    }

    for (const auto& output : tx.vout) {
        // Check output value is positive and within bounds
        // Phase M.6.3: Use AmountUna type-safe comparison
        if (output.value == AmountUna::Zero()) {
            return TxValidationResult::INVALID_OUTPUT_VALUE;
        }

        if (output.value.GetUna() > MAX_MONEY) {
            return TxValidationResult::INVALID_OUTPUT_VALUE;
        }

        // Phase M.6.3: Use checked arithmetic - overflow impossible to miss
        auto result = total_out.Add(output.value);
        if (!result) {
            return TxValidationResult::OUTPUT_SUM_OVERFLOW;
        }
        total_out = *result;
    }

    // Final check: total outputs must not exceed MAX_MONEY
    if (total_out.GetUna() > MAX_MONEY) {
        return TxValidationResult::OUTPUT_SUM_OVERFLOW;
    }

    return TxValidationResult::OK;
}

TxValidationResult validateInputs(
    const Transaction& tx,
    CoinsViewCache& view,
    const TxValidationContext& ctx,
    AmountUna& total_in
) {
    total_in = AmountUna::Zero();

    if (tx.vin.empty()) {
        return TxValidationResult::TX_EMPTY;
    }

    std::vector<UTXOEntry> input_utxos;
    input_utxos.reserve(tx.vin.size());

    for (size_t i = 0; i < tx.vin.size(); i++) {
        const auto& input = tx.vin[i];
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);

        // Check for null outpoint in non-coinbase transaction
        if (outpoint.IsNull()) {
            return TxValidationResult::NON_COINBASE_HAS_NULL_INPUT;
        }

        // Lookup UTXO in view
        auto coin_result = view.getCoin(outpoint);
        if (!coin_result.ok()) {
            return TxValidationResult::INPUT_NOT_FOUND;
        }

        const UTXOEntry& coin = coin_result.value();
        input_utxos.push_back(coin);

        // Check coinbase maturity (100 confirmations)
        if (coin.isCoinbase && !coin.isMature(ctx.block_height)) {
            return TxValidationResult::COINBASE_MATURITY_VIOLATION;
        }

        // Phase M.6.3: Use checked arithmetic - overflow impossible to miss
        auto result = total_in.Add(coin.value);
        if (!result) {
            return TxValidationResult::OUTPUT_SUM_OVERFLOW;
        }
        total_in = *result;

        // ====================================================================
        // Phase 24: Script verification (ENABLED)
        // ====================================================================
        // Full script execution for all script types:
        // - P2PKH (Pay-to-PubKey-Hash)
        // - P2SH (Pay-to-Script-Hash)
        // - P2WPKH (SegWit v0 witness pubkey hash)
        // - P2WSH (SegWit v0 witness script hash)
        // - P2TR (Taproot witness v1)
        //
        // F.10.9: AssumeValid optimization (skip during IBD if below trusted height)
        // When skip_script_verification=true, bypass expensive signature checks
        // Still validates: PoW, merkle roots, UTXO existence, amounts, structure
        // ====================================================================

    }

    if (!ctx.skip_script_verification) {
        std::vector<uint64_t> all_input_amounts;
        std::vector<std::vector<uint8_t>> all_input_scriptpubkeys;
        std::vector<uint8_t> all_input_confidential_flags;
        std::vector<std::vector<uint8_t>> all_input_commitments;
        all_input_amounts.reserve(input_utxos.size());
        all_input_scriptpubkeys.reserve(input_utxos.size());
        all_input_confidential_flags.reserve(input_utxos.size());
        all_input_commitments.reserve(input_utxos.size());

        for (const auto& utxo : input_utxos) {
            all_input_amounts.push_back(utxo.value.GetUna());
            all_input_scriptpubkeys.push_back(utxo.scriptPubKey);
            all_input_confidential_flags.push_back(utxo.is_confidential ? 1 : 0);
            all_input_commitments.push_back(utxo.commitment);
        }

        const PrecomputedTransactionData
            covenant_precomputed(tx, input_utxos);
        for (size_t i = 0; i < tx.vin.size(); ++i) {
            const auto& input = tx.vin[i];
            const auto& coin = input_utxos[i];
            bool script_ok = verifyScript(
                input.scriptSig,
                coin.scriptPubKey,
                input.witness,
                tx,
                static_cast<uint32_t>(i),
                coin.value.GetUna(),
                all_input_amounts,
                all_input_scriptpubkeys,
                all_input_confidential_flags,
                all_input_commitments,
                ctx.block_height,
                &covenant_precomputed
            );

            if (!script_ok) {
                return TxValidationResult::SCRIPT_VERIFY_FAILED;
            }
        }
    }
    // else: AssumeValid enabled - skipping script verification for IBD performance

    return TxValidationResult::OK;
}

// BIP68 constants
constexpr uint32_t SEQUENCE_LOCKTIME_DISABLE_FLAG = 0x80000000;  // Bit 31
constexpr uint32_t SEQUENCE_LOCKTIME_TYPE_FLAG    = 0x00400000;  // Bit 22 (time vs height)
constexpr uint32_t SEQUENCE_LOCKTIME_MASK         = 0x0000FFFF;  // Bits 0-15
constexpr uint32_t SEQUENCE_LOCKTIME_GRANULARITY  = 512;         // Time units in seconds

bool checkSequenceLocks(
    const Transaction& tx,
    CoinsViewCache& view,
    const TxValidationContext& ctx
) {
    // Phase 23.3: BIP 68 relative locktime validation
    //
    // BIP 68 allows inputs to specify relative locktimes using nSequence:
    // - If nSequence has bit 31 set (SEQUENCE_LOCKTIME_DISABLE_FLAG), locktime is disabled
    // - Otherwise, bits 0-15 encode relative locktime:
    //   - If bit 22 is set (SEQUENCE_LOCKTIME_TYPE_FLAG): time-based (512-second units)
    //   - If bit 22 is clear: height-based (block count)

    // Skip sequence lock validation if explicitly disabled in context
    if (!ctx.check_sequence_locks) {
        return true;
    }

    // Coinbase transactions don't have sequence lock constraints
    if (isCoinbase(tx)) {
        return true;
    }

    for (size_t i = 0; i < tx.vin.size(); ++i) {
        const auto& input = tx.vin[i];
        uint32_t sequence = input.sequence;

        // If bit 31 is set, sequence lock is disabled for this input
        if (sequence & SEQUENCE_LOCKTIME_DISABLE_FLAG) {
            continue;
        }

        // Get the UTXO to find when it was confirmed
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);
        auto utxo_result = view.getCoin(outpoint);
        if (!utxo_result.ok()) {
            // UTXO not found - this is a separate validation error
            // Let validateInputs() handle this case
            continue;
        }

        const UTXOEntry& utxo = *utxo_result;
        uint32_t relative_locktime = sequence & SEQUENCE_LOCKTIME_MASK;

        if (sequence & SEQUENCE_LOCKTIME_TYPE_FLAG) {
            // Time-based lock (bit 22 set)
            // Enforce: current_mtp >= utxo_mtp + (relative_locktime * 512)

            // If no MTP lookup is provided, fail-closed (reject)
            if (!ctx.mtp_at_height) {
                return false;
            }

            // Look up MTP of the block that confirmed the UTXO
            auto utxo_mtp_opt = ctx.mtp_at_height(utxo.height);
            if (!utxo_mtp_opt) {
                // Failed to look up MTP - fail-closed
                return false;
            }

            uint64_t utxo_mtp = *utxo_mtp_opt;
            uint64_t required_time = utxo_mtp + (static_cast<uint64_t>(relative_locktime) * SEQUENCE_LOCKTIME_GRANULARITY);

            if (ctx.median_time_past < required_time) {
                // Time lock not yet expired
                return false;
            }
        } else {
            // Height-based lock (bit 22 clear)
            // Input can be spent when: current_height >= utxo_height + relative_locktime
            uint32_t min_height = utxo.height + relative_locktime;

            if (ctx.block_height < min_height) {
                // Lock not yet expired
                return false;
            }
        }
    }

    // All sequence locks satisfied
    return true;
}

bool verifyScript(
    const std::vector<uint8_t>& scriptSig,
    const std::vector<uint8_t>& scriptPubKey,
    const std::vector<std::vector<uint8_t>>& witness,
    const Transaction& tx,
    uint32_t input_index,
    uint64_t amount,
    const std::vector<uint64_t>& all_input_amounts,
    const std::vector<std::vector<uint8_t>>& all_input_scriptpubkeys,
    const std::vector<uint8_t>& all_input_confidential_flags,
    const std::vector<std::vector<uint8_t>>& all_input_commitments,
    uint32_t block_height,
    const PrecomputedTransactionData* covenant_precomputed
) {
    // ========================================================================
    // Phase 24: Full Script Verification Engine
    // ========================================================================
    //
    // This implements Bitcoin's complete script execution model:
    // - Legacy scripts (P2PKH, P2SH) with full opcode support
    // - SegWit v0 (P2WPKH, P2WSH) with BIP 143 sighash
    // - Taproot (P2TR) with BIP 340 Schnorr signatures
    // - All standard consensus flags enabled

    // Create scripts
    Script sig_script(scriptSig);
    Script pubkey_script(scriptPubKey);

    const uint32_t flags = CovenantActivationParams::StandardFlags(
        block_height, dinero::Params());

    // Route Taproot through the verifier that authenticates the control block
    // and output-key tweak. The generic legacy interpreter's historical v1
    // branch predates that check and must not be a consensus entry point.
    if (ScriptVerifier::IsP2TR(scriptPubKey)) {
        if (!scriptSig.empty() ||
            all_input_amounts.size() != tx.vin.size() ||
            all_input_scriptpubkeys.size() != tx.vin.size() ||
            all_input_confidential_flags.size() != tx.vin.size() ||
            all_input_commitments.size() != tx.vin.size()) {
            return false;
        }

        size_t effective_items = witness.size();
        if (effective_items >= 2 &&
            !witness.back().empty() &&
            witness.back()[0] == 0x50) {
            --effective_items;
        }
        if (effective_items != 1 &&
            !CovenantActivationParams::IsScriptPathActive(
                block_height, dinero::Params())) {
            return false;
        }

        std::vector<UTXOEntry> prevouts;
        prevouts.reserve(tx.vin.size());
        for (size_t i = 0; i < tx.vin.size(); ++i) {
            UTXOEntry prevout;
            prevout.value = AmountUna::Una(all_input_amounts[i]);
            prevout.scriptPubKey = all_input_scriptpubkeys[i];
            prevout.is_confidential = all_input_confidential_flags[i] != 0;
            prevout.commitment = all_input_commitments[i];
            prevouts.push_back(std::move(prevout));
        }

        std::string taproot_error;
        return ScriptVerifier::VerifyTaproot(
            tx, input_index, prevouts, taproot_error, flags,
            covenant_precomputed);
    }

    // Create execution context with authoritative height-derived flags.
    ScriptExecutionContext ctx(
        &tx,
        input_index,
        amount,
        flags,
        all_input_amounts,
        all_input_scriptpubkeys,
        all_input_confidential_flags,
        all_input_commitments
    );

    // Execute script verification
    ScriptError error;
    bool result = VerifyScript(
        sig_script,
        pubkey_script,
        witness,
        ctx,
        error
    );

    // Log error for debugging (optional)
    if (!result) {
        // Script verification failed - error contains details
        // In production, you might want to log: ScriptErrorString(error)
        (void)error;  // Suppress unused warning
    }

    return result;
}

TxValidationOutput validateCoinbase(
    const Transaction& tx,
    const TxValidationContext& ctx
) {
    // Coinbase validation rules (Bitcoin Core consensus)

    // 1. Exactly 1 input
    if (tx.vin.size() != 1) {
        return TxValidationOutput(
            TxValidationResult::COINBASE_INVALID_INPUT,
            "Coinbase must have exactly 1 input"
        );
    }

    // 2. Input must have null outpoint (Phase M.0: use uint256::IsNull())
    const auto& input = tx.vin[0];
    if (!input.prevout.txid.IsNull() || input.prevout.vout != 0xFFFFFFFF) {
        return TxValidationOutput(
            TxValidationResult::COINBASE_INVALID_INPUT,
            "Coinbase input must have null outpoint (0x00...00:0xFFFFFFFF)"
        );
    }

    // 3. ScriptSig size must be between 2 and 100 bytes
    // (BIP 34 requires block height as first element)
    if (input.scriptSig.size() < 2 || input.scriptSig.size() > 100) {
        return TxValidationOutput(
            TxValidationResult::COINBASE_INVALID_SCRIPTSIG,
            "Coinbase scriptSig must be 2-100 bytes (BIP 34)"
        );
    }

    // ========================================================================
    // CONSENSUS: WITNESS NONCE STRUCTURE (Utreexo-compatible)
    // ========================================================================
    // Dinero requires coinbase to have exactly 1 witness item of 8 bytes.
    // This is where miners inject their extranonce (in witness, NOT scriptSig).
    // The witness doesn't affect txid, preserving Utreexo commitment integrity.
    //
    // This is a HARD consensus rule - blocks with invalid coinbase witness
    // structure will be rejected.
    // ========================================================================
    if (input.witness.size() != 1) {
        return TxValidationOutput(
            TxValidationResult::COINBASE_INVALID_WITNESS,
            "Coinbase must have exactly 1 witness item (witness nonce)"
        );
    }
    if (input.witness[0].size() != 8) {
        return TxValidationOutput(
            TxValidationResult::COINBASE_INVALID_WITNESS,
            "Coinbase witness nonce must be exactly 8 bytes"
        );
    }

    // 4. Validate outputs (amount checks)
    // Phase M.6.3: Use AmountUna for type-safe fee calculation
    AmountUna total_out = AmountUna::Zero();
    TxValidationResult output_result = validateOutputs(tx, total_out);
    if (output_result != TxValidationResult::OK) {
        return TxValidationOutput(output_result, "Invalid coinbase outputs");
    }

    // 5. Coinbase value check will be done at block level (Phase 23.2)
    // Block validator will check: coinbase_value ≤ block_reward + fees

    (void)ctx;  // Context not needed for basic coinbase validation

    TxValidationOutput result;
    result.result = TxValidationResult::OK;
    result.fee = 0;  // Coinbase has no fee (it creates coins)
    return result;
}

TxValidationOutput validateTransaction(
    const Transaction& tx,
    CoinsViewCache& view,
    const TxValidationContext& ctx,
    bool is_coinbase
) {
    // Special case: coinbase validation
    if (is_coinbase) {
        return validateCoinbase(tx, ctx);
    }

    // ========================================================================
    // Phase 23.1.A: Validate Transaction Inputs
    // ========================================================================

    // Check for duplicate inputs (same UTXO spent twice in this tx)
    if (hasDuplicateInputs(tx)) {
        return TxValidationOutput(
            TxValidationResult::DUPLICATE_INPUTS,
            "Transaction has duplicate inputs"
        );
    }

    // Validate all inputs exist and are spendable
    // Phase M.6.3: Use AmountUna for type-safe arithmetic
    AmountUna total_in = AmountUna::Zero();
    TxValidationResult input_result = validateInputs(tx, view, ctx, total_in);
    if (input_result != TxValidationResult::OK) {
        return TxValidationOutput(input_result, "Input validation failed");
    }

    // ========================================================================
    // Phase 23.1.B: Validate Transaction Outputs
    // ========================================================================

    AmountUna total_out = AmountUna::Zero();
    TxValidationResult output_result = validateOutputs(tx, total_out);
    if (output_result != TxValidationResult::OK) {
        return TxValidationOutput(output_result, "Output validation failed");
    }

    // ========================================================================
    // Phase 23.1.C: Value Balance + Fee Calculation
    // ========================================================================

    // Total outputs must not exceed total inputs
    if (total_out > total_in) {
        return TxValidationOutput(
            TxValidationResult::INSUFFICIENT_INPUT_VALUE,
            "Outputs exceed inputs (no fee coverage)"
        );
    }

    // Phase M.6.3: Calculate fee using checked arithmetic
    auto fee_result = total_in.Sub(total_out);
    if (!fee_result) {
        return TxValidationOutput(
            TxValidationResult::INTERNAL_ERROR,
            "Fee calculation underflow (should be impossible)"
        );
    }
    AmountUna fee = *fee_result;

    // ========================================================================
    // Phase 23.1.D: Legacy Rules
    // ========================================================================

    // Already checked:
    // ✅ No duplicate inputs
    // ✅ At least 1 input and 1 output (checked in validateInputs/validateOutputs)
    // ✅ No null inputs in non-coinbase (checked in validateInputs)

    // ========================================================================
    // Phase 23.1.E: Sequence Locks (BIP 68)
    // ========================================================================

    if (ctx.check_sequence_locks) {
        if (!checkSequenceLocks(tx, view, ctx)) {
            return TxValidationOutput(
                TxValidationResult::SEQUENCE_LOCK_FAIL,
                "Sequence locks not satisfied (BIP 68)"
            );
        }
    }

    // ========================================================================
    // Validation Success
    // ========================================================================

    TxValidationOutput result;
    result.result = TxValidationResult::OK;
    // Phase M.6.3: Extract raw value for boundary type (TxValidationOutput is public API)
    result.fee = fee.GetUna();
    return result;
}

} // namespace consensus
} // namespace dinero
