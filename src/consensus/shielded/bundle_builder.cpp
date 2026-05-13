#include "consensus/shielded/bundle_builder.h"
#include "consensus/shielded/binding_sig.h"
#include "consensus/shielded/pedersen_commit.h"
#include "consensus/shielded/pedersen_generators.h"
#include "consensus/shielded/range_proof.h"

#include "crypto/evp_secp256k1.h"

#include <secp256k1.h>
#include <secp256k1_rangeproof.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace dinero::consensus::shielded {

BundleBuildResult BuildShieldedBundle(const std::vector<PlannedSpend>& spends,
                                      const std::vector<PlannedOutput>& outputs,
                                      const Hash& tx_sighash,
                                      ShieldedBundle& out_bundle) {
    if (!PedersenGeneratorsReady()) return BundleBuildResult::GeneratorNotReady;
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    if (!ctx) return BundleBuildResult::GeneratorNotReady;

    if (spends.size() > kMaxSpendsPerBundle) return BundleBuildResult::TooManySpends;
    if (outputs.size() > kMaxOutputsPerBundle) return BundleBuildResult::TooManyOutputs;

    out_bundle = ShieldedBundle{};

    // ── 1. Build cv + range proof for each spend, in input order ────
    out_bundle.spends.reserve(spends.size());
    for (const auto& s : spends) {
        ShieldedSpend ws{};
        ws.nullifier = s.nullifier;
        ws.anchor    = s.anchor;
        if (PedersenCommit(s.rcv, s.value_una, ws.cv) != PedersenResult::Ok) {
            return BundleBuildResult::PedersenFailure;
        }
        ws.zk_proof = s.spend_proof;
        out_bundle.spends.push_back(std::move(ws));
    }

    out_bundle.outputs.reserve(outputs.size());
    for (const auto& o : outputs) {
        ShieldedOutput wo{};
        wo.commitment      = o.commitment;
        if (PedersenCommit(o.rcv, o.value_una, wo.cv) != PedersenResult::Ok) {
            return BundleBuildResult::PedersenFailure;
        }
        wo.encrypted_note  = o.encrypted_note;
        wo.zk_proof        = o.output_proof;
        out_bundle.outputs.push_back(std::move(wo));
    }

    // ── 2. value_balance (Dinero convention) ────────────────────────
    int64_t sum_out = 0;
    int64_t sum_in  = 0;
    for (const auto& o : outputs) sum_out += static_cast<int64_t>(o.value_una);
    for (const auto& s : spends)  sum_in  += static_cast<int64_t>(s.value_una);
    out_bundle.value_balance = sum_out - sum_in;

    // ── 3. Build per-cv range proofs in canonical order ─────────────
    // Canonical order = nullifier-sorted spends, then commitment-sorted
    // outputs (matches what consensus iterates in VerifyBundleRangeProofs).
    struct CvProof {
        std::vector<uint8_t> proof;
        Hash sort_key;          // nullifier or commitment
        bool is_spend;
    };
    std::vector<CvProof> all;
    all.reserve(spends.size() + outputs.size());

    for (const auto& s : spends) {
        std::vector<uint8_t> proof;
        if (SignRangeProof(s.rcv, s.nonce, s.value_una, proof)
                != RangeProofResult::Ok) {
            return BundleBuildResult::RangeProofFailure;
        }
        all.push_back({std::move(proof), s.nullifier, true});
    }
    for (const auto& o : outputs) {
        std::vector<uint8_t> proof;
        if (SignRangeProof(o.rcv, o.nonce, o.value_una, proof)
                != RangeProofResult::Ok) {
            return BundleBuildResult::RangeProofFailure;
        }
        all.push_back({std::move(proof), o.commitment, false});
    }
    // Spends first (any spend < any output), then sort each group by key.
    std::stable_sort(all.begin(), all.end(),
                     [](const CvProof& a, const CvProof& b) {
                         if (a.is_spend != b.is_spend) return a.is_spend;
                         return a.sort_key < b.sort_key;
                     });
    std::vector<std::vector<uint8_t>> proofs_in_order;
    proofs_in_order.reserve(all.size());
    for (auto& cp : all) proofs_in_order.push_back(std::move(cp.proof));
    out_bundle.aggregated_range_proof =
        EncodeAggregatedRangeProof(proofs_in_order);

    // ── 4. bsk = sum(rcv_spend) - sum(rcv_output) ───────────────────
    // libsecp's pedersen_blind_sum: first npositive blinds added,
    // remaining subtracted. Spends contribute positive, outputs
    // contribute negative.
    Hash bsk{};
    {
        std::vector<const unsigned char*> blinds;
        blinds.reserve(spends.size() + outputs.size());
        for (const auto& s : spends)  blinds.push_back(s.rcv.data());
        for (const auto& o : outputs) blinds.push_back(o.rcv.data());
        if (blinds.empty()) {
            // Edge case: empty bundle. Can't sign anything; caller
            // should not invoke the builder for empty bundles
            // (validation short-circuits empty as Ok already).
            return BundleBuildResult::BlindSumFailure;
        }
        if (!secp256k1_pedersen_blind_sum(ctx, bsk.data(), blinds.data(),
                                          blinds.size(), spends.size())) {
            return BundleBuildResult::BlindSumFailure;
        }
    }

    // ── 5. bvk_commitment = bsk·G in pedersen format ────────────────
    if (ComputeBvkCommitment(bsk, out_bundle.bvk_commitment)
            != BindingSigResult::Ok) {
        return BundleBuildResult::BvkFailure;
    }

    // ── 6. Sign binding sig over canonical sighash ──────────────────
    const Hash sighash = ComputeBindingSighash(out_bundle, tx_sighash);
    if (SignBinding(bsk, sighash, out_bundle.binding_sig)
            != BindingSigResult::Ok) {
        return BundleBuildResult::BindingSigFailure;
    }
    return BundleBuildResult::Ok;
}

}  // namespace dinero::consensus::shielded
