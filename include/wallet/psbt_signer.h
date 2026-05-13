#pragma once
#include "dinero/core/wallet/psbt.h"
#include "wallet/schnorr_signer.h"
#include "wallet/taproot_sighash.h"
#include "wallet/wallet_iface.h"
#include <vector>
#include <cstdint>
#include <memory>
#include <string>
#include <algorithm>

namespace din {

/**
 * @brief Result of PSBT signing operation with detailed error reporting
 */
struct PsbtSignResult {
    size_t signed_count = 0;
    bool success = true;
    std::string error;  // Fatal error that stopped signing

    struct InputError {
        size_t input_index;
        std::string error;
        std::string severity;  // "error" or "warning"
    };
    std::vector<InputError> input_errors;

    // Helper: Add input error
    void addInputError(size_t idx, const std::string& err, const std::string& sev = "error") {
        input_errors.push_back({idx, err, sev});
    }

    // Helper: Check if has errors
    bool hasErrors() const {
        return !error.empty() ||
               std::any_of(input_errors.begin(), input_errors.end(),
                          [](const InputError& e) { return e.severity == "error"; });
    }
};

/**
 * @brief Software PSBT signer for P2WPKH and P2PKH inputs
 */
class PsbtSigner {
public:
    explicit PsbtSigner(std::shared_ptr<IKeyStore> keystore, std::string wallet_policy = "bip84");

    /**
     * @brief Sign PSBT inputs with available keys
     *
     * Attempts to sign all inputs where:
     * - We have the private key
     * - Input type is supported (P2WPKH, P2PKH, P2TR)
     * - Required UTXO information is present
     *
     * @param psbt PSBT to sign (modified in place)
     * @return PsbtSignResult with detailed error information
     */
    PsbtSignResult signPsbt(Psbt& psbt);

    /**
     * @brief Sign PSBT inputs (legacy interface - returns count only)
     * @deprecated Use signPsbt() for detailed error reporting
     */
    size_t signPsbtLegacy(Psbt& psbt);

    /**
     * @brief Check if signer has key for given pubkey
     */
    bool hasKey(const std::vector<uint8_t>& pubkey33) const;

private:
    std::shared_ptr<IKeyStore> keystore_;
    std::string wallet_policy_;
    
    /**
     * @brief Sign a single P2WPKH input
     * 
     * @param psbt PSBT containing the input
     * @param input_idx Index of input to sign
     * @param pubkey33 33-byte compressed public key
     * @param privkey32 32-byte private key
     * @param utxo_value Value of the UTXO being spent
     * @param utxo_script ScriptPubKey of the UTXO being spent
     * @param sighash_type Sighash type (default SIGHASH_ALL)
     * @return true if signing succeeded
     */
    bool signP2WPKH(
        Psbt& psbt,
        size_t input_idx,
        const std::vector<uint8_t>& pubkey33,
        const std::vector<uint8_t>& privkey32,
        uint64_t utxo_value,
        const std::vector<uint8_t>& utxo_script,
        uint32_t sighash_type = 1 // SIGHASH_ALL
    );
    
    /**
     * @brief Sign a single P2PKH input
     * 
     * @param psbt PSBT containing the input
     * @param input_idx Index of input to sign
     * @param pubkey33 33-byte compressed public key
     * @param privkey32 32-byte private key
     * @param prev_tx_bytes Full previous transaction bytes
     * @param prev_output_idx Index of output in previous transaction
     * @param sighash_type Sighash type (default SIGHASH_ALL)
     * @return true if signing succeeded
     */
    bool signP2PKH(
        Psbt& psbt,
        size_t input_idx,
        const std::vector<uint8_t>& pubkey33,
        const std::vector<uint8_t>& privkey32,
        const std::vector<uint8_t>& prev_tx_bytes,
        uint32_t prev_output_idx,
        uint32_t sighash_type = 1 // SIGHASH_ALL
    );
    
    /**
     * @brief Sign P2TR input using Schnorr signature
     * 
     * @param psbt PSBT to sign
     * @param input_idx Input index
     * @param pubkey32 32-byte x-only public key
     * @param privkey32 32-byte private key
     * @param tapleaf_hash Optional tapleaf hash for script path
     * @param sighash_type Sighash type
     * @return true if signing successful
     */
    bool signP2TR(
        Psbt& psbt,
        size_t input_idx,
        const std::vector<uint8_t>& pubkey32,
        const std::vector<uint8_t>& privkey32,
        const std::optional<std::vector<uint8_t>>& tapleaf_hash = std::nullopt,
        TaprootSighash::SighashType sighash_type = TaprootSighash::SighashType::DEFAULT
    );
    
    /**
     * @brief Create BIP-143 sighash for SegWit inputs
     */
    std::vector<uint8_t> createSegwitSighash(
        const std::vector<uint8_t>& unsigned_tx,
        size_t input_idx,
        const std::vector<uint8_t>& script_code,
        uint64_t value,
        uint32_t sighash_type
    );
    
    /**
     * @brief Create legacy sighash for P2PKH inputs
     */
    std::vector<uint8_t> createLegacySighash(
        const std::vector<uint8_t>& unsigned_tx,
        size_t input_idx,
        const std::vector<uint8_t>& script_pubkey,
        uint32_t sighash_type
    );
    
    /**
     * @brief Sign hash with secp256k1 and return DER signature
     */
    std::vector<uint8_t> signHash(
        const std::vector<uint8_t>& hash32,
        const std::vector<uint8_t>& privkey32
    );
    
    /**
     * @brief Determine input type from PSBT input fields
     */
    enum class InputType {
        Unknown,
        P2WPKH,
        P2SH_P2WPKH,
        P2PKH,
        P2TR
    };
    
    InputType detectInputType(const PsbtInput& input);
};

/**
 * @brief Simple concrete keystore for production use
 * 
 * This is a minimal implementation that can be used in production
 * until proper wallet integration is complete.
 */
class SimpleKeyStore : public IKeyStore {
public:
    SimpleKeyStore() = default;
    
    // IKeyStore interface implementation
    std::optional<std::string> getXPub(const std::string& path) const override {
        return std::nullopt; // No keys available
    }
    
    std::optional<std::string> getXPriv(const std::string& path) const override {
        return std::nullopt; // No keys available
    }
    
    std::optional<std::vector<uint8_t>> sign(const std::vector<uint8_t>& hash, 
                                            const std::string& key_path) override {
        return std::nullopt; // No keys available
    }
    
    bool canSign(const std::string& key_path) const override {
        return false; // No keys available
    }
    
    bool hasKey(const std::string& key_path) const override {
        return false; // No keys available
    }
    
    std::vector<std::string> listKeyPaths() const override {
        return {}; // No keys available
    }
};

} // namespace din
