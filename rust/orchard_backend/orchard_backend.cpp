#include "orchard_backend.h"
#include <cstddef>
#include <exception>
#include <string>
#include <utility>
#include <algorithm>

namespace dinero::orchard {
static_assert(kMaxActionsV1 == DINERO_ORCHARD_V1_MAX_ACTIONS);
static_assert(sizeof(DineroOrchardProtocol) == 20);
static_assert(sizeof(DineroOrchardFacts) == 624);
static_assert(offsetof(DineroOrchardFacts, value_balance) == 96);
static_assert(offsetof(DineroOrchardFacts, nullifiers) == 112);

BackendError::BackendError(std::int32_t status)
    : std::runtime_error("Orchard backend status " + std::to_string(status)), status_(status) {}
namespace {
void Check(std::int32_t status) { if (status != 0) throw BackendError(status); }
}
static_assert(sizeof(DineroOrchardFrontier) == 1120);
static_assert(offsetof(DineroOrchardFrontier, leaf_count) == 32);
static_assert(offsetof(DineroOrchardFrontier, encoded_length) == 40);
static_assert(offsetof(DineroOrchardFrontier, encoded) == 44);
OrchardFrontier::OrchardFrontier(const DineroOrchardFrontier& result)
    : root_([&] { Hash h; std::copy_n(result.root, 32, h.begin()); return h; }()),
      size_(result.leaf_count), bytes_([&] {
        if (result.encoded_length > DINERO_ORCHARD_V1_MAX_FRONTIER_BYTES ||
            result.encoded_length < 16 || result.leaf_count > (uint64_t{1} << 32))
            throw BackendError(DINERO_ORCHARD_FORMAT);
        return std::vector<uint8_t>(result.encoded, result.encoded + result.encoded_length);
      }()) {}
OrchardFrontier OrchardFrontier::Empty() {
    DineroOrchardFrontier result{};
    Check(dinero_orchard_frontier_empty_v1(&result));
    return OrchardFrontier(result);
}
OrchardFrontier OrchardFrontier::Decode(std::span<const uint8_t> bytes) {
    DineroOrchardFrontier result{};
    Check(dinero_orchard_frontier_append_v1(bytes.data(), bytes.size(), nullptr, 0, &result));
    return OrchardFrontier(result);
}
OrchardFrontier OrchardFrontier::Append(std::span<const Hash> commitments) const {
    if (commitments.size() > DINERO_ORCHARD_V1_MAX_ACTIONS)
        throw BackendError(DINERO_ORCHARD_LIMIT);
    // Flatten explicitly rather than depending on array-of-std::array layout.
    std::vector<uint8_t> flat;
    for (const auto& cmx : commitments) flat.insert(flat.end(), cmx.begin(), cmx.end());
    DineroOrchardFrontier result{};
    Check(dinero_orchard_frontier_append_v1(bytes_.data(), bytes_.size(), flat.data(), commitments.size(), &result));
    return OrchardFrontier(result);
}
ParsedBundle::ParsedBundle(std::shared_ptr<const DineroOrchardHandle> handle,
                           DineroOrchardFacts facts)
    : handle_(std::move(handle)), facts_(facts) {}
VerifiedAuthorization::VerifiedAuthorization(std::shared_ptr<const DineroOrchardHandle> handle,
                                             DineroOrchardFacts facts, Hash digest)
    : handle_(std::move(handle)), facts_(facts), digest_(digest) {}
ParsedBundle ParsedBundle::Decode(std::span<const std::uint8_t> bytes) {
    DineroOrchardProtocol profile{};
    Check(dinero_orchard_protocol_v1(&profile));
    if (profile.transaction_version != kTransactionVersion ||
        profile.bundle_wire_profile != kBundleWireProfile ||
        profile.pool_profile != kOrchardPoolProfile ||
        profile.circuit_profile != kCircuitProfile ||
        profile.effect_commitment_version != kEffectCommitmentVersion)
        throw BackendError(DINERO_ORCHARD_FORMAT);
    DineroOrchardHandle* raw = nullptr;
    Check(dinero_orchard_decode_v1(bytes.data(), bytes.size(), &raw));
    std::shared_ptr<const DineroOrchardHandle> handle(raw, [](const auto* p) {
        // shared_ptr destruction cannot report a cleanup failure. Fail
        // closed rather than silently continuing after a Rust destructor panic.
        if (dinero_orchard_free_v1(const_cast<DineroOrchardHandle*>(p)) != 0)
            std::terminate();
    });
    DineroOrchardFacts facts{};
    Check(dinero_orchard_facts_v1(handle.get(), &facts));
    return ParsedBundle(std::move(handle), facts);
}
Hash ParsedBundle::SigningDigest(const SigningContext& context) const {
    return context.Digest(facts_);
}
VerifiedAuthorization ParsedBundle::VerifyAuthorization(const SigningContext& context) const {
    const auto digest = SigningDigest(context);
    Check(dinero_orchard_verify_v1(handle_.get(), digest.data(), context.RequiredValueBalance()));
    return VerifiedAuthorization(handle_, facts_, digest);
}
} // namespace dinero::orchard
