/*
 * V7 PQ wallet RPC handlers — implementation.
 * See include/rpc/v7_pq_handlers.h for the contract and security notes.
 *
 * Discipline this file enforces:
 *
 *   - Every branch that touches plaintext secret material wipes it before
 *     exiting (OPENSSL_cleanse or SecureSeed/SecureKeypair RAII).
 *   - Nothing is persisted without going through V7P2MRStore, which holds
 *     the schema invariants (UNIQUE, encryption layout).
 *   - Handlers NEVER return raw seed bytes except ExportP2MRSeed.
 *   - Status codes are precise so the dispatcher can map cleanly to
 *     JSON-RPC error codes / user-facing strings without introspection.
 */

#include "rpc/v7_pq_handlers.h"

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
// wallet.getnewp2mraddress
// ---------------------------------------------------------------------------
GetNewP2MRAddressResult GetNewP2MRAddress(wlt::V7P2MRStore&       store,
                                          GetNewP2MRAddressParams params) {
    GetNewP2MRAddressResult out;

    if (params.hrp.empty()) {
        out.status = HandlerStatus::InvalidParams;
        out.error_message = "hrp is empty";
        Scrub(params.master_key.data(),  params.master_key.size());
        Scrub(params.bip32_priv.data(),  params.bip32_priv.size());
        Scrub(params.bip32_chain.data(), params.bip32_chain.size());
        return out;
    }

    // 1. HKDF → pq_seed; KeygenFromSeed → SecureKeypair.
    //    DerivePQKeypair scrubs ikm/seed intermediates internally.
    //    We still need the pq_seed itself for storage, so call DerivePQSeed
    //    separately. The seed is kept in a local array that we scrub.
    auto pq_seed = wpq::DerivePQSeed(params.bip32_priv, params.bip32_chain,
                                     params.leaf_index);
    mldsa::Seed ml_seed{};
    std::memcpy(ml_seed.data(), pq_seed.data(), ml_seed.size());
    wlt::SecureKeypair kp(mldsa::KeygenFromSeed(ml_seed));
    Scrub(ml_seed.data(), ml_seed.size());

    out.pubkey          = kp.pubkey();
    out.merkle_root     = LeafHash(pq::SCHEME_ID_ML_DSA_65, kp.pubkey());
    out.derivation_path = BuildDerivationPath(params.account, params.change,
                                              params.address_index);
    out.leaf_index      = params.leaf_index;
    out.address         = wlt::EncodeP2MRAddress(params.hrp, out.merkle_root);

    if (out.address.empty()) {
        out.status = HandlerStatus::InvalidParams;
        out.error_message = "invalid hrp; address encoding failed";
        Scrub(pq_seed.data(), pq_seed.size());
        Scrub(params.master_key.data(),  params.master_key.size());
        Scrub(params.bip32_priv.data(),  params.bip32_priv.size());
        Scrub(params.bip32_chain.data(), params.bip32_chain.size());
        return out;
    }

    // 2. Encrypt pq_seed under master_key.
    wlt::AeadSeed plain{};
    std::memcpy(plain.data(), pq_seed.data(), plain.size());
    wlt::AeadSealOutput sealed{};
    try {
        sealed = wlt::SealSeed(plain, params.master_key);
    } catch (const std::exception&) {
        out.status = HandlerStatus::InternalError;
        out.error_message = "SealSeed failed";
        Scrub(plain.data(),   plain.size());
        Scrub(pq_seed.data(), pq_seed.size());
        Scrub(params.master_key.data(),  params.master_key.size());
        Scrub(params.bip32_priv.data(),  params.bip32_priv.size());
        Scrub(params.bip32_chain.data(), params.bip32_chain.size());
        return out;
    }
    Scrub(plain.data(),   plain.size());
    Scrub(pq_seed.data(), pq_seed.size());

    // 3. Persist.
    auto rc = store.AddAddress(
        params.wallet_id, out.address, out.merkle_root, kp.pubkey(),
        sealed.ciphertext, sealed.nonce, sealed.tag,
        out.derivation_path, out.leaf_index, params.label, params.now_unix);
    out.status = MapAddResult(rc);
    if (out.status != HandlerStatus::Ok) {
        out.error_message = (out.status == HandlerStatus::UniqueConflict)
            ? "address already exists for this (wallet_id, path, leaf_index)"
            : "store AddAddress failed";
    }

    Scrub(params.master_key.data(),  params.master_key.size());
    Scrub(params.bip32_priv.data(),  params.bip32_priv.size());
    Scrub(params.bip32_chain.data(), params.bip32_chain.size());
    return out;
}

// ---------------------------------------------------------------------------
// wallet.listp2mraddresses
// ---------------------------------------------------------------------------
ListP2MRAddressesResult ListP2MRAddresses(const wlt::V7P2MRStore&  store,
                                          ListP2MRAddressesParams  params) {
    ListP2MRAddressesResult out;
    try {
        out.entries = store.ListByWallet(params.wallet_id);
    } catch (const std::exception&) {
        out.status = HandlerStatus::StoreError;
        out.error_message = "P2MR inventory unavailable or invalid";
        return out;
    }
    out.status  = HandlerStatus::Ok;
    return out;
}

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

// ---------------------------------------------------------------------------
// wallet.importp2mrseed
// ---------------------------------------------------------------------------
ImportP2MRSeedResult ImportP2MRSeed(wlt::V7P2MRStore&      store,
                                    ImportP2MRSeedParams   params) {
    ImportP2MRSeedResult out;

    if (params.hrp.empty()) {
        out.status        = HandlerStatus::InvalidParams;
        out.error_message = "hrp is empty";
        Scrub(params.master_key.data(), params.master_key.size());
        Scrub(params.pq_seed.data(),    params.pq_seed.size());
        return out;
    }

    wlt::SecureKeypair kp(mldsa::KeygenFromSeed(params.pq_seed));
    out.pubkey       = kp.pubkey();
    out.merkle_root  = LeafHash(pq::SCHEME_ID_ML_DSA_65, kp.pubkey());
    out.address      = wlt::EncodeP2MRAddress(params.hrp, out.merkle_root);
    if (out.address.empty()) {
        out.status        = HandlerStatus::InvalidParams;
        out.error_message = "invalid hrp; address encoding failed";
        Scrub(params.master_key.data(), params.master_key.size());
        Scrub(params.pq_seed.data(),    params.pq_seed.size());
        return out;
    }

    // Encrypt.
    wlt::AeadSeed plain{};
    std::memcpy(plain.data(), params.pq_seed.data(), plain.size());
    wlt::AeadSealOutput sealed{};
    try {
        sealed = wlt::SealSeed(plain, params.master_key);
    } catch (const std::exception&) {
        out.status        = HandlerStatus::InternalError;
        out.error_message = "SealSeed failed";
        Scrub(plain.data(),             plain.size());
        Scrub(params.master_key.data(), params.master_key.size());
        Scrub(params.pq_seed.data(),    params.pq_seed.size());
        return out;
    }
    Scrub(plain.data(), plain.size());

    auto rc = store.AddAddress(
        params.wallet_id, out.address, out.merkle_root, kp.pubkey(),
        sealed.ciphertext, sealed.nonce, sealed.tag,
        params.derivation_path, params.leaf_index, params.label,
        params.now_unix);
    out.status = MapAddResult(rc);
    if (out.status != HandlerStatus::Ok) {
        out.error_message = (out.status == HandlerStatus::UniqueConflict)
            ? "address already exists for this (wallet_id, path, leaf_index)"
            : "store AddAddress failed";
    }

    Scrub(params.master_key.data(), params.master_key.size());
    Scrub(params.pq_seed.data(),    params.pq_seed.size());
    return out;
}

// ---------------------------------------------------------------------------
// wallet.exportp2mrseed
// ---------------------------------------------------------------------------
ExportP2MRSeedResult ExportP2MRSeed(const wlt::V7P2MRStore& store, ExportP2MRSeedParams params) {
    ExportP2MRSeedResult out;
    ScopedMasterKey master_guard{params.master_key};
    wlt::SecureSeed seed;
    std::optional<wlt::SecureKeypair> key;
    try {
        out.status = OpenBoundKey(store, params.wallet_id, params.address,
                                  params.master_key, seed, key);
        if (out.status != HandlerStatus::Ok) {
            out.error_message = "P2MR export key or stored address binding unavailable";
            return out;
        }
        // Existing explicit export contract: caller owns and must scrub this copy.
        std::memcpy(out.pq_seed.data(), seed.bytes().data(), out.pq_seed.size());
    } catch (const std::exception&) {
        out.status = HandlerStatus::InternalError;
        out.error_message = "P2MR export failed";
    }
    return out;
}

} // namespace dinero::rpc::v7
