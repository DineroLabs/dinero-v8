#pragma once
#include "orchard_backend.h"
#include <utility>

namespace dinero::orchard {
// Draft, disjoint outer envelope. This is not selected by numeric version 7
// alone. No existing Transaction parser or live admission caller is changed.
inline constexpr std::size_t kMaxTransactionBytes = 100000;
struct EnvelopeInput {
    Hash txid_wire{};
    std::uint32_t output_index = 0;
    std::uint32_t sequence = 0;
    // Profile 1 has no scriptSig authorization: it must be empty.
    // Host admission still has to validate native witness programs and stacks.
    std::vector<std::uint8_t> script_sig;
    std::vector<std::vector<std::uint8_t>> witness;
};
struct PreviousOutput {
    Hash txid_wire{};
    std::uint32_t output_index = 0;
    std::uint64_t amount_una = 0;
    std::vector<std::uint8_t> script_pub_key;
};
class TransactionEnvelope;
// Orchard authorization bound to an exact envelope identity. This result is
// NOT a scripts/coin-view/chainstate validation result.
class VerifiedEnvelopeAuthorization {
public:
    const VerifiedAuthorization& Orchard() const noexcept { return authorization_; }
    const Hash& Txid() const noexcept { return txid_; }
    const Hash& Wtxid() const noexcept { return wtxid_; }
    const std::vector<std::uint8_t>& CanonicalBytes() const noexcept { return bytes_; }
private:
    friend class TransactionEnvelope;
    VerifiedEnvelopeAuthorization(VerifiedAuthorization authorization, Hash txid,
        Hash wtxid, std::vector<std::uint8_t> bytes)
        : authorization_(std::move(authorization)), txid_(txid), wtxid_(wtxid), bytes_(std::move(bytes)) {}
    const VerifiedAuthorization authorization_;
    const Hash txid_, wtxid_;
    const std::vector<std::uint8_t> bytes_;
};
class TransactionEnvelope {
public:
    [[nodiscard]] static TransactionEnvelope Create(std::uint32_t lock_time,
        const std::vector<EnvelopeInput>& inputs,
        const std::vector<TransparentOutput>& outputs, std::uint64_t explicit_fee,
        std::span<const std::uint8_t> bundle);
    [[nodiscard]] static TransactionEnvelope DecodeExact(std::span<const std::uint8_t> bytes);
    // For block streams: consumes ONE bounded envelope, retaining later bytes.
    [[nodiscard]] static std::pair<TransactionEnvelope, std::size_t>
        DecodePrefix(std::span<const std::uint8_t> bytes);
    const std::vector<std::uint8_t>& CanonicalBytes() const noexcept { return bytes_; }
    const std::vector<EnvelopeInput>& Inputs() const noexcept { return inputs_; }
    const std::vector<TransparentOutput>& Outputs() const noexcept { return outputs_; }
    std::uint32_t LockTime() const noexcept { return lock_time_; }
    std::uint64_t ExplicitFee() const noexcept { return fee_; }
    const DineroOrchardFacts& UnverifiedFacts() const noexcept { return bundle_.UnverifiedFacts(); }
    Hash Txid() const;
    Hash Wtxid() const;
    // Previous outputs MUST be resolved by the host's authenticated coin view,
    // in input order. Matching the outpoints here does not prove provenance.
    Hash SigningDigest(SigningDomain domain, std::span<const PreviousOutput> coins) const;
    [[nodiscard]] VerifiedEnvelopeAuthorization VerifyAuthorization(
        SigningDomain domain, std::span<const PreviousOutput> coins) const;
private:
    TransactionEnvelope(std::uint32_t lock_time, std::vector<EnvelopeInput> inputs,
        std::vector<TransparentOutput> outputs, std::uint64_t fee,
        std::vector<std::uint8_t> bundle_bytes, ParsedBundle bundle,
        std::vector<std::uint8_t> bytes);
    SigningContext Context(SigningDomain, std::span<const PreviousOutput>) const;
    const std::uint32_t lock_time_;
    const std::vector<EnvelopeInput> inputs_;
    const std::vector<TransparentOutput> outputs_;
    const std::uint64_t fee_;
    const std::vector<std::uint8_t> bundle_bytes_;
    const ParsedBundle bundle_;
    const std::vector<std::uint8_t> bytes_;
};
} // namespace dinero::orchard
