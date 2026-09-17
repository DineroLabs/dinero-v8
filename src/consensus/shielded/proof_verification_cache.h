#pragma once

#include "consensus/shielded/shielded_circuit.h"
#include "crypto/sha256.h"

#include <array>
#include <mutex>
#include <set>

namespace dinero::consensus::shielded::detail {

// Process-local successful cryptographic checks only. The caller MUST perform
// the real verification before RememberVerified. This contains neither proofs
// nor chain-state validity decisions: a hit says nothing about spent nullifiers,
// anchors, transaction signatures, fees, resource limits, or activation height.
// A fixed FIFO bounds memory independently of attacker-controlled proof sizes.
// No cache lock is held while verifying a proof or acquiring a chain-state lock.
template<size_t Capacity = 1024>
class VerifiedProofCache {
    static_assert(Capacity > 0);
public:
    bool Contains(const Hash& key) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return keys_.find(key) != keys_.end();
    }

    void RememberVerified(const Hash& key) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (keys_.find(key) != keys_.end()) return;
        if (keys_.size() == Capacity) keys_.erase(order_[next_]);
        keys_.insert(key);
        order_[next_] = key;
        next_ = (next_ + 1) % Capacity;
    }

private:
    mutable std::mutex mutex_;
    std::set<Hash> keys_;
    std::array<Hash, Capacity> order_{};
    size_t next_ = 0;
};

// Every field before proof_bytes has a fixed width. The proof is the final
// variable-width field, including its version and all serialized bytes. Hash
// the public inputs themselves (not txid), all verifier switches, and a distinct
// spend/output domain. Encode integers explicitly, never native structs/padding.
inline Hash SpendProofCacheKey(const std::vector<uint8_t>& proof_bytes,
                              const SpendPublicInputs& pub,
                              bool bind, bool cv, bool auth, bool covenant) {
    constexpr uint8_t domain[] = "dinero.shielded.verified.spend.v1";
    const uint8_t profile[]{uint8_t(bind), uint8_t(cv), uint8_t(auth), uint8_t(covenant)};
    const uint8_t height[]{uint8_t(pub.covenant_minimum_height),
        uint8_t(pub.covenant_minimum_height >> 8),
        uint8_t(pub.covenant_minimum_height >> 16),
        uint8_t(pub.covenant_minimum_height >> 24)};
    Hash key{};
    crypto::CSHA256().Write(domain, sizeof(domain)).Write(profile, sizeof(profile))
        .Write(pub.nullifier.data(), pub.nullifier.size())
        .Write(pub.anchor.data(), pub.anchor.size())
        .Write(pub.cv.data(), pub.cv.size())
        .Write(pub.covenant_outputs.data(), pub.covenant_outputs.size())
        .Write(height, sizeof(height)).Write(proof_bytes.data(), proof_bytes.size())
        .Finalize(key.data());
    return key;
}

inline Hash OutputProofCacheKey(const std::vector<uint8_t>& proof_bytes,
                               const OutputPublicInputs& pub, bool bind, bool cv) {
    constexpr uint8_t domain[] = "dinero.shielded.verified.output.v1";
    const uint8_t profile[]{uint8_t(bind), uint8_t(cv)};
    Hash key{};
    crypto::CSHA256().Write(domain, sizeof(domain)).Write(profile, sizeof(profile))
        .Write(pub.commitment.data(), pub.commitment.size())
        .Write(pub.cv.data(), pub.cv.size())
        .Write(proof_bytes.data(), proof_bytes.size()).Finalize(key.data());
    return key;
}

} // namespace dinero::consensus::shielded::detail
