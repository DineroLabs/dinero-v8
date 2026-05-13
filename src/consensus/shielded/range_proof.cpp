#include "consensus/shielded/range_proof.h"
#include "consensus/shielded/pedersen_commit.h"
#include "consensus/shielded/pedersen_generators.h"

#include "crypto/evp_secp256k1.h"

#include <secp256k1.h>
#include <secp256k1_generator.h>
#include <secp256k1_rangeproof.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace dinero::consensus::shielded {

// Internal accessor implemented in pedersen_generators.cpp.
const secp256k1_generator* PedersenGeneratorVInternal();

namespace {

// CompactSize helpers — same encoding as shielded_serialization.cpp.
// Kept local to avoid coupling.
void WriteCompactSize(std::vector<uint8_t>& out, uint64_t v) {
    if (v < 253) {
        out.push_back(static_cast<uint8_t>(v));
    } else if (v <= 0xFFFF) {
        out.push_back(0xFD);
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    } else if (v <= 0xFFFFFFFF) {
        out.push_back(0xFE);
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
        }
    } else {
        out.push_back(0xFF);
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
        }
    }
}

bool ReadCompactSize(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
    if (p >= end) return false;
    uint8_t first = *p++;
    if (first < 253) {
        out = first;
    } else if (first == 0xFD) {
        if (p + 2 > end) return false;
        out = static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8);
        if (out < 253) return false;
        p += 2;
    } else if (first == 0xFE) {
        if (p + 4 > end) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            out |= static_cast<uint64_t>(p[i]) << (8 * i);
        }
        if (out <= 0xFFFF) return false;
        p += 4;
    } else {
        if (p + 8 > end) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) {
            out |= static_cast<uint64_t>(p[i]) << (8 * i);
        }
        if (out <= 0xFFFFFFFF) return false;
        p += 8;
    }
    return true;
}

// Parse a 33-byte cv (libsecp pedersen serialization with prefix
// 0x08/0x09 conveying y-parity) into a pedersen_commitment.
bool ParseCv(secp256k1_context* ctx, const ValueCommitment& cv,
             secp256k1_pedersen_commitment& out) {
    return secp256k1_pedersen_commitment_parse(ctx, &out, cv.data()) != 0;
}

// Decode the `aggregated_range_proof` blob into a list of per-cv
// proof byte-vectors. Returns false on parse error.
bool DecodeAggregated(const std::vector<uint8_t>& blob,
                      std::vector<std::vector<uint8_t>>& out) {
    const uint8_t* p = blob.data();
    const uint8_t* end = p + blob.size();
    uint64_t n = 0;
    if (!ReadCompactSize(p, end, n)) return false;
    out.clear();
    out.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t plen = 0;
        if (!ReadCompactSize(p, end, plen)) return false;
        if (p + plen > end) return false;
        out.emplace_back(p, p + plen);
        p += plen;
    }
    return p == end;
}

}  // namespace

std::vector<uint8_t> EncodeAggregatedRangeProof(
    const std::vector<std::vector<uint8_t>>& per_cv_proofs) {
    std::vector<uint8_t> out;
    WriteCompactSize(out, per_cv_proofs.size());
    for (const auto& proof : per_cv_proofs) {
        WriteCompactSize(out, proof.size());
        out.insert(out.end(), proof.begin(), proof.end());
    }
    return out;
}

RangeProofResult SignRangeProof(const Hash& blind,
                                const Hash& nonce,
                                uint64_t value,
                                std::vector<uint8_t>& out_proof) {
    if (!PedersenGeneratorsReady()) return RangeProofResult::GeneratorNotReady;
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    if (!ctx) return RangeProofResult::GeneratorNotReady;
    const auto* gen = PedersenGeneratorVInternal();
    if (!gen) return RangeProofResult::GeneratorNotReady;

    secp256k1_pedersen_commitment commit{};
    if (!secp256k1_pedersen_commit(ctx, &commit, blind.data(), value, gen)) {
        return RangeProofResult::CommitmentInvalid;
    }

    out_proof.assign(secp256k1_rangeproof_max_size(ctx, UINT64_MAX, 0), 0);
    size_t plen = out_proof.size();
    if (!secp256k1_rangeproof_sign(ctx, out_proof.data(), &plen,
                                   /*min_value=*/0,
                                   &commit,
                                   blind.data(),
                                   nonce.data(),
                                   /*exp=*/0,
                                   /*min_bits=*/0,
                                   value,
                                   /*message=*/nullptr, /*msg_len=*/0,
                                   /*extra_commit=*/nullptr, /*extra_commit_len=*/0,
                                   gen)) {
        return RangeProofResult::VerifyFailed;
    }
    out_proof.resize(plen);
    return RangeProofResult::Ok;
}

RangeProofResult VerifyBundleRangeProofs(const ShieldedBundle& bundle) {
    if (!PedersenGeneratorsReady()) return RangeProofResult::GeneratorNotReady;
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    if (!ctx) return RangeProofResult::GeneratorNotReady;
    const auto* gen = PedersenGeneratorVInternal();
    if (!gen) return RangeProofResult::GeneratorNotReady;

    // Decode the aggregated blob into per-cv proofs.
    std::vector<std::vector<uint8_t>> proofs;
    if (!DecodeAggregated(bundle.aggregated_range_proof, proofs)) {
        return RangeProofResult::ParseError;
    }
    const size_t expected = bundle.spends.size() + bundle.outputs.size();
    if (proofs.size() != expected) {
        return RangeProofResult::CountMismatch;
    }

    // Iterate canonical-ordered spend cvs first, then output cvs. The
    // serializer already canonicalizes order, but the in-memory bundle
    // may not be sorted — sort copies of cv vectors for verification.
    std::vector<ValueCommitment> cv_order;
    cv_order.reserve(expected);

    auto spends_sorted = bundle.spends;
    std::sort(spends_sorted.begin(), spends_sorted.end(),
              [](const auto& a, const auto& b) { return a.nullifier < b.nullifier; });
    for (const auto& s : spends_sorted) cv_order.push_back(s.cv);

    auto outputs_sorted = bundle.outputs;
    std::sort(outputs_sorted.begin(), outputs_sorted.end(),
              [](const auto& a, const auto& b) { return a.commitment < b.commitment; });
    for (const auto& o : outputs_sorted) cv_order.push_back(o.cv);

    for (size_t i = 0; i < expected; ++i) {
        secp256k1_pedersen_commitment commit{};
        if (!ParseCv(ctx, cv_order[i], commit)) {
            return RangeProofResult::CommitmentInvalid;
        }
        uint64_t min_value = 0;
        uint64_t max_value = 0;
        if (!secp256k1_rangeproof_verify(ctx,
                                         &min_value, &max_value,
                                         &commit,
                                         proofs[i].data(), proofs[i].size(),
                                         /*extra_commit=*/nullptr, /*extra_commit_len=*/0,
                                         gen)) {
            return RangeProofResult::VerifyFailed;
        }
        // The rangeproof guarantees v ∈ [min_value, max_value] AND
        // v ∈ [0, 2^64). Anything outside that is a libsecp invariant
        // violation; we assert via shape rather than sentinel values.
    }
    return RangeProofResult::Ok;
}

}  // namespace dinero::consensus::shielded
