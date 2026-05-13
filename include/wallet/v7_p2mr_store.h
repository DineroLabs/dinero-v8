#pragma once
/**
 * V7 P2MR wallet store — SQLite-backed persistence for P2MR addresses
 * and their encrypted seeds.
 *
 * Spec: docs/consensus/V7_WALLET_SCHEMA.md §2 "Wallet Storage Schema".
 *
 * Design intent:
 *   - Keeps v7-specific data in its OWN table (v7_p2mr_addresses) so a
 *     wallet DB holding v5 keys + v7 keys during the migration window
 *     doesn't need a disruptive schema rewrite.
 *   - Stores only the 32-byte pq_seed (encrypted) + public-side metadata.
 *     The 4032-byte ML-DSA secret is never written to disk. To sign, the
 *     wallet decrypts the seed, calls KeygenFromSeed, drops the resulting
 *     SecureKeypair immediately after.
 *   - The master key is caller-managed. This library does NOT derive it
 *     from a password — that's the wallet-manager's job (Argon2id over
 *     the user's passphrase, same stack v5 already uses).
 *
 * This header is wallet-layer only. Nothing here is consensus-critical.
 */

#include "consensus/pq/ml_dsa_65.h"
#include "wallet/aead_seed.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;  // opaque

namespace dinero::wallet {

/** Public view of a stored P2MR address. Never exposes secret material. */
struct P2MRStoredAddress {
    int64_t                                                          id;
    int64_t                                                          wallet_id;
    std::string                                                      address;         ///< "din1r..."
    std::array<uint8_t, 32>                                          merkle_root;
    std::array<uint8_t, dinero::consensus::pq::ml_dsa_65::PUBKEY_BYTES> pubkey;
    std::string                                                      derivation_path; ///< "m/88'/1448'/acct'/chg/idx"
    uint32_t                                                         leaf_index;
    std::string                                                      label;
    int64_t                                                          created_at_unix;
};

class V7P2MRStore {
public:
    enum class OpenResult : uint8_t {
        Ok            = 0,
        IoError       = 1,   ///< sqlite3_open failed
        SchemaError   = 2,   ///< migration DDL failed
    };

    /**
     * Open or create a SQLite DB at `path`. Creates the
     * v7_p2mr_addresses table + indices if they don't exist.
     * Idempotent — safe to call on an already-initialized DB.
     */
    OpenResult Open(const std::string& path);

    /** Close the DB. Destructor does this automatically. */
    void Close() noexcept;
    ~V7P2MRStore() { Close(); }

    V7P2MRStore() = default;
    V7P2MRStore(const V7P2MRStore&) = delete;
    V7P2MRStore& operator=(const V7P2MRStore&) = delete;

    enum class AddResult : uint8_t {
        Ok                    = 0,
        DbError               = 1,
        UniqueConflict        = 2,   ///< (wallet_id, path, leaf) already present
    };

    /**
     * Insert a new P2MR address. Pre-encrypt the seed with SealSeed
     * and pass all three AEAD fields here. `now_unix` is the timestamp
     * stored in the row; callers typically pass time(nullptr).
     */
    AddResult AddAddress(int64_t                 wallet_id,
                         const std::string&      address,
                         const std::array<uint8_t, 32>& merkle_root,
                         const std::array<uint8_t, dinero::consensus::pq::ml_dsa_65::PUBKEY_BYTES>& pubkey,
                         const AeadCiphertext&   seed_ciphertext,
                         const AeadNonce&        seed_nonce,
                         const AeadTag&          seed_tag,
                         const std::string&      derivation_path,
                         uint32_t                leaf_index,
                         const std::string&      label,
                         int64_t                 now_unix);

    /**
     * Look up by bech32m address. Returns nullopt if absent.
     * Never returns secret material.
     */
    std::optional<P2MRStoredAddress>
    GetByAddress(int64_t wallet_id, const std::string& address) const;

    /**
     * Look up by 32-byte Merkle root — the opaque commitment stored in
     * every P2MR scriptPubKey (bytes 2..34 of the 34-byte output script).
     *
     * Uses idx_v7_p2mr_addresses_merkle_root. Intended for the spender
     * path, where a consumed UTXO's scriptPubKey is the only handle we
     * have on the row; re-encoding merkle_root → bech32m → address would
     * require threading chain-params HRP into the signer, coupling the
     * signing layer to chain awareness it otherwise does not need.
     *
     * Returns nullopt if absent. Never returns secret material.
     */
    std::optional<P2MRStoredAddress>
    GetByMerkleRoot(int64_t wallet_id,
                    const std::array<uint8_t, 32>& merkle_root) const;

    /**
     * List all P2MR addresses for a given wallet_id, ordered by created_at
     * ascending. Intended for wallet UI / RPC listing.
     */
    std::vector<P2MRStoredAddress> ListByWallet(int64_t wallet_id) const;

    /**
     * Load the encrypted-seed fields for a stored address so the caller
     * can decrypt via OpenSeed and then call KeygenFromSeed. Returns
     * nullopt if the address is not present in the store.
     *
     * The caller retains responsibility for scrubbing the decrypted seed
     * after use.
     */
    struct EncryptedSeed {
        AeadCiphertext ciphertext;
        AeadNonce      nonce;
        AeadTag        tag;
    };
    std::optional<EncryptedSeed>
    LoadEncryptedSeed(int64_t wallet_id, const std::string& address) const;

private:
    sqlite3* db_ = nullptr;
};

} // namespace dinero::wallet
