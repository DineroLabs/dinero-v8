#pragma once

// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║                    WALLET LAYER - BIP143 SIGNING                           ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║                                                                           ║
// ║  This module handles SIGNING (wallet operation).                          ║
// ║  Sighash COMPUTATION is delegated to consensus/crypto/sighash_bip143.h    ║
// ║                                                                           ║
// ║  Direction of trust: Wallet → Consensus                                   ║
// ║    - Wallet calls consensus for sighash computation                       ║
// ║    - Wallet owns signing keys and UTXO ownership verification             ║
// ║                                                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

#include "wallet/transaction.h"
#include "wallet/hd_wallet.h"
#include "consensus/crypto/sighash_bip143.h"  // Sighash computation (consensus)
#include <vector>
#include <string>

namespace dinero {

/**
 * BIP143 Signing - Wallet layer
 *
 * Uses consensus::SighashBIP143 for hash computation.
 * Owns signing keys and UTXO ownership verification.
 */
class BIP143Signer {
public:
    // Re-export SIGHASH constants from consensus for convenience
    static constexpr uint32_t SIGHASH_ALL = consensus::SighashBIP143::SIGHASH_ALL;
    static constexpr uint32_t SIGHASH_NONE = consensus::SighashBIP143::SIGHASH_NONE;
    static constexpr uint32_t SIGHASH_SINGLE = consensus::SighashBIP143::SIGHASH_SINGLE;
    static constexpr uint32_t SIGHASH_ANYONECANPAY = consensus::SighashBIP143::SIGHASH_ANYONECANPAY;

    /**
     * Sign a SegWit transaction
     * Returns true if all inputs were signed successfully
     */
    static bool SignTransaction(
        Transaction& tx,
        const std::vector<CanonicalWalletUTXO>& utxos,
        const std::vector<std::vector<uint8_t>>& private_keys
    );

    /**
     * Compute BIP143 sighash for a single input
     * DELEGATES to consensus::SighashBIP143::ComputeSighash
     */
    static std::vector<uint8_t> ComputeSighash(
        const Transaction& tx,
        size_t input_index,
        const std::vector<uint8_t>& scriptCode,
        uint64_t input_value,
        uint32_t sighash_type = SIGHASH_ALL
    ) {
        // Delegate to consensus layer
        return consensus::SighashBIP143::ComputeSighash(tx, input_index, scriptCode, input_value, sighash_type);
    }

    /**
     * Sign a single input
     * Verifies UTXO ownership before signing
     */
    static bool SignInput(
        Transaction& tx,
        size_t input_index,
        const CanonicalWalletUTXO& utxo,
        const std::vector<uint8_t>& private_key
    );
    
private:
    // ECDSA signing with secp256k1
    static std::vector<uint8_t> SignECDSA(
        const std::vector<uint8_t>& message_hash,
        const std::vector<uint8_t>& private_key
    );

    // Get public key from private key
    static std::vector<uint8_t> GetPublicKey(
        const std::vector<uint8_t>& private_key
    );
};

} // namespace dinero
