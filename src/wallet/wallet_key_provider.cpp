/*
 * WalletKeyProvider — see include/wallet/wallet_key_provider.h for contract.
 *
 * Legacy path: unchanged MapKeyProvider semantics. Hex strings are decoded
 *              to bytes at construction time; lookups are O(log n) map hits.
 *
 * PQ path: builds a SignP2MRParams, forwards to the existing SignP2MR
 *          handler (which owns seed decryption, KeygenFromSeed, scrubbing).
 *          The returned signature + pubkey are packed into a canonical
 *          P2MRWitness and serialized. Depth-0 trees only today — matches
 *          what wallet.getnewp2mraddress produces.
 */

#include "wallet/wallet_key_provider.h"

#include "consensus/pq/p2mr_consensus.h"   // SerializeP2MRWitness, DeserializeP2MRWitness, P2MRWitness, IsP2MRScript
#include "consensus/pq/scheme_registry.h"  // GetSchemeParams, SCHEME_ID_ML_DSA_65
#include "rpc/v7_pq_handlers.h"            // SignP2MRParams / SignP2MR
#include "wallet/v7_p2mr_store.h"

#include <openssl/crypto.h>

#include <cassert>
#include <cstring>

namespace dinero::wallet {

namespace {

std::vector<uint8_t> HexToBytes(const std::string& hex) {
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        auto hi = hex[i];
        auto lo = hex[i + 1];
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        const int h = nib(hi);
        const int l = nib(lo);
        if (h < 0 || l < 0) { out.clear(); return out; }
        out.push_back(static_cast<uint8_t>((h << 4) | l));
    }
    return out;
}

} // namespace

WalletKeyProvider::WalletKeyProvider(Config cfg)
    : p2mr_store_(cfg.p2mr_store),
      wallet_id_(cfg.wallet_id),
      master_key_(cfg.master_key) {
    for (const auto& [path, hex] : cfg.legacy_keys_by_path) {
        auto bytes = HexToBytes(hex);
        if (!bytes.empty()) {
            legacy_keys_.emplace(path, std::move(bytes));
        }
    }
    OPENSSL_cleanse(cfg.master_key.data(), cfg.master_key.size());
}

WalletKeyProvider::~WalletKeyProvider() {
    OPENSSL_cleanse(master_key_.data(), master_key_.size());
}

std::vector<uint8_t> WalletKeyProvider::GetPrivateKey(const std::string& path) const {
    auto it = legacy_keys_.find(path);
    if (it == legacy_keys_.end()) return {};
    return it->second;
}

bool WalletKeyProvider::HasKey(const std::string& path) const {
    return legacy_keys_.find(path) != legacy_keys_.end();
}

std::vector<uint8_t> WalletKeyProvider::SignP2MR(
    const std::vector<uint8_t>&    script_pubkey,
    const std::array<uint8_t, 32>& sighash) const {

    if (!p2mr_store_) return {};
    if (!dinero::consensus::pq::IsP2MRScript(script_pubkey)) return {};

    // Bytes 2..34 of a P2MR scriptPubKey are the Merkle-root commitment.
    std::array<uint8_t, 32> merkle_root{};
    std::memcpy(merkle_root.data(), script_pubkey.data() + 2, 32);

    auto row = p2mr_store_->GetByMerkleRoot(wallet_id_, merkle_root);
    if (!row) return {};

    // Reuse the existing SignP2MR handler. It owns the seed decryption,
    // KeygenFromSeed, signing, and scrubbing disciplines. We just feed it
    // the address (looked up above), sighash, and a copy of the master key.
    dinero::rpc::v7::SignP2MRParams params{};
    params.wallet_id  = wallet_id_;
    params.address    = row->address;
    params.sighash    = sighash;
    std::memcpy(params.master_key.data(), master_key_.data(), master_key_.size());

    auto r = dinero::rpc::v7::SignP2MR(*p2mr_store_, params);
    // SignP2MR scrubs params.master_key on every path; nothing more to do.

    if (r.status != dinero::rpc::v7::HandlerStatus::Ok) return {};

    // ── Scheme-binding invariant ──────────────────────────────────────
    // The scheme_id returned by SignP2MR MUST be a known, Accept-state
    // scheme whose fixed pubkey and signature lengths match exactly. A
    // mismatch here means either the registry drifted, the handler was
    // compiled against a different PQClean, or the scheme was
    // inadvertently downgraded to DarkReserved. Catching it here avoids
    // producing a witness blob that consensus will reject.
    namespace pq = dinero::consensus::pq;
    const auto& sp = pq::GetSchemeParams(r.scheme_id);
    if (sp.state != pq::SchemeState::Accept) return {};
    if (r.pubkey.size()    != sp.pubkey_bytes_max)    return {};
    if (r.signature.size() != sp.signature_bytes_max) return {};

    // Depth-0 Merkle tree (v7 genesis default — what getnewp2mraddress
    // currently produces). Future multi-leaf support will populate
    // sibling_hashes + leaf_index from the store row.
    pq::P2MRWitness w{};
    w.scheme_id        = r.scheme_id;
    w.pubkey_bytes.assign(r.pubkey.begin(),    r.pubkey.end());
    w.signature_bytes.assign(r.signature.begin(), r.signature.end());
    w.merkle_depth     = 0;
    w.leaf_index       = 0;
    // sibling_hashes left empty (depth == 0)

    std::vector<uint8_t> blob = pq::SerializeP2MRWitness(w);
    if (blob.empty()) return {};

    // ── Round-trip invariant ──────────────────────────────────────────
    // Deserialize the blob we just produced and verify it decodes back
    // to Ok. This is the signer-side equivalent of the consensus
    // NonCanonical check: if Serialize→Deserialize doesn't round-trip,
    // the blob will be rejected at mempool / block validation time.
    // Catching it here turns a consensus-rejection into a wallet-side
    // error with a clear root cause.
    pq::P2MRWitness rt{};
    auto decode_err = pq::DeserializeP2MRWitness(blob, &rt);
    if (decode_err != pq::P2MRWitnessDecodeError::Ok) {
        assert(false && "SIGNER BUG: produced a non-decodable P2MR witness blob");
        return {};
    }

    // Verify the round-tripped fields match the originals. The decode
    // already checks structural validity; these assertions catch
    // field-level drift (e.g. a varint encoding change that silently
    // truncates a length).
    assert(rt.scheme_id == w.scheme_id);
    assert(rt.pubkey_bytes.size()    == w.pubkey_bytes.size());
    assert(rt.signature_bytes.size() == w.signature_bytes.size());
    assert(rt.merkle_depth           == w.merkle_depth);
    assert(rt.leaf_index             == w.leaf_index);

    return blob;
}

} // namespace dinero::wallet
