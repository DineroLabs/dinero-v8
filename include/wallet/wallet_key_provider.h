#pragma once
/**
 * Wallet-layer KeyProvider that handles BOTH legacy ECDSA/Schnorr
 * inputs (P2WPKH, P2TR) and v7 post-quantum P2MR inputs (ML-DSA-65,
 * witness v3).
 *
 * Legacy path:
 *   - Identical to MapKeyProvider. An in-memory `derivation_path → hex`
 *     map is consulted when TransactionSigner needs a 32-byte secp256k1
 *     private key. Path-keyed, not address-keyed — matches what
 *     deriveKeyForScriptPubKey + getPrivateKeyForPath hand to the
 *     wallet send flow today.
 *
 * PQ path:
 *   - Extracts the 32-byte merkle_root from the P2MR scriptPubKey,
 *     looks up the stored row in V7P2MRStore by (wallet_id, merkle_root),
 *     decrypts the encrypted seed with a wallet-session master key,
 *     re-derives the ML-DSA-65 keypair, signs the BIP-341 sighash, and
 *     serializes a canonical P2MRWitness. Returns the ready-to-stuff
 *     witness blob.
 *
 * Security:
 *   - The provider holds a copy of the wallet's AeadKey in memory.
 *     The destructor OPENSSL_cleanse's it on every path.
 *   - The V7P2MRStore pointer is non-owning. Caller guarantees it
 *     outlives the provider (true by construction in the send flow —
 *     the store is opened on the local stack, the provider is built
 *     alongside, both live for the scope of one RPC call).
 */

#include "wallet/transaction_signer.h"  // KeyProvider
#include "wallet/aead_seed.h"            // AeadKey

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dinero::wallet {

class V7P2MRStore;  // forward decl

class WalletKeyProvider : public ::dinero::KeyProvider {
public:
    struct Config {
        /** derivation_path → hex-encoded 32-byte secp256k1 private key. */
        std::map<std::string, std::string> legacy_keys_by_path;

        /** V7 P2MR SQLite store. Non-owning; must outlive the provider. */
        V7P2MRStore*                       p2mr_store = nullptr;

        /** Row scope for the store lookup. Single-wallet today → 1. */
        int64_t                            wallet_id  = 1;

        /** Wallet AEAD master key (32 bytes). Copied in; scrubbed on drop. */
        AeadKey                            master_key{};
    };

    explicit WalletKeyProvider(Config cfg);
    ~WalletKeyProvider() override;

    WalletKeyProvider(const WalletKeyProvider&)            = delete;
    WalletKeyProvider& operator=(const WalletKeyProvider&) = delete;
    WalletKeyProvider(WalletKeyProvider&&)                 = delete;
    WalletKeyProvider& operator=(WalletKeyProvider&&)      = delete;

    // KeyProvider — legacy ECDSA/Schnorr path
    std::vector<uint8_t> GetPrivateKey(const std::string& path) const override;
    bool                 HasKey(const std::string& path) const override;

    // KeyProvider — PQ P2MR path
    std::vector<uint8_t> SignP2MR(
        const std::vector<uint8_t>&      script_pubkey,
        const std::array<uint8_t, 32>&   sighash) const override;

private:
    std::map<std::string, std::vector<uint8_t>> legacy_keys_;
    V7P2MRStore*                                 p2mr_store_;
    int64_t                                      wallet_id_;
    mutable AeadKey                              master_key_;  ///< scrubbed in dtor
};

} // namespace dinero::wallet
