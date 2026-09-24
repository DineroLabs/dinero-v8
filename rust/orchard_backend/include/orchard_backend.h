#pragma once

#include "orchard_backend_ffi.h"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

namespace dinero::orchard {
using Hash = std::array<std::uint8_t, 32>;

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
    static ParsedBundle Decode(std::span<const std::uint8_t> bytes);
    const DineroOrchardFacts& UnverifiedFacts() const noexcept { return facts_; }
    // digest and balance MUST be derived by the host from the same immutable
    // transaction and authenticated prevouts. Neither is a wire claim.
    VerifiedAuthorization VerifyAuthorization(const Hash& digest,
                                               std::int64_t required_balance) const;
private:
    ParsedBundle(std::shared_ptr<const DineroOrchardHandle> handle,
                 DineroOrchardFacts facts);
    const std::shared_ptr<const DineroOrchardHandle> handle_;
    const DineroOrchardFacts facts_;
};
} // namespace dinero::orchard
