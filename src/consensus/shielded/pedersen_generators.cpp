#include "consensus/shielded/pedersen_generators.h"

#include "crypto/evp_secp256k1.h"

#include <openssl/sha.h>
#include <secp256k1.h>
#include <secp256k1_generator.h>

#include <cstring>
#include <mutex>
#include <string>

namespace dinero::consensus::shielded {

namespace {

// Lazy-initialized state — derivation happens on first call to
// PedersenGeneratorV(), guarded by once-flag for thread safety.
struct DerivedState {
    Hash v_x{};                       // x-only big-endian
    secp256k1_generator v_internal{}; // 64-byte libsecp generator
    bool ok = false;
};

DerivedState& State() {
    static DerivedState s;
    static std::once_flag once;
    std::call_once(once, [&]() {
        secp256k1_context* ctx = ::dinero::crypto::GetSecp256k1ContextSignVerify();
        if (!ctx) return;

        // seed = SHA-256(DST_string), no salt, no msg.
        const std::string dst = kPedersenVDST;
        unsigned char seed[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(dst.data()), dst.size(), seed);

        if (!secp256k1_generator_generate(ctx, &s.v_internal, seed)) {
            return;  // s.ok remains false; PedersenGeneratorsReady() exposes this
        }

        // Serialize to 33-byte compressed form, take the x-coordinate.
        unsigned char compressed[33];
        if (!secp256k1_generator_serialize(ctx, compressed, &s.v_internal)) {
            return;
        }
        std::memcpy(s.v_x.data(), compressed + 1, 32);
        s.ok = true;
    });
    return s;
}

}  // namespace

const Hash& PedersenGeneratorV() {
    return State().v_x;
}

bool PedersenGeneratorsReady() {
    return State().ok;
}

// Internal accessor used by pedersen_commit.cpp — returns the cached
// libsecp generator form (64-byte internal type) without re-running
// the SHA-256 + generator_generate work.
const secp256k1_generator* PedersenGeneratorVInternal() {
    auto& s = State();
    return s.ok ? &s.v_internal : nullptr;
}

}  // namespace dinero::consensus::shielded
