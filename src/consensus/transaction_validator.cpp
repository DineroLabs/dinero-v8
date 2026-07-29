#include "consensus/transaction_validator.h"
#include "consensus/interfaces/iutxo_provider.h"  // IUTXOProvider interface
#include "consensus/utxo_entry.h"
#include "consensus/outpoint.h"  // OutPoint type
#include "crypto/evp_secp256k1.h"  // DineroSecpIllegalCallback (non-aborting)
#include "consensus/script_cache.h"  // F.8.3: Script execution cache
#include "consensus/signature_cache.h"  // F.8.4: Signature cache
#include "consensus/script_interpreter.h"  // For SignatureHashTaproot, CheckSchnorrSignature
#include "consensus/script_validation.h"   // ValidateSpend / P2MR dispatcher
#include "consensus/script_verify.h"       // Phase C.1: ScriptVerifier::VerifyTaproot (script-path)
#include "consensus/covenant_activation.h" // Phase C.2: Height-gated covenant activation
#include "consensus/crypto/sighash_bip143.h"  // Sighash computation (consensus)
#include "primitives/hash_domains.h"  // Phase M.4.3-B: TxId type
#include <iostream>
#include <set>
#include <unordered_set>  // Phase M.0: For TxOutPoint duplicate detection
#include <utility>
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <secp256k1_extrakeys.h>

namespace dinero {

namespace {

std::string ScriptValidationFailureReason(consensus::ScriptValidationResult rc) {
    switch (rc) {
        case consensus::ScriptValidationResult::INVALID_SIGNATURE:
            return "invalid signature";
        case consensus::ScriptValidationResult::INVALID_SCRIPT:
            return "invalid script";
        case consensus::ScriptValidationResult::UNSUPPORTED_SCRIPT:
            return "unsupported script";
        case consensus::ScriptValidationResult::EXTRACT_FAILED:
            return "script extraction failed";
        case consensus::ScriptValidationResult::OK:
            return "ok";
    }
    return "unknown script validation failure";
}

} // namespace

TransactionValidator::ValidationResult TransactionValidator::ValidateTransaction(
    const Transaction& tx,
    consensus::IUTXOProvider* utxo_provider,
    uint32_t current_height
) {
    ValidationResult result;
    result.valid = false;
    // Phase M.6.1: Use AmountUna::Zero()
    result.total_fee = AmountUna::Zero();

    // 1. Check transaction structure
    if (!CheckStructure(tx, result.error)) {
        return result;
    }

    // 2. Check all inputs exist in UTXO set
    if (!CheckInputsExist(tx, utxo_provider, current_height, result.error)) {
        return result;
    }

    // 3. Check for double-spends (inputs not already in mempool)
    if (!CheckNoDoubleSpend(tx, utxo_provider, result.error)) {
        return result;
    }

    // 4. Verify all signatures
    if (!VerifySignatures(tx, utxo_provider, current_height, result.error)) {
        return result;
    }

    // 5. Check fees are sufficient
    if (!CheckFees(tx, utxo_provider, result.total_fee, result.error)) {
        return result;
    }

    result.valid = true;
    return result;
}

bool TransactionValidator::CheckStructure(const Transaction& tx, std::string& error) {
    // Check version
    if (tx.version < 1 || tx.version > 2) {
        error = "Invalid transaction version";
        return false;
    }
    
    // Check has inputs
    if (tx.vin.empty()) {
        error = "Transaction has no inputs";
        return false;
    }
    
    // Check has outputs
    if (tx.vout.empty()) {
        error = "Transaction has no outputs";
        return false;
    }
    
    // Check size
    size_t tx_size = tx.GetBaseSize();
    if (tx_size > MAX_TX_SIZE) {
        error = "Transaction too large: " + std::to_string(tx_size) + " bytes";
        return false;
    }
    
    // Check outputs are valid
    for (const auto& output : tx.vout) {
        // Phase M.6.1: Use AmountUna comparison
        if (output.value == AmountUna::Zero()) {
            error = "Output value cannot be zero";
            return false;
        }

        if (output.scriptPubKey.empty()) {
            error = "Output scriptPubKey is empty";
            return false;
        }

        // Check for dust (too small to be economical)
        // Phase M.6.1: Use IsDust() helper
        if (output.value.IsDust()) {
            error = "Output value below dust threshold (546 una)";
            return false;
        }
    }
    
    // Check inputs are valid
    for (const auto& input : tx.vin) {
        if (input.prevout.txid.IsNull()) {
            error = "Input has empty txid";
            return false;
        }
        
        // For witness transactions, witness data is required
        if (tx.HasWitness() && input.witness.empty()) {
            error = "Witness transaction missing witness data";
            return false;
        }
    }
    
    return true;
}

bool TransactionValidator::CheckInputsExist(
    const Transaction& tx,
    consensus::IUTXOProvider* utxo_provider,
    uint32_t current_height,
    std::string& error
) {
    if (!utxo_provider) {
        error = "UTXO provider not available";
        return false;
    }

    // Check each input exists in UTXO set
    for (const auto& input : tx.vin) {
        // Look up UTXO by outpoint
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);
        auto utxo_opt = utxo_provider->GetUTXO(outpoint);
        if (!utxo_opt.has_value()) {
            // UTXO not found = either doesn't exist or already spent
            error = "Input UTXO not found (missing or spent): " +
                    input.prevout.txid.AsUint256().GetHex().substr(0, 16) + "...:" +
                    std::to_string(input.prevout.vout);
            return false;
        }

        const consensus::UTXOEntry& utxo = utxo_opt.value();

        // Check coinbase maturity (must wait 100 blocks before spending coinbase)
        if (utxo.isCoinbase) {
            uint32_t maturity = current_height - utxo.height;
            if (maturity < 100) {  // COINBASE_MATURITY constant
                error = "Coinbase UTXO not yet mature (need 100 blocks, have " +
                       std::to_string(maturity) + " blocks)";
                return false;
            }
        }
    }

    return true;
}

bool TransactionValidator::CheckNoDoubleSpend(
    const Transaction& tx,
    consensus::IUTXOProvider* utxo_provider,
    std::string& error
) {
    // Check that none of the inputs are already spent in mempool
    // For now, we assume mempool tracks spent inputs
    // In production, you'd check against mempool's spent set
    (void)utxo_provider;  // Not currently used for mempool tracking

    // Check for duplicate inputs within the transaction itself
    // Phase M.0: Use TxOutPoint directly for identity (consensus-critical)
    std::unordered_set<TxOutPoint> input_set;
    for (const auto& input : tx.vin) {
        if (input_set.count(input.prevout)) {  // Phase M.0: Direct TxOutPoint comparison
            error = "Transaction spends same input twice";
            return false;
        }
        input_set.insert(input.prevout);  // Phase M.0: Store TxOutPoint, not hex string
    }

    return true;
}

// ============================================================================
// F.8.5: Per-Input Verification (Canonical Engine)
// ============================================================================

TransactionValidator::InputVerificationResult TransactionValidator::VerifyInput(
    const Transaction& tx,
    size_t input_index,
    const std::vector<uint8_t>& scriptPubKey,
    uint64_t value,
    uint32_t script_flags,
    uint32_t height,
    uint64_t median_time_past,
    const std::vector<uint64_t>& all_input_amounts,
    const std::vector<std::vector<uint8_t>>& all_input_scriptpubkeys,
    const std::vector<uint8_t>& all_input_confidential_flags,
    const std::vector<std::vector<uint8_t>>& all_input_commitments
) {
    InputVerificationResult result;
    result.valid = false;

    // Validate input_index
    if (input_index >= tx.vin.size()) {
        result.error = "Input index out of bounds";
        return result;
    }

    const auto& input = tx.vin[input_index];

    // This bit must be folded into the cache key before lookup. Restrict the
    // chain-parameter query to Taproot script-path spends; other script types
    // must remain testable before SelectParams().
    const bool is_taproot_script_path =
        scriptPubKey.size() == 34 &&
        scriptPubKey[0] == 0x51 &&
        scriptPubKey[1] == 0x20 &&
        input.witness.size() >= 2;
    if (is_taproot_script_path &&
        consensus::CcvSuccessorBindingActivationParams::IsActive(
            height, dinero::GetActiveChain())) {
        script_flags |= consensus::SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING;
    }

    // ========================================================================
    // BIP113: Locktime Validation (Median Time Past for time-based locks)
    // ========================================================================
    // Only validate locktime if this is the FIRST input (index 0)
    // and transaction is not final
    //
    // Transaction is final if:
    //   - lockTime == 0, OR
    //   - All inputs have sequence == 0xffffffff
    //
    // If not final, check locktime against height or MTP:
    //   - lockTime < 500000000: Block height (compare with height)
    //   - lockTime >= 500000000: Unix timestamp (BIP113: compare with MTP)
    // ========================================================================
    if (input_index == 0) {  // Only check once per transaction
        // Check if transaction is final
        bool is_final = (tx.lockTime == 0);

        if (!is_final) {
            // Check if all inputs have max sequence (final)
            bool all_max_sequence = true;
            for (const auto& vin : tx.vin) {
                if (vin.sequence != 0xffffffff) {
                    all_max_sequence = false;
                    break;
                }
            }
            is_final = all_max_sequence;
        }

        if (!is_final) {
            // Transaction is NOT final - validate locktime
            const uint32_t LOCKTIME_THRESHOLD = 500000000;

            if (tx.lockTime < LOCKTIME_THRESHOLD) {
                // Height-based locktime
                if (tx.lockTime > height) {
                    result.error = "Transaction locktime not yet satisfied (height-based): " +
                                  std::to_string(tx.lockTime) + " > " + std::to_string(height);
                    return result;
                }
            } else {
                // Time-based locktime - BIP113: Use Median Time Past
                if (tx.lockTime > median_time_past) {
                    result.error = "Transaction locktime not yet satisfied (BIP113 time-based): " +
                                  std::to_string(tx.lockTime) + " > " + std::to_string(median_time_past);
                    return result;
                }
            }
        }
    }

    // Continue with signature verification...

    // Get transaction ID for cache key computation - Phase M.4.3-D: Explicit boundary
    // ScriptCache not yet migrated to TxId, so unwrap at boundary
    uint256 txid = tx.GetTxid().AsUint256();

    // F.8.3: Check script cache before expensive verification
    bool cached_result = false;
    if (consensus::g_script_cache) {
        auto cache_key = consensus::ScriptCache::computeKey(
            txid,
            static_cast<uint32_t>(input_index),
            script_flags,
            input.witness
        );

        if (consensus::g_script_cache->get(cache_key, cached_result)) {
            // Cache hit!
            if (!cached_result) {
                result.error = "Script verification failed (cached failure)";
                return result;
            }
            // Cache hit with success
            result.valid = true;
            return result;
        }
    }

    // Cache miss - perform full verification.
    //
    // P2MR is the v7 post-quantum path. Route it through the same
    // ValidateSpend dispatcher used by mempool and block validation so the
    // parallel TransactionValidator path cannot drift from canonical consensus
    // script validation.
    if (consensus::DetectScriptType(scriptPubKey) == consensus::ScriptType::P2MR) {
        const bool has_any_prevout_context =
            !all_input_amounts.empty() ||
            !all_input_scriptpubkeys.empty() ||
            !all_input_confidential_flags.empty() ||
            !all_input_commitments.empty();

        const bool has_full_prevout_context =
            all_input_amounts.size() == tx.vin.size() &&
            all_input_scriptpubkeys.size() == tx.vin.size() &&
            all_input_confidential_flags.size() == tx.vin.size() &&
            all_input_commitments.size() == tx.vin.size();

        std::vector<consensus::UTXOEntry> all_utxos;
        all_utxos.reserve(tx.vin.size());

        if (has_full_prevout_context) {
            for (size_t i = 0; i < tx.vin.size(); ++i) {
                consensus::UTXOEntry entry;
                entry.value = AmountUna::Una(all_input_amounts[i]);
                entry.scriptPubKey = all_input_scriptpubkeys[i];
                entry.height = height;
                entry.is_confidential = (all_input_confidential_flags[i] != 0);
                entry.commitment = all_input_commitments[i];
                all_utxos.push_back(std::move(entry));
            }
        } else if (!has_any_prevout_context && tx.vin.size() == 1) {
            consensus::UTXOEntry entry;
            entry.value = AmountUna::Una(value);
            entry.scriptPubKey = scriptPubKey;
            entry.height = height;
            all_utxos.push_back(std::move(entry));
        } else {
            result.error = "P2MR prevout context incomplete for input " +
                          std::to_string(input_index) +
                          " (need amounts, scripts, flags, and commitments for all " +
                          std::to_string(tx.vin.size()) + " prevouts)";
            return result;
        }

        const consensus::ScriptValidationResult script_result =
            consensus::ValidateSpend(tx, input_index, all_utxos[input_index],
                                     height, all_utxos);

        if (consensus::g_script_cache) {
            auto cache_key = consensus::ScriptCache::computeKey(
                txid, static_cast<uint32_t>(input_index), script_flags, input.witness
            );
            consensus::g_script_cache->insert(
                cache_key, script_result == consensus::ScriptValidationResult::OK);
        }

        if (script_result == consensus::ScriptValidationResult::OK) {
            result.valid = true;
        } else {
            result.error = "P2MR script validation failed for input " +
                           std::to_string(input_index) + ": " +
                           ScriptValidationFailureReason(script_result);
        }
        return result;
    }

    // Cache miss - perform full secp256k1 verification.
    // Register the non-aborting illegal_callback so malformed adversarial
    // inputs hit a logged-rejection path instead of __fastfail.
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (ctx) {
        secp256k1_context_set_illegal_callback(
            ctx, dinero::crypto::DineroSecpIllegalCallback, nullptr);
    }

    // SegWit v0 P2WPKH validation
    if (tx.IsSegWitV0()) {
        // P2WPKH witness: [signature, pubkey]
        if (input.witness.size() != 2) {
            secp256k1_context_destroy(ctx);
            result.error = "Invalid witness structure for input " + std::to_string(input_index) +
                          " (expected 2 items for P2WPKH, got " + std::to_string(input.witness.size()) + ")";

            // Cache negative result
            if (consensus::g_script_cache) {
                auto cache_key = consensus::ScriptCache::computeKey(
                    txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                );
                consensus::g_script_cache->insert(cache_key, false);
            }
            return result;
        }

        const auto& signature_der = input.witness[0];
        const auto& pubkey_bytes = input.witness[1];

        // Verify signature length (64-73 bytes for DER encoding)
        if (signature_der.size() < 64 || signature_der.size() > 73) {
            secp256k1_context_destroy(ctx);
            result.error = "Invalid signature length for input " + std::to_string(input_index);

            // Cache negative result
            if (consensus::g_script_cache) {
                auto cache_key = consensus::ScriptCache::computeKey(
                    txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                );
                consensus::g_script_cache->insert(cache_key, false);
            }
            return result;
        }

        // Verify pubkey length (33 bytes for compressed)
        if (pubkey_bytes.size() != 33) {
            secp256k1_context_destroy(ctx);
            result.error = "Invalid pubkey length for input " + std::to_string(input_index) +
                          " (expected 33 bytes, got " + std::to_string(pubkey_bytes.size()) + ")";

            // Cache negative result
            if (consensus::g_script_cache) {
                auto cache_key = consensus::ScriptCache::computeKey(
                    txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                );
                consensus::g_script_cache->insert(cache_key, false);
            }
            return result;
        }

        // Parse the public key
        secp256k1_pubkey pubkey;
        if (!secp256k1_ec_pubkey_parse(ctx, &pubkey, pubkey_bytes.data(), pubkey_bytes.size())) {
            secp256k1_context_destroy(ctx);
            result.error = "Failed to parse public key for input " + std::to_string(input_index);

            // Cache negative result
            if (consensus::g_script_cache) {
                auto cache_key = consensus::ScriptCache::computeKey(
                    txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                );
                consensus::g_script_cache->insert(cache_key, false);
            }
            return result;
        }

        // Parse the DER signature
        secp256k1_ecdsa_signature sig;
        if (!secp256k1_ecdsa_signature_parse_der(ctx, &sig, signature_der.data(), signature_der.size() - 1)) {
            secp256k1_context_destroy(ctx);
            result.error = "Failed to parse DER signature for input " + std::to_string(input_index);

            // Cache negative result
            if (consensus::g_script_cache) {
                auto cache_key = consensus::ScriptCache::computeKey(
                    txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                );
                consensus::g_script_cache->insert(cache_key, false);
            }
            return result;
        }

        // Build scriptCode for P2WPKH: OP_DUP OP_HASH160 <20> <pubkey_hash> OP_EQUALVERIFY OP_CHECKSIG
        std::vector<uint8_t> pubkey_hash(scriptPubKey.begin() + 2, scriptPubKey.end());
        std::vector<uint8_t> scriptCode;
        scriptCode.push_back(0x76); // OP_DUP
        scriptCode.push_back(0xa9); // OP_HASH160
        scriptCode.push_back(0x14); // 20 bytes
        scriptCode.insert(scriptCode.end(), pubkey_hash.begin(), pubkey_hash.end());
        scriptCode.push_back(0x88); // OP_EQUALVERIFY
        scriptCode.push_back(0xac); // OP_CHECKSIG

        // Compute BIP143 sighash (consensus layer)
        auto sighash = consensus::SighashBIP143::ComputeSighash(
            tx,
            input_index,
            scriptCode,
            value,
            consensus::SighashBIP143::SIGHASH_ALL
        );

        if (sighash.empty() || sighash.size() != 32) {
            secp256k1_context_destroy(ctx);
            result.error = "Failed to compute BIP143 sighash for input " + std::to_string(input_index);

            // Cache negative result
            if (consensus::g_script_cache) {
                auto cache_key = consensus::ScriptCache::computeKey(
                    txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                );
                consensus::g_script_cache->insert(cache_key, false);
            }
            return result;
        }

        // F.8.4: Check signature cache before expensive secp256k1 verification
        bool sig_valid = false;
        if (consensus::g_signature_cache) {
            auto sig_cache_key = consensus::SignatureCache::computeKey(
                signature_der,
                pubkey_bytes,
                sighash
            );

            if (consensus::g_signature_cache->get(sig_cache_key, sig_valid)) {
                // Signature cache hit!
                secp256k1_context_destroy(ctx);

                if (!sig_valid) {
                    // Cached failure
                    result.error = "Signature verification failed for input " + std::to_string(input_index) +
                                  " (cached signature failure)";

                    // Also cache at script level
                    if (consensus::g_script_cache) {
                        auto cache_key = consensus::ScriptCache::computeKey(
                            txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                        );
                        consensus::g_script_cache->insert(cache_key, false);
                    }
                    return result;
                }

                // Cached success - also cache at script level
                if (consensus::g_script_cache) {
                    auto cache_key = consensus::ScriptCache::computeKey(
                        txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                    );
                    consensus::g_script_cache->insert(cache_key, true);
                }
                result.valid = true;
                return result;
            }
        }

        // Signature cache miss - perform full secp256k1 verification
        if (!secp256k1_ecdsa_verify(ctx, &sig, sighash.data(), &pubkey)) {
            secp256k1_context_destroy(ctx);

            // Cache negative results at both levels
            if (consensus::g_signature_cache) {
                auto sig_cache_key = consensus::SignatureCache::computeKey(
                    signature_der, pubkey_bytes, sighash
                );
                consensus::g_signature_cache->insert(sig_cache_key, false);
            }

            if (consensus::g_script_cache) {
                auto cache_key = consensus::ScriptCache::computeKey(
                    txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                );
                consensus::g_script_cache->insert(cache_key, false);
            }

            result.error = "Signature verification failed for input " + std::to_string(input_index) +
                          " (invalid ECDSA signature)";
            return result;
        }

        // ✅ Signature verified successfully!
        secp256k1_context_destroy(ctx);

        // Cache positive results at both levels
        if (consensus::g_signature_cache) {
            auto sig_cache_key = consensus::SignatureCache::computeKey(
                signature_der, pubkey_bytes, sighash
            );
            consensus::g_signature_cache->insert(sig_cache_key, true);
        }

        if (consensus::g_script_cache) {
            auto cache_key = consensus::ScriptCache::computeKey(
                txid, static_cast<uint32_t>(input_index), script_flags, input.witness
            );
            consensus::g_script_cache->insert(cache_key, true);
        }

        result.valid = true;
        return result;
    }

    // ========================================================================
    // BIP341 Taproot (SegWit v1) Key-Path Spending
    // ========================================================================
    // P2TR scriptPubKey: OP_1 <32-byte x-only pubkey>
    // Key-path witness: single 64-byte (or 65-byte with sighash) Schnorr sig
    // ========================================================================
    if (scriptPubKey.size() == 34 &&
        scriptPubKey[0] == 0x51 &&  // OP_1 (witness version 1)
        scriptPubKey[1] == 0x20) {  // PUSH32

        // Extract x-only pubkey from scriptPubKey
        std::vector<uint8_t> x_only_pubkey(scriptPubKey.begin() + 2, scriptPubKey.end());

        // Key-path spend: exactly 1 witness element (the signature)
        if (input.witness.size() == 1) {
            const auto& schnorr_sig = input.witness[0];

            // BIP340: Signature must be 64 or 65 bytes
            if (schnorr_sig.size() != 64 && schnorr_sig.size() != 65) {
                secp256k1_context_destroy(ctx);
                result.error = "Invalid Taproot signature size for input " + std::to_string(input_index);
                return result;
            }

            // Extract sighash type
            uint8_t sighash_type = 0x00;  // SIGHASH_DEFAULT
            if (schnorr_sig.size() == 65) {
                sighash_type = schnorr_sig[64];
                uint8_t base_type = sighash_type & 0x7F;
                if (base_type > 0x03) {
                    secp256k1_context_destroy(ctx);
                    result.error = "Invalid Taproot sighash type for input " + std::to_string(input_index);
                    return result;
                }
            }

            // Build context for BIP341 sighash
            // BIP341 requires ALL input amounts and scriptPubKeys for sighash computation
            std::vector<uint64_t> amounts_for_sighash;
            std::vector<std::vector<uint8_t>> scripts_for_sighash;
            std::vector<uint8_t> confidential_for_sighash;
            std::vector<std::vector<uint8_t>> commitments_for_sighash;

            if (!all_input_amounts.empty() && !all_input_scriptpubkeys.empty() &&
                all_input_amounts.size() == tx.vin.size() &&
                all_input_scriptpubkeys.size() == tx.vin.size() &&
                all_input_confidential_flags.size() == tx.vin.size() &&
                all_input_commitments.size() == tx.vin.size()) {
                // Full UTXO context provided - use it (correct BIP341 behavior)
                amounts_for_sighash = all_input_amounts;
                scripts_for_sighash = all_input_scriptpubkeys;
                confidential_for_sighash = all_input_confidential_flags;
                commitments_for_sighash = all_input_commitments;
            } else {
                secp256k1_context_destroy(ctx);
                result.error = "Taproot prevout context incomplete for input " +
                              std::to_string(input_index) + " (need amounts, scripts, flags, and commitments for all " +
                              std::to_string(tx.vin.size()) + " prevouts)";
                return result;
            }

            if (amounts_for_sighash.size() != tx.vin.size() ||
                scripts_for_sighash.size() != tx.vin.size() ||
                confidential_for_sighash.size() != tx.vin.size() ||
                commitments_for_sighash.size() != tx.vin.size()) {
                secp256k1_context_destroy(ctx);
                result.error = "Taproot prevout context incomplete for input " +
                              std::to_string(input_index) + " (missing amounts/scripts for all " +
                              std::to_string(tx.vin.size()) + " inputs)";
                return result;
            }

            consensus::ScriptExecutionContext sighash_ctx(
                &tx,
                static_cast<uint32_t>(input_index),
                value,
                consensus::SCRIPT_VERIFY_TAPROOT,
                amounts_for_sighash,
                scripts_for_sighash,
                confidential_for_sighash,
                commitments_for_sighash
            );

            std::vector<uint8_t> sighash = consensus::SignatureHashTaproot(
                sighash_ctx, sighash_type, {}, {});

            if (sighash.size() != 32) {
                secp256k1_context_destroy(ctx);
                result.error = "Failed to compute Taproot sighash for input " + std::to_string(input_index);
                return result;
            }

            // BIP340 Schnorr verification
            if (!consensus::CheckSchnorrSignature(schnorr_sig, x_only_pubkey, sighash,
                                                   consensus::SCRIPT_VERIFY_TAPROOT)) {
                secp256k1_context_destroy(ctx);
                result.error = "Taproot signature verification failed for input " + std::to_string(input_index);
                return result;
            }

            // Success!
            secp256k1_context_destroy(ctx);
            result.valid = true;
            return result;
        }

        // Script-path spend (>1 witness elements) — delegate to ScriptVerifier
        // which handles control block, Merkle proof, TapTweak, and Tapscript execution
        // including covenant opcodes (CTV, CSFS, TXHASH, CCV).
        if (input.witness.size() >= 2) {
            secp256k1_context_destroy(ctx);

            // Phase C.2: Check covenant activation height
            if (!consensus::CovenantActivationParams::IsCovenantActive(height, dinero::GetActiveChain())) {
                result.error = "Taproot script-path spending not yet active at height " +
                              std::to_string(height) + " (activates at " +
                              std::to_string(consensus::CovenantActivationParams::GetActivationHeight(
                                  dinero::GetActiveChain())) + ")";
                return result;
            }

            // Build UTXOEntry vector for ScriptVerifier
            std::vector<consensus::UTXOEntry> utxo_entries;
            utxo_entries.reserve(tx.vin.size());
            for (size_t i = 0; i < tx.vin.size(); ++i) {
                consensus::UTXOEntry entry;
                if (i < all_input_amounts.size()) entry.value = AmountUna::Una(all_input_amounts[i]);
                if (i < all_input_scriptpubkeys.size()) entry.scriptPubKey = all_input_scriptpubkeys[i];
                if (i < all_input_confidential_flags.size()) entry.is_confidential = (all_input_confidential_flags[i] != 0);
                if (i < all_input_commitments.size()) entry.commitment = all_input_commitments[i];
                utxo_entries.push_back(entry);
            }

            std::string script_error;
            const uint32_t taproot_flags =
                consensus::SCRIPT_VERIFY_STANDARD |
                (script_flags & consensus::SCRIPT_VERIFY_CCV_SUCCESSOR_BINDING);
            bool valid = consensus::ScriptVerifier::VerifyTaproot(
                tx, input_index, utxo_entries, script_error, taproot_flags);

            if (valid) {
                // Cache positive result
                if (consensus::g_script_cache) {
                    auto cache_key = consensus::ScriptCache::computeKey(
                        txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                    );
                    consensus::g_script_cache->insert(cache_key, true);
                }
                result.valid = true;
            } else {
                result.error = "Taproot script-path: " + script_error;
                // Cache negative result
                if (consensus::g_script_cache) {
                    auto cache_key = consensus::ScriptCache::computeKey(
                        txid, static_cast<uint32_t>(input_index), script_flags, input.witness
                    );
                    consensus::g_script_cache->insert(cache_key, false);
                }
            }
            return result;
        }

        // Empty witness
        secp256k1_context_destroy(ctx);
        result.error = "Empty witness for Taproot output at input " + std::to_string(input_index);
        return result;
    }

    // Unsupported script type
    secp256k1_context_destroy(ctx);
    result.error = "Unsupported script type for input " + std::to_string(input_index);
    return result;
}

// ============================================================================
// Legacy VerifySignatures (now calls VerifyInput in a loop)
// ============================================================================

bool TransactionValidator::VerifySignatures(
    const Transaction& tx,
    consensus::IUTXOProvider* utxo_provider,
    uint32_t current_height,
    std::string& error
) {
    if (!utxo_provider) {
        error = "UTXO provider not available";
        return false;
    }

    // Verify each signature using BIP143 (SegWit) sighash
    // F.8.5: Now uses canonical VerifyInput engine

    std::vector<consensus::UTXOEntry> prevouts;
    prevouts.reserve(tx.vin.size());
    std::vector<uint64_t> all_input_amounts;
    std::vector<std::vector<uint8_t>> all_input_scriptpubkeys;
    std::vector<uint8_t> all_input_confidential_flags;
    std::vector<std::vector<uint8_t>> all_input_commitments;

    for (size_t i = 0; i < tx.vin.size(); ++i) {
        const auto& input = tx.vin[i];
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);
        auto utxo_opt = utxo_provider->GetUTXO(outpoint);
        if (!utxo_opt.has_value()) {
            error = "Cannot verify signature: UTXO not found for input " + std::to_string(i);
            return false;
        }

        prevouts.push_back(utxo_opt.value());
        all_input_amounts.push_back(prevouts.back().value.GetUna());
        all_input_scriptpubkeys.push_back(prevouts.back().scriptPubKey);
        all_input_confidential_flags.push_back(prevouts.back().is_confidential ? 1 : 0);
        all_input_commitments.push_back(prevouts.back().commitment);
    }

    for (size_t i = 0; i < tx.vin.size(); i++) {
        const consensus::UTXOEntry& utxo = prevouts[i];

        // F.8.5: Use canonical VerifyInput engine
        // Mempool context: no block context, so MTP = 0 (permissive for mempool admission)
        // Height = utxo.height (for height-based CLTV when implemented)
        // Phase M.6.2: Extract raw value for script verification
        auto result = VerifyInput(tx, i, utxo.scriptPubKey, utxo.value.GetUna(),
                                   0 /* script_flags */,
                                   current_height,
                                   0 /* median_time_past - mempool has no MTP context */,
                                   all_input_amounts,
                                   all_input_scriptpubkeys,
                                   all_input_confidential_flags,
                                   all_input_commitments);
        if (!result.valid) {
            error = result.error;
            return false;
        }
    }

    return true;
}

bool TransactionValidator::CheckFees(
    const Transaction& tx,
    consensus::IUTXOProvider* utxo_provider,
    AmountUna& fee,
    std::string& error
) {
    if (!utxo_provider) {
        error = "UTXO provider not available";
        return false;
    }

    // Phase M.6.3: Calculate total input value with checked arithmetic
    AmountUna total_in = AmountUna::Zero();

    for (const auto& input : tx.vin) {
        // Look up the UTXO to get its value
        OutPoint outpoint(input.prevout.txid, input.prevout.vout);
        auto utxo_opt = utxo_provider->GetUTXO(outpoint);
        if (!utxo_opt.has_value()) {
            error = "Cannot calculate fee: UTXO not found for input " +
                    input.prevout.txid.AsUint256().GetHex().substr(0, 16) + "...:" + std::to_string(input.prevout.vout);
            return false;
        }

        const consensus::UTXOEntry& utxo = utxo_opt.value();

        // Phase M.6.3: Use checked arithmetic - overflow impossible to miss
        auto result = total_in.Add(utxo.value);
        if (!result) {
            error = "Input value overflow";
            return false;
        }
        total_in = *result;
    }
    
    // Phase M.6.3: Calculate total output value with checked arithmetic
    AmountUna total_out = AmountUna::Zero();
    for (const auto& output : tx.vout) {
        auto result = total_out.Add(output.value);
        if (!result) {
            error = "Output value overflow";
            return false;
        }
        total_out = *result;
    }
    
    // Outputs cannot exceed inputs
    if (total_out > total_in) {
        error = "Output value exceeds input value (total_in: " + std::to_string(total_in.GetUna()) +
               " una, total_out: " + std::to_string(total_out.GetUna()) + " una)";
        return false;
    }

    // Phase M.6.3: Calculate fee with checked subtraction
    auto fee_result = total_in.Sub(total_out);
    if (!fee_result) {
        error = "Fee calculation underflow (should be impossible after total check)";
        return false;
    }
    fee = *fee_result;

    // Check minimum fee (100 una minimum)
    // Phase M.6.1: Use amounts::MIN_TX_FEE constant
    if (fee < amounts::MIN_TX_FEE) {
        error = "Fee too low: " + std::to_string(fee.GetUna()) + " una (minimum: " +
               std::to_string(amounts::MIN_TX_FEE.GetUna()) + " una)";
        return false;
    }
    
    // Check fee is reasonable (not absurdly high - prevents user errors)
    size_t tx_size = tx.GetVirtualSize();
    uint64_t max_reasonable_fee = tx_size * 1000;  // 1000 una per vbyte max
    // Phase M.6.1: Compare and extract values
    if (fee.GetUna() > max_reasonable_fee) {
        error = "Fee unreasonably high: " + std::to_string(fee.GetUna()) + " una (" +
               std::to_string(static_cast<double>(fee.GetUna()) / 1e8) + " DIN) " +
               "(max reasonable: " + std::to_string(max_reasonable_fee) + " una for " +
               std::to_string(tx_size) + " vbytes)";
        return false;
    }
    
    return true;
}

} // namespace dinero
