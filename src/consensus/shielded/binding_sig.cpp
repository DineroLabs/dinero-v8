#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/crypto/sighash_bip143.h"

#include "crypto/evp_secp256k1.h"
#include "crypto/sha256.h"

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_generator.h>
#include <secp256k1_rangeproof.h>
#include <secp256k1_schnorrsig.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace dinero::consensus::shielded {

const secp256k1_generator* PedersenGeneratorVInternal();

namespace {

struct Sha256Builder {
    dinero::crypto::CSHA256 ctx;
    void Add(const void* p, size_t n) {
        ctx.Write(static_cast<const uint8_t*>(p), n);
    }
    void AddU64(uint64_t v) {
        unsigned char buf[8];
        for (int i = 0; i < 8; ++i) buf[i] = static_cast<unsigned char>((v >> (8 * i)) & 0xFF);
        ctx.Write(buf, 8);
    }
    Hash Finalize() {
        Hash out{};
        ctx.Finalize(out.data());
        return out;
    }
};

}  // namespace

Hash ComputeShieldedTxSighash(const ::dinero::Transaction& tx) {
    using ::dinero::consensus::SighashBIP143;
    constexpr const char kDst[] = "DIN/v7/shielded/tx-sighash/v1";
    Sha256Builder b;
    b.Add(kDst, sizeof(kDst) - 1);

    // version
    unsigned char v[4];
    for (int i = 0; i < 4; ++i) v[i] = static_cast<unsigned char>((tx.version >> (8 * i)) & 0xFF);
    b.Add(v, 4);

    // BIP143 components covering every transparent malleation.
    // SighashBIP143's helpers are private; we reproduce the equivalent
    // commitments here by hashing prevouts, sequences, and outputs
    // through ComputeSighash on a synthetic SIGHASH_ALL — except we
    // can't call ComputeSighash without per-input scriptCode/value.
    // So we hash the raw fields directly; same domain-separation goal.
    auto hash_u32_le = [&](uint32_t x) {
        unsigned char buf[4];
        for (int i = 0; i < 4; ++i) buf[i] = static_cast<unsigned char>((x >> (8 * i)) & 0xFF);
        b.Add(buf, 4);
    };
    auto hash_u64_le = [&](uint64_t x) {
        unsigned char buf[8];
        for (int i = 0; i < 8; ++i) buf[i] = static_cast<unsigned char>((x >> (8 * i)) & 0xFF);
        b.Add(buf, 8);
    };

    hash_u32_le(static_cast<uint32_t>(tx.vin.size()));
    for (const auto& in : tx.vin) {
        const auto& txid_u = in.prevout.txid.AsUint256();
        b.Add(txid_u.begin(), 32);  // uint256 = 32 bytes
        hash_u32_le(in.prevout.vout);
        hash_u32_le(in.sequence);
    }
    hash_u32_le(static_cast<uint32_t>(tx.vout.size()));
    for (const auto& out : tx.vout) {
        hash_u64_le(out.value.GetUna());
        hash_u32_le(static_cast<uint32_t>(out.scriptPubKey.size()));
        b.Add(out.scriptPubKey.data(), out.scriptPubKey.size());
    }
    hash_u32_le(tx.lockTime);
    return b.Finalize();
}

Hash ComputeBindingSighash(const ShieldedBundle& bundle,
                           const Hash& tx_sighash) {
    constexpr const char kDst[] = "DIN/v7/shielded/binding/v1";
    Sha256Builder b;
    b.Add(kDst, sizeof(kDst) - 1);
    b.AddU64(static_cast<uint64_t>(bundle.value_balance));
    b.Add(tx_sighash.data(), tx_sighash.size());

    std::vector<ValueCommitment> spend_cvs;
    spend_cvs.reserve(bundle.spends.size());
    for (const auto& s : bundle.spends) spend_cvs.push_back(s.cv);
    std::sort(spend_cvs.begin(), spend_cvs.end());

    std::vector<ValueCommitment> output_cvs;
    output_cvs.reserve(bundle.outputs.size());
    for (const auto& o : bundle.outputs) output_cvs.push_back(o.cv);
    std::sort(output_cvs.begin(), output_cvs.end());

    b.AddU64(spend_cvs.size());
    for (const auto& cv : spend_cvs) b.Add(cv.data(), cv.size());
    b.AddU64(output_cvs.size());
    for (const auto& cv : output_cvs) b.Add(cv.data(), cv.size());
    return b.Finalize();
}

BindingSigResult ComputeBvkCommitment(const Hash& bsk,
                                      ValueCommitment& out_bvk) {
    if (!PedersenGeneratorsReady()) return BindingSigResult::GeneratorNotReady;
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    const auto* gen_v = PedersenGeneratorVInternal();
    if (!ctx || !gen_v) return BindingSigResult::GeneratorNotReady;

    // bvk = bsk · G + 0 · V
    secp256k1_pedersen_commitment commit{};
    if (!secp256k1_pedersen_commit(ctx, &commit, bsk.data(), 0, gen_v)) {
        return BindingSigResult::CommitmentInvalid;
    }
    if (!secp256k1_pedersen_commitment_serialize(ctx, out_bvk.data(), &commit)) {
        return BindingSigResult::CommitmentInvalid;
    }
    return BindingSigResult::Ok;
}

BindingSigResult SignBinding(const Hash& bsk,
                             const Hash& binding_sighash,
                             BindingSignature& out_sig) {
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    if (!ctx) return BindingSigResult::GeneratorNotReady;

    secp256k1_keypair kp{};
    if (!secp256k1_keypair_create(ctx, &kp, bsk.data())) {
        return BindingSigResult::SignatureMalformed;
    }
    if (!secp256k1_schnorrsig_sign32(ctx, out_sig.data(),
                                     binding_sighash.data(),
                                     &kp, /*aux_rand32=*/nullptr)) {
        return BindingSigResult::SignatureMalformed;
    }
    return BindingSigResult::Ok;
}

BindingSigResult VerifyBinding(const ShieldedBundle& bundle,
                               const Hash& tx_sighash) {
    if (!PedersenGeneratorsReady()) return BindingSigResult::GeneratorNotReady;
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    const auto* gen_v = PedersenGeneratorVInternal();
    if (!ctx || !gen_v) return BindingSigResult::GeneratorNotReady;

    // Step 1: pedersen_verify_tally for the Dinero balance equation.
    //
    // Dinero convention: value_balance = sum(value_output) - sum(value_spend)
    //                                  = transparent_in - transparent_out - fee
    // Positive = shield (value flowing INTO the pool), negative = unshield.
    //
    // bvk derivation:
    //   sum(cv_spend) - sum(cv_output) = bsk·G + (sum(v_spend) - sum(v_output))·V
    //                                  = bsk·G - value_balance·V
    //   bvk = bsk·G = sum(cv_spend) - sum(cv_output) + value_balance·V
    //
    // Tally (sum(pos) - sum(neg) == 0):
    //   pos = {cv_spend...}                 (always)
    //   neg = {cv_output..., bvk_commit}    (always)
    //   For value_balance > 0 (shield):  add  vb·V  on the POS side.
    //   For value_balance < 0 (unshield): add |vb|·V on the NEG side.
    std::vector<secp256k1_pedersen_commitment> commit_storage;
    commit_storage.reserve(bundle.spends.size() + bundle.outputs.size() + 2);
    std::vector<const secp256k1_pedersen_commitment*> pos_ptrs;
    std::vector<const secp256k1_pedersen_commitment*> neg_ptrs;

    auto parse_into_storage = [&](const ValueCommitment& cv,
                                  std::vector<const secp256k1_pedersen_commitment*>& target,
                                  bool& ok) {
        secp256k1_pedersen_commitment c{};
        if (!secp256k1_pedersen_commitment_parse(ctx, &c, cv.data())) {
            ok = false;
            return;
        }
        commit_storage.push_back(c);
        target.push_back(&commit_storage.back());
    };

    bool parse_ok = true;
    for (const auto& s : bundle.spends) parse_into_storage(s.cv, pos_ptrs, parse_ok);
    for (const auto& o : bundle.outputs) parse_into_storage(o.cv, neg_ptrs, parse_ok);
    if (!parse_ok) return BindingSigResult::CommitmentInvalid;

    // C_vb = pedersen_commit(blind=0, value=|vb|), placed on the
    // appropriate side of the tally.
    if (bundle.value_balance != 0) {
        Hash zero_blind{};
        const uint64_t abs_v = (bundle.value_balance < 0)
            ? static_cast<uint64_t>(-(bundle.value_balance + 1)) + 1
            : static_cast<uint64_t>(bundle.value_balance);
        secp256k1_pedersen_commitment c_vb{};
        if (!secp256k1_pedersen_commit(ctx, &c_vb, zero_blind.data(),
                                       abs_v, gen_v)) {
            return BindingSigResult::CommitmentInvalid;
        }
        commit_storage.push_back(c_vb);
        if (bundle.value_balance > 0) {
            // Shield: vb·V joins the POS side so bvk_commit = bsk·G alone.
            pos_ptrs.push_back(&commit_storage.back());
        } else {
            // Unshield: |vb|·V joins the NEG side.
            neg_ptrs.push_back(&commit_storage.back());
        }
    }

    // bvk_commitment on the negative side.
    {
        secp256k1_pedersen_commitment c_bvk{};
        if (!secp256k1_pedersen_commitment_parse(ctx, &c_bvk,
                                                 bundle.bvk_commitment.data())) {
            return BindingSigResult::CommitmentInvalid;
        }
        commit_storage.push_back(c_bvk);
        neg_ptrs.push_back(&commit_storage.back());
    }

    if (!secp256k1_pedersen_verify_tally(ctx,
                                         pos_ptrs.data(), pos_ptrs.size(),
                                         neg_ptrs.data(), neg_ptrs.size())) {
        return BindingSigResult::SignatureInvalid;
    }

    // Step 2: BIP340 Schnorr verify. The xonly pubkey is the x-coord
    // of bvk_commitment (bytes [1..33]) interpreted as even-y.
    // BIP340's keypair_create auto-negated bsk to ensure the keypair
    // pubkey has even-y, so the signature is valid against
    // even_y(bvk_commitment.x) regardless of the actual y-parity.
    secp256k1_xonly_pubkey xonly{};
    if (!secp256k1_xonly_pubkey_parse(ctx, &xonly,
                                      bundle.bvk_commitment.data() + 1)) {
        return BindingSigResult::CommitmentInvalid;
    }
    const Hash sighash = ComputeBindingSighash(bundle, tx_sighash);
    if (!secp256k1_schnorrsig_verify(ctx, bundle.binding_sig.data(),
                                     sighash.data(), sighash.size(),
                                     &xonly)) {
        return BindingSigResult::SignatureInvalid;
    }
    return BindingSigResult::Ok;
}

}  // namespace dinero::consensus::shielded
