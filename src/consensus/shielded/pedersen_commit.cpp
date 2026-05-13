#include "consensus/shielded/pedersen_commit.h"
#include "consensus/shielded/pedersen_generators.h"

#include "crypto/evp_secp256k1.h"

#include <secp256k1.h>
#include <secp256k1_generator.h>
#include <secp256k1_rangeproof.h>

#include <cstring>

namespace dinero::consensus::shielded {

// Implemented in pedersen_generators.cpp; returns a non-owning pointer
// to the cached `secp256k1_generator` once the lazy derivation has run,
// or nullptr if derivation failed (in which case PedersenGeneratorsReady
// also returns false).
const secp256k1_generator* PedersenGeneratorVInternal();

PedersenResult PedersenCommit(const Hash& blind, uint64_t value,
                              ValueCommitment& out_cv) {
    if (!PedersenGeneratorsReady()) return PedersenResult::GeneratorNotReady;
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    if (!ctx) return PedersenResult::LibsecpFailure;
    const secp256k1_generator* gen = PedersenGeneratorVInternal();
    if (!gen) return PedersenResult::GeneratorNotReady;

    secp256k1_pedersen_commitment commit{};
    if (!secp256k1_pedersen_commit(ctx, &commit, blind.data(), value, gen)) {
        return PedersenResult::LibsecpFailure;
    }

    if (!secp256k1_pedersen_commitment_serialize(ctx, out_cv.data(), &commit)) {
        return PedersenResult::LibsecpFailure;
    }
    return PedersenResult::Ok;
}

PedersenResult PedersenCommitSerialize(const secp256k1_pedersen_commitment& commit,
                                       std::array<uint8_t, 33>& out33) {
    auto* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
    if (!ctx) return PedersenResult::LibsecpFailure;
    if (!secp256k1_pedersen_commitment_serialize(ctx, out33.data(), &commit)) {
        return PedersenResult::LibsecpFailure;
    }
    return PedersenResult::Ok;
}

}  // namespace dinero::consensus::shielded
