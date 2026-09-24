#pragma once

#include "orchard_backend_ffi.h"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace dinero::orchard {
using Hash = std::array<std::uint8_t, 32>;

// Draft protocol-v1 limits. Increasing the action limit changes both the
// accepted inner encoding and the fixed v1 ABI; it requires explicit review.
inline constexpr std::size_t kMaxActionsV1 = 8;
inline constexpr std::uint32_t kTransactionVersion = 7;
inline constexpr std::uint8_t kBundleWireProfile = 1;
// One wire-profile identity for the outer frame, inner codec and signed D.
// A future outer format cannot select an independent, unsigned profile.
inline constexpr std::uint8_t kOuterEnvelopeProfile = kBundleWireProfile;
inline constexpr std::uint8_t kOrchardPoolProfile = 1;
inline constexpr std::uint8_t kCircuitProfile = 1;
inline constexpr std::uint32_t kEffectCommitmentVersion = 5;
inline constexpr std::uint64_t kMaxMoneyUna = 26'542'800'000'000'000ULL;

struct SigningDomain {
    // Default is invalid, never an implicit mainnet domain.
    std::uint8_t network_code = 0xff;
    Hash genesis_wire{};
    std::uint32_t branch_id = 0;
};
struct ResolvedInput {
    Hash txid_wire{};
    std::uint32_t output_index = 0;
    std::uint32_t sequence = 0;
    std::uint64_t amount_una = 0;
    std::vector<std::uint8_t> script_pub_key;
};
struct TransparentOutput {
    std::uint64_t amount_una = 0;
    std::vector<std::uint8_t> script_pub_key;
};

// Owned draft signing context. Resolved inputs must come from the host's
// authenticated coin snapshot, in transaction order. Constructing this type
// checks shape and arithmetic, not coin provenance, maturity or scripts.
class SigningContext {
public:
    static SigningContext Create(SigningDomain domain, std::uint32_t lock_time,
                                 const std::vector<ResolvedInput>& inputs,
                                 const std::vector<TransparentOutput>& outputs,
                                 std::uint64_t explicit_fee_una);
    std::int64_t RequiredValueBalance() const noexcept { return required_balance_; }
private:
    friend class ParsedBundle;
    SigningContext(SigningDomain domain, std::uint32_t lock_time,
                   std::vector<ResolvedInput> inputs,
                   std::vector<TransparentOutput> outputs,
                   std::uint64_t fee, std::int64_t required_balance);
    Hash Digest(const DineroOrchardFacts& facts) const;
    const SigningDomain domain_;
    const std::uint32_t lock_time_;
    const std::vector<ResolvedInput> inputs_;
    const std::vector<TransparentOutput> outputs_;
    const std::uint64_t fee_;
    const std::int64_t required_balance_;
};

class BackendError : public std::runtime_error {
public:
    explicit BackendError(std::int32_t status);
    std::int32_t Status() const noexcept { return status_; }
private:
    std::int32_t status_;
};

class ParsedBundle;
// Only ParsedBundle::VerifyAuthorization constructs this result. It records
// the exact digest checked and shares ownership of the immutable parsed data.
// Chainstate and transparent-script validation are separate requirements.
class VerifiedAuthorization {
public:
    const DineroOrchardFacts& Facts() const noexcept { return facts_; }
    const Hash& SigningDigest() const noexcept { return digest_; }
private:
    friend class ParsedBundle;
    VerifiedAuthorization(std::shared_ptr<const DineroOrchardHandle> handle,
                          DineroOrchardFacts facts, Hash digest);
    const std::shared_ptr<const DineroOrchardHandle> handle_;
    const DineroOrchardFacts facts_;
    const Hash digest_;
};

class ParsedBundle {
public:
    [[nodiscard]] static ParsedBundle Decode(std::span<const std::uint8_t> bytes);
    const DineroOrchardFacts& UnverifiedFacts() const noexcept { return facts_; }
    // Derives D from the owned context and THIS bundle's effect. No public
    // C++ verification overload accepts a caller-supplied digest or balance.
    Hash SigningDigest(const SigningContext& context) const;
    [[nodiscard]] VerifiedAuthorization VerifyAuthorization(const SigningContext& context) const;
private:
    ParsedBundle(std::shared_ptr<const DineroOrchardHandle> handle,
                 DineroOrchardFacts facts);
    const std::shared_ptr<const DineroOrchardHandle> handle_;
    const DineroOrchardFacts facts_;
};
} // namespace dinero::orchard
