/*
 * P2MR signing — implementation (moved from src/rpc/v7_pq_handlers.cpp,
 * wallet/node separation Task 2.5). SignP2MR's body is moved verbatim.
 * The file-local helpers it uses are verbatim COPIES of the ones in
 * v7_pq_handlers.cpp's anonymous namespace (still used there by
 * GetNewP2MRAddress, ImportP2MRSeed and ExportP2MRSeed).
 */

#include "wallet/p2mr_signing.h"

#include <exception>

#include "consensus/pq/ml_dsa_65.h"
#include "consensus/pq/scheme_registry.h"
#include "crypto/sha256.h"
#include "wallet/aead_seed.h"
#include "wallet/p2mr_address.h"
#include "wallet/pq_derivation.h"
#include "wallet/secure_keypair.h"
#include "wallet/v7_p2mr_store.h"

#include <cstring>
#include <openssl/crypto.h>
#include <optional>
#include <string>

namespace dinero::rpc::v7 {

namespace {

namespace mldsa = dinero::consensus::pq::ml_dsa_65;
namespace pq    = dinero::consensus::pq;
namespace wlt   = dinero::wallet;
namespace wpq   = dinero::wallet::pq;

void Scrub(void* p, std::size_t n) noexcept { OPENSSL_cleanse(p, n); }

std::string BuildDerivationPath(int account, int change, int address_index) {
    // Matches V7_WALLET_SCHEMA §1: m/88'/1448'/acct'/chg/addr_idx.
    // Purpose 88 = P2MR (post-quantum). See docs/wallet/DINERO_HD_WALLET_SPEC.md.
    return "m/88'/1448'/" + std::to_string(account) + "'/"
         + std::to_string(change) + "/"
         + std::to_string(address_index);
}

std::array<uint8_t, 32> LeafHash(uint8_t scheme_id, const mldsa::PublicKey& pk) {
    std::array<uint8_t, 32> out{};
    dinero::crypto::CSHA256()
        .Write(&scheme_id, 1)
        .Write(pk.data(), pk.size())
        .Finalize(out.data());
    return out;
}

HandlerStatus MapAddResult(wlt::V7P2MRStore::AddResult rc) {
    switch (rc) {
        case wlt::V7P2MRStore::AddResult::Ok:             return HandlerStatus::Ok;
        case wlt::V7P2MRStore::AddResult::UniqueConflict: return HandlerStatus::UniqueConflict;
        case wlt::V7P2MRStore::AddResult::DbError:        return HandlerStatus::StoreError;
    }
    return HandlerStatus::InternalError;
}

// This guard also covers exceptions before a typed result can be returned.
struct ScopedMasterKey {
    wlt::AeadKey& key;
    ~ScopedMasterKey() { Scrub(key.data(), key.size()); }
};

HandlerStatus OpenBoundKey(const wlt::V7P2MRStore& store, int64_t wallet_id,
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

} // namespace

// ---------------------------------------------------------------------------
// wallet.signp2mr
// ---------------------------------------------------------------------------
SignP2MRResult SignP2MR(const wlt::V7P2MRStore& store, SignP2MRParams params) {
    SignP2MRResult out;
    ScopedMasterKey master_guard{params.master_key};
    wlt::SecureSeed seed;
    std::optional<wlt::SecureKeypair> key;
    try {
        out.status = OpenBoundKey(store, params.wallet_id, params.address,
                                  params.master_key, seed, key);
        if (out.status != HandlerStatus::Ok) {
            out.error_message = "P2MR signing key or stored address binding unavailable";
            return out;
        }
        auto signature = mldsa::Sign(params.sighash.data(), params.sighash.size(), key->secret());
        out.scheme_id = pq::SCHEME_ID_ML_DSA_65;
        out.pubkey = key->pubkey();
        out.signature = signature;
    } catch (const std::exception&) {
        out.status = HandlerStatus::InternalError;
        out.error_message = "P2MR signing failed";
    }
    return out;
}

} // namespace dinero::rpc::v7
