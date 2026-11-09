/*
 * P2MR key-handling helpers shared by src/rpc/v7_pq_handlers.cpp and
 * src/wallet/p2mr_signing.cpp (wallet/node separation Task 2.5, stage-2
 * review follow-up: one text source instead of two drifting copies).
 *
 * CONTRACT: this file is textually included INSIDE an anonymous namespace
 * within namespace dinero::rpc::v7, after these headers are included:
 *   consensus/pq/ml_dsa_65.h, consensus/pq/scheme_registry.h,
 *   crypto/sha256.h, wallet/aead_seed.h, wallet/p2mr_address.h,
 *   wallet/secure_keypair.h, wallet/v7_p2mr_store.h, wallet/p2mr_signing.h
 *   (for HandlerStatus), <openssl/crypto.h>, <optional>, <string>.
 * It defines internal-linkage helpers per translation unit; it is not a
 * header and must not be installed or included anywhere else.
 */

namespace mldsa = dinero::consensus::pq::ml_dsa_65;
namespace pq    = dinero::consensus::pq;
namespace wlt   = dinero::wallet;
namespace wpq   = dinero::wallet::pq;

inline void Scrub(void* p, std::size_t n) noexcept { OPENSSL_cleanse(p, n); }

inline std::array<uint8_t, 32> LeafHash(uint8_t scheme_id, const mldsa::PublicKey& pk) {
    std::array<uint8_t, 32> out{};
    dinero::crypto::CSHA256()
        .Write(&scheme_id, 1)
        .Write(pk.data(), pk.size())
        .Finalize(out.data());
    return out;
}

// This guard also covers exceptions before a typed result can be returned.
struct ScopedMasterKey {
    wlt::AeadKey& key;
    ~ScopedMasterKey() { Scrub(key.data(), key.size()); }
};

inline HandlerStatus OpenBoundKey(const wlt::V7P2MRStore& store, int64_t wallet_id,
                                  const std::string& address, const wlt::AeadKey& master,
                                  wlt::SecureSeed& seed, std::optional<wlt::SecureKeypair>& key) {
    const auto decoded = wlt::DecodeP2MRAddress(address);
    if (!decoded) return HandlerStatus::InvalidParams;
    std::optional<wlt::V7P2MRStore::KeyRecord> captured;
    try {
        captured = store.CaptureKeyByAddress(wallet_id, address);
    } catch (const std::exception&) {
        return HandlerStatus::StoreError;
    }
    if (!captured) return HandlerStatus::AddressNotFound;
    const auto& encrypted = captured->encrypted_seed;
    if (wlt::OpenSeedSecure(encrypted.ciphertext, encrypted.nonce, encrypted.tag,
                            master, &seed) != wlt::AeadOpenResult::Ok)
        return HandlerStatus::DecryptFailed;
    key.emplace(mldsa::KeygenFromSeed(seed.bytes()));
    const auto root = LeafHash(pq::SCHEME_ID_ML_DSA_65, key->pubkey());
    if (key->pubkey() != captured->metadata.pubkey ||
        root != captured->metadata.merkle_root || root != decoded->merkle_root)
        return HandlerStatus::DerivationMismatch;
    return HandlerStatus::Ok;
}
