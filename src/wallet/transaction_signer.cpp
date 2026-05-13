#include "wallet/transaction_signer.h"
#include "wallet/bip143_signer.h"
#include "wallet/taproot_tx_signer.h"  // For Taproot signing
#include "consensus/pq/p2mr_consensus.h"  // IsP2MRScript
#include "consensus/script_interpreter.h"  // SignatureHashTaproot, ScriptExecutionContext
#include "common/logger.h"
#include <algorithm>
#include <cstring>

namespace dinero {

// ═══════════════════════════════════════════════════════════════════════════
// Phase M.3: LegacyToHDWallet DELETED - no conversion needed
// ═══════════════════════════════════════════════════════════════════════════
// All wallet code now uses CanonicalWalletUTXO directly.
// The conversion function was removed as part of Phase M.3 lock enforcement.
// ═══════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════
// MapKeyProvider Implementation
// ═══════════════════════════════════════════════════════════════════════════

MapKeyProvider::MapKeyProvider(const std::map<std::string, std::string>& keys) {
    for (const auto& [address, hex_key] : keys) {
        // Convert hex string to bytes
        std::vector<uint8_t> key_bytes;
        for (size_t i = 0; i < hex_key.length(); i += 2) {
            std::string byte_str = hex_key.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
            key_bytes.push_back(byte);
        }
        keys_[address] = key_bytes;
    }
}

std::vector<uint8_t> MapKeyProvider::GetPrivateKey(const std::string& address) const {
    auto it = keys_.find(address);
    if (it != keys_.end()) {
        return it->second;
    }
    return {};
}

bool MapKeyProvider::HasKey(const std::string& address) const {
    return keys_.find(address) != keys_.end();
}

// ═══════════════════════════════════════════════════════════════════════════
// TransactionSigner Implementation
// ═══════════════════════════════════════════════════════════════════════════

SignResult TransactionSigner::Sign(
    const UnsignedTransaction& unsigned_tx,
    const KeyProvider& key_provider
) {
    SignResult result;

    // Copy transaction structure (inputs/outputs MUST NOT change)
    Transaction tx = unsigned_tx.tx;

    // Verify we have UTXOs for all inputs
    if (tx.vin.size() != unsigned_tx.selected_utxos.size()) {
        result.error = "Input count mismatch: " + std::to_string(tx.vin.size()) +
                       " inputs but " + std::to_string(unsigned_tx.selected_utxos.size()) + " UTXOs";
        return result;
    }

    // Sign each input
    std::vector<SignatureMetadata> signatures;
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        const CanonicalWalletUTXO& utxo = unsigned_tx.selected_utxos[i];  // Phase M.3: CanonicalWalletUTXO

        SignatureMetadata sig_meta;
        sig_meta.input_index = i;
        sig_meta.address = utxo.path;  // Phase M.3: Use path as identifier

        // P2MR (witness v3, BIP-360) dispatch — no ECDSA key to fetch.
        // The PQ seed is held behind KeyProvider::SignP2MR and expanded
        // into a canonical witness blob over the BIP-341 sighash. Sighash
        // uses the consensus-layer primitive so it is bit-identical to
        // what ValidateP2MRSpend will compute at mempool + block time.
        if (dinero::consensus::pq::IsP2MRScript(utxo.spk)) {
            dinero::consensus::ScriptExecutionContext sctx(
                &tx, static_cast<uint32_t>(i),
                utxo.value.GetUna(), /*flags=*/0);
            sctx.all_amounts.reserve(unsigned_tx.selected_utxos.size());
            sctx.all_scriptpubkeys.reserve(unsigned_tx.selected_utxos.size());
            sctx.all_confidential_flags.reserve(unsigned_tx.selected_utxos.size());
            sctx.all_input_commitments.reserve(unsigned_tx.selected_utxos.size());
            for (const auto& u : unsigned_tx.selected_utxos) {
                sctx.all_amounts.push_back(u.value.GetUna());
                sctx.all_scriptpubkeys.push_back(u.spk);
                sctx.all_confidential_flags.push_back(u.is_confidential ? 1 : 0);
                sctx.all_input_commitments.push_back(u.commitment);
            }
            std::vector<uint8_t> leaf_hash;
            std::vector<uint8_t> sighash_vec =
                dinero::consensus::SignatureHashTaproot(sctx, /*hash_type=*/0x00, leaf_hash);
            if (sighash_vec.size() != 32) {
                sig_meta.is_signed = false;
                sig_meta.error = "P2MR sighash computation failed at input " + std::to_string(i);
                signatures.push_back(sig_meta);
                result.error = sig_meta.error;
                return result;
            }
            std::array<uint8_t, 32> sighash32{};
            std::memcpy(sighash32.data(), sighash_vec.data(), 32);

            std::vector<uint8_t> witness_bytes = key_provider.SignP2MR(utxo.spk, sighash32);
            if (witness_bytes.empty()) {
                sig_meta.is_signed = false;
                sig_meta.error = "KeyProvider could not sign P2MR input " + std::to_string(i);
                signatures.push_back(sig_meta);
                result.error = sig_meta.error;
                return result;
            }

            // Verify the blob is consensus-decodable before stamping it
            // onto the witness. If a provider produces a structurally
            // invalid or non-canonical blob, we catch it here rather than
            // letting it hit mempool/block validation.
            dinero::consensus::pq::P2MRWitness decoded{};
            auto dec = dinero::consensus::pq::DeserializeP2MRWitness(witness_bytes, &decoded);
            if (dec != dinero::consensus::pq::P2MRWitnessDecodeError::Ok) {
                sig_meta.is_signed = false;
                sig_meta.error = "P2MR witness blob failed decode at input " + std::to_string(i)
                    + " (code " + std::to_string(static_cast<int>(dec)) + ")";
                signatures.push_back(sig_meta);
                result.error = sig_meta.error;
                return result;
            }

            tx.vin[i].scriptSig.clear();
            tx.vin[i].witness.clear();
            tx.vin[i].witness.push_back(std::move(witness_bytes));

            sig_meta.is_signed = true;
            signatures.push_back(sig_meta);
            continue;
        }

        // Check if we have the key (using path as identifier)
        if (!key_provider.HasKey(utxo.path)) {
            sig_meta.is_signed = false;
            sig_meta.error = "Missing private key for path: " + utxo.path;
            signatures.push_back(sig_meta);
            result.error = sig_meta.error;
            return result;
        }

        // Get private key
        std::vector<uint8_t> private_key = key_provider.GetPrivateKey(utxo.path);
        if (private_key.empty()) {
            sig_meta.is_signed = false;
            sig_meta.error = "Failed to retrieve private key for: " + utxo.path;
            signatures.push_back(sig_meta);
            result.error = sig_meta.error;
            return result;
        }

        // Phase M.3: No conversion needed - utxo is already CanonicalWalletUTXO

        // Sign input - pass FULL UTXO set (BIP341 requires all inputs for sighash)
        bool sign_success = SignInput(tx, i, unsigned_tx.selected_utxos, private_key);
        if (!sign_success) {
            sig_meta.is_signed = false;
            sig_meta.error = "Failed to sign input " + std::to_string(i);
            signatures.push_back(sig_meta);
            result.error = sig_meta.error;
            return result;
        }

        sig_meta.is_signed = true;
        signatures.push_back(sig_meta);
    }

    // Populate result
    result.success = true;
    result.signed_tx.tx = tx;
    result.signed_tx.signatures = signatures;
    result.signed_tx.fee = unsigned_tx.fee;
    result.signed_tx.change_amount = unsigned_tx.change_amount;

    // Local validation
    std::string validation_error;
    if (!ValidateLocally(result.signed_tx, validation_error)) {
        result.success = false;
        result.error = "Local validation failed: " + validation_error;
        return result;
    }

    // Verify txid unchanged (structure must be identical)
    if (!VerifyTxidUnchanged(unsigned_tx, result.signed_tx)) {
        result.success = false;
        result.error = "CRITICAL: Signing changed transaction structure (txid mismatch)";
        g_logger.error("[TransactionSigner] " + result.error);
        return result;
    }

    g_logger.info("[TransactionSigner] Successfully signed transaction with " +
                  std::to_string(tx.vin.size()) + " inputs");

    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
// Local Validation (Wallet-Side Sanity Checks)
// ═══════════════════════════════════════════════════════════════════════════

bool TransactionSigner::ValidateLocally(
    const SignedTransaction& signed_tx,
    std::string& error
) {
    const Transaction& tx = signed_tx.tx;

    // Check: Transaction has inputs and outputs
    if (tx.vin.empty()) {
        error = "Transaction has no inputs";
        return false;
    }

    if (tx.vout.empty()) {
        error = "Transaction has no outputs";
        return false;
    }

    // Check: All inputs have witness data (SegWit requirement)
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        const TxInput& input = tx.vin[i];

        // SegWit: scriptSig must be empty, witness must be non-empty
        if (!input.scriptSig.empty()) {
            error = "Input " + std::to_string(i) + " has non-empty scriptSig (not SegWit)";
            return false;
        }

        if (input.witness.empty()) {
            error = "Input " + std::to_string(i) + " has empty witness (not signed)";
            return false;
        }

        // Validate witness structure based on size:
        // - P2TR (Taproot) key-path: 1 item [64-byte Schnorr sig]
        // - P2MR (witness v3):       1 item [canonical blob ~5.3 KB]
        // - P2WPKH (SegWit v0):      2 items [signature, pubkey]
        if (input.witness.size() == 1) {
            if (input.witness[0].empty()) {
                error = "Input " + std::to_string(i) + " has empty witness element";
                return false;
            }
        } else if (input.witness.size() == 2) {
            // P2WPKH witness: [signature, pubkey]
            if (input.witness[0].empty()) {
                error = "Input " + std::to_string(i) + " has empty signature";
                return false;
            }
            if (input.witness[1].empty()) {
                error = "Input " + std::to_string(i) + " has empty pubkey";
                return false;
            }
        } else {
            error = "Input " + std::to_string(i) + " has invalid witness size " +
                    std::to_string(input.witness.size()) + " (expected 1 for P2TR or 2 for P2WPKH)";
            return false;
        }
    }

    // Check: All outputs have non-zero value (prevent accidental burns)
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        if (tx.vout[i].value == dinero::AmountUna::Zero()) {
            error = "Output " + std::to_string(i) + " has zero value";
            return false;
        }
    }

    // Check: Fee is reasonable (sanity check, not policy)
    // Fee should be > 0 and < total output value
    uint64_t total_output = 0;
    for (const auto& output : tx.vout) {
        total_output += output.value.GetUna();
    }

    if (signed_tx.fee == 0) {
        error = "Fee is zero";
        return false;
    }

    // Warn if fee > 10% of total output (likely error)
    if (signed_tx.fee > total_output / 10) {
        g_logger.warning("[TransactionSigner] Fee " + std::to_string(signed_tx.fee) +
                        " is > 10% of output " + std::to_string(total_output));
    }

    return true;
}

bool TransactionSigner::VerifyTxidUnchanged(
    const UnsignedTransaction& unsigned_tx,
    const SignedTransaction& signed_tx
) {
    // Compute txid of unsigned transaction - Phase M.4: GetTxid() returns TxId semantic type
    TxId unsigned_txid = unsigned_tx.tx.GetTxid();

    // Compute txid of signed transaction
    // NOTE: Txid is computed from non-witness data only (BIP141)
    // Witness data does NOT change txid
    TxId signed_txid = signed_tx.tx.GetTxid();

    if (unsigned_txid != signed_txid) {
        g_logger.error("[TransactionSigner] Txid mismatch: unsigned=" + unsigned_txid.AsUint256().GetHex() +
                      ", signed=" + signed_txid.AsUint256().GetHex());
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// Private Helpers
// ═══════════════════════════════════════════════════════════════════════════

bool TransactionSigner::SignInput(
    Transaction& tx,
    size_t input_index,
    const std::vector<CanonicalWalletUTXO>& all_utxos,
    const std::vector<uint8_t>& private_key
) {
    if (input_index >= all_utxos.size()) {
        return false;
    }

    const CanonicalWalletUTXO& utxo = all_utxos[input_index];

    // Detect UTXO type and use appropriate signer
    if (TaprootTxSigner::IsTaprootUTXO(utxo)) {
        // Use BIP341 Taproot signing for P2TR inputs
        // BIP341 REQUIRES the full UTXO set for sighash computation
        return TaprootTxSigner::SignInput(tx, input_index, all_utxos, private_key);
    } else {
        // Use BIP143 SegWit v0 signing for P2WPKH inputs
        // Note: BIP143 only needs the single UTXO being spent
        return BIP143Signer::SignInput(tx, input_index, utxo, private_key);
    }
}

bool TransactionSigner::ValidateWitness(
    const Transaction& tx,
    std::string& error
) {
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        const TxInput& input = tx.vin[i];

        if (input.witness.empty()) {
            error = "Input " + std::to_string(i) + " missing witness data";
            return false;
        }

        // P2TR key-path: 1 item [64-byte Schnorr signature]
        // P2WPKH: 2 items [DER signature, compressed pubkey]
        if (input.witness.size() != 1 && input.witness.size() != 2) {
            error = "Input " + std::to_string(i) + " has invalid witness count " +
                    std::to_string(input.witness.size()) +
                    " (expected 1 for P2TR or 2 for P2WPKH)";
            return false;
        }
    }

    return true;
}

} // namespace dinero
