#include "orchard_backend.h"
#include <cstddef>
#include <string>
#include <utility>

namespace dinero::orchard {
static_assert(sizeof(DineroOrchardFacts) == 624);
static_assert(offsetof(DineroOrchardFacts, value_balance) == 96);
static_assert(offsetof(DineroOrchardFacts, nullifiers) == 112);

BackendError::BackendError(std::int32_t status)
    : std::runtime_error("Orchard backend status " + std::to_string(status)), status_(status) {}
namespace {
void Check(std::int32_t status) { if (status != 0) throw BackendError(status); }
}
ParsedBundle::ParsedBundle(std::shared_ptr<const DineroOrchardHandle> handle,
                           DineroOrchardFacts facts)
    : handle_(std::move(handle)), facts_(facts) {}
VerifiedAuthorization::VerifiedAuthorization(std::shared_ptr<const DineroOrchardHandle> handle,
                                             DineroOrchardFacts facts, Hash digest)
    : handle_(std::move(handle)), facts_(facts), digest_(digest) {}
ParsedBundle ParsedBundle::Decode(std::span<const std::uint8_t> bytes) {
    DineroOrchardHandle* raw = nullptr;
    Check(dinero_orchard_decode_v1(bytes.data(), bytes.size(), &raw));
    std::shared_ptr<const DineroOrchardHandle> handle(raw, [](const auto* p) {
        dinero_orchard_free_v1(const_cast<DineroOrchardHandle*>(p));
    });
    DineroOrchardFacts facts{};
    Check(dinero_orchard_facts_v1(handle.get(), &facts));
    return ParsedBundle(std::move(handle), facts);
}
VerifiedAuthorization ParsedBundle::VerifyAuthorization(const Hash& digest,
                                                        std::int64_t required_balance) const {
    Check(dinero_orchard_verify_v1(handle_.get(), digest.data(), required_balance));
    return VerifiedAuthorization(handle_, facts_, digest);
}
} // namespace dinero::orchard
