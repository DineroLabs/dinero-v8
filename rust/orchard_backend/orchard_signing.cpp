#include "orchard_backend.h"
#include <openssl/evp.h>
#include <set>
#include <algorithm>
#include <string_view>
#include <utility>

namespace dinero::orchard {
namespace {
constexpr std::size_t kMaxItems = 4096;
constexpr std::size_t kMaxScriptBytes = 10000;
constexpr std::size_t kMaxContextBytes = 1024 * 1024;
using Bytes = std::vector<std::uint8_t>;
void U32(Bytes& out, std::uint32_t x) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void U64(Bytes& out, std::uint64_t x) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void Append(Bytes& out, std::span<const std::uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}
void Script(Bytes& out, const Bytes& script) {
    U32(out, static_cast<std::uint32_t>(script.size())); Append(out, script);
}
void AddAmount(std::uint64_t x, std::uint64_t& sum) {
    if (x > kMaxMoneyUna || sum > kMaxMoneyUna - x)
        throw std::invalid_argument("Orchard signing amount exceeds money limit");
    sum += x;
}
void AddSize(std::size_t x, std::size_t& sum) {
    if (x > kMaxContextBytes || sum > kMaxContextBytes - x)
        throw std::invalid_argument("Orchard signing context too large");
    sum += x;
}
void CheckScript(const Bytes& script, std::size_t& size) {
    if (script.size() > kMaxScriptBytes)
        throw std::invalid_argument("Orchard signing script too large");
    AddSize(script.size(), size);
}
} // namespace

SigningContext::SigningContext(SigningDomain domain, std::uint32_t lock_time,
                               std::vector<ResolvedInput> inputs,
                               std::vector<TransparentOutput> outputs,
                               std::uint64_t fee, std::int64_t required_balance)
    : domain_(domain), lock_time_(lock_time), inputs_(std::move(inputs)),
      outputs_(std::move(outputs)), fee_(fee), required_balance_(required_balance) {}

SigningContext SigningContext::Create(SigningDomain domain, std::uint32_t lock_time,
                                     const std::vector<ResolvedInput>& inputs,
                                     const std::vector<TransparentOutput>& outputs,
                                     std::uint64_t explicit_fee_una) {
    if (domain.network_code > 2 || domain.branch_id == 0 ||
        std::all_of(domain.genesis_wire.begin(),domain.genesis_wire.end(),[](auto b){return b==0;}) ||
        inputs.size() > kMaxItems || outputs.size() > kMaxItems)
        throw std::invalid_argument("invalid Orchard signing context shape");
    std::uint64_t input_sum = 0, output_sum = 0;
    std::size_t size = 256; // Conservatively covers fixed framing and bundle effect.
    std::set<std::pair<Hash, std::uint32_t>> seen;
    for (const auto& in : inputs) {
        if (!seen.emplace(in.txid_wire, in.output_index).second)
            throw std::invalid_argument("duplicate Orchard signing input");
        AddAmount(in.amount_una, input_sum);
        AddSize(52, size); CheckScript(in.script_pub_key, size);
    }
    for (const auto& out : outputs) {
        AddAmount(out.amount_una, output_sum);
        AddSize(12, size); CheckScript(out.script_pub_key, size);
    }
    AddAmount(explicit_fee_una, output_sum);
    // Both totals are already <= MAX_MONEY < INT64_MAX, so this subtraction
    // is defined. Orchard positive means withdrawal from the shielded pool.
    const auto required = static_cast<std::int64_t>(output_sum) - static_cast<std::int64_t>(input_sum);
    return SigningContext(domain, lock_time, inputs, outputs, explicit_fee_una, required);
}

Hash SigningContext::Digest(const DineroOrchardFacts& facts) const {
    if (facts.action_count == 0 || facts.action_count > kMaxActionsV1)
        throw std::invalid_argument("invalid Orchard action count");
    if (facts.value_balance != required_balance_) throw BackendError(13);
    // Candidate-D contract. This draft is not a frozen outer wire protocol.
    // Typed inputs have a mandatory fee. The fee-presence/profile bytes below
    // cannot be dropped or changed by callers of this API.
    constexpr std::string_view domain = "DIN/orchard-v2/tx-sighash/v1";
    Bytes bytes(domain.begin(), domain.end());
    bytes.push_back(0);
    bytes.push_back(domain_.network_code); Append(bytes, domain_.genesis_wire);
    U32(bytes, domain_.branch_id); U32(bytes, kTransactionVersion); U32(bytes, lock_time_);
    U32(bytes, static_cast<std::uint32_t>(inputs_.size()));
    for (const auto& in : inputs_) {
        Append(bytes, in.txid_wire); U32(bytes, in.output_index); U32(bytes, in.sequence);
        U64(bytes, in.amount_una); Script(bytes, in.script_pub_key);
    }
    U32(bytes, static_cast<std::uint32_t>(outputs_.size()));
    for (const auto& out : outputs_) { U64(bytes, out.amount_una); Script(bytes, out.script_pub_key); }
    bytes.push_back(1); U64(bytes, fee_);
    bytes.insert(bytes.end(), {kBundleWireProfile, kOrchardPoolProfile, kCircuitProfile}); U32(bytes, facts.action_count); Append(bytes, facts.effect);
    Hash digest{};
    unsigned int length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
        length != digest.size()) throw std::runtime_error("Orchard signing hash failed");
    return digest;
}
} // namespace dinero::orchard
