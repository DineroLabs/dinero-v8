#pragma once

#include "consensus/orchard_coin_snapshot.h"
#include "consensus/orchard_pool_balance.h"
#include <functional>
#include <optional>

namespace dinero::consensus {

// Draft new-format rules only; never used for historical transaction signatures.
inline constexpr uint8_t ORCHARD_TRANSPARENT_AUTH_PROFILE = 1;
static_assert(ORCHARD_TRANSPARENT_AUTH_PROFILE == orchard::kOuterEnvelopeProfile,
              "Transparent authorization and envelope profiles require joint review");
enum class OrchardTransparentErrorCode {
    ContextMismatch, MissingMedianTime, ImmatureCoinbase, NonFinal,
    UnsupportedProgram, InvalidWitness, InvalidSignature
};
class OrchardTransparentError : public std::runtime_error {
public:
    explicit OrchardTransparentError(OrchardTransparentErrorCode code)
        : std::runtime_error("Orchard transparent authorization rejected"), code_(code) {}
    OrchardTransparentErrorCode Code() const noexcept { return code_; }
private:
    OrchardTransparentErrorCode code_;
};
using OrchardBranchMtpLookup = std::function<std::optional<uint64_t>(uint32_t)>;

// Wallet-facing digest construction uses the same immutable resolved context.
// It takes neither a raw Orchard effect nor a caller-provided signing hash.
orchard::Hash OrchardTransparentSigningDigest(const OrchardCoinSnapshot& snapshot,
    orchard::SigningDomain domain, size_t input_index);

class VerifiedOrchardTransparentInputs;
[[nodiscard]] VerifiedOrchardTransparentInputs VerifyOrchardTransparentInputs(
    const OrchardCoinSnapshot& snapshot, orchard::SigningDomain domain,
    uint32_t candidate_height, const OrchardBranchMtpLookup& branch_mtp);

// Only the transparent signatures, maturity and locks have been checked.
// Orchard proof/anchor/nullifier/pool validation and atomic application remain
// mandatory. This owns its exact snapshot and is never a reusable unspentness
// certificate: the host's chainstate-lock contract still applies.
class VerifiedOrchardTransparentInputs {
public:
    const OrchardCoinSnapshot& Snapshot() const noexcept { return snapshot_; }
    const orchard::Hash& OrchardIntent() const noexcept { return intent_; }
    uint32_t CandidateHeight() const noexcept { return candidate_height_; }
private:
    friend VerifiedOrchardTransparentInputs VerifyOrchardTransparentInputs(
        const OrchardCoinSnapshot&, orchard::SigningDomain, uint32_t,
        const OrchardBranchMtpLookup&);
    VerifiedOrchardTransparentInputs(OrchardCoinSnapshot snapshot, orchard::Hash intent, uint32_t height)
        : snapshot_(std::move(snapshot)), intent_(intent), candidate_height_(height) {}
    const OrchardCoinSnapshot snapshot_;
    const orchard::Hash intent_;
    const uint32_t candidate_height_;
};

// Derive the turnstile input from the exact owned transparent authorization
// context. Orchard proof verification is deliberately not consulted here.
// Current unspentness still requires the caller's held chainstate lock/view.
[[nodiscard]] OrchardValueFlow GetOrchardValueFlow(const VerifiedOrchardTransparentInputs& inputs);
} // namespace dinero::consensus
