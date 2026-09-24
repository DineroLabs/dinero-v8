#include "orchard_transaction.h"
#include <openssl/evp.h>
#include <algorithm>
#include <set>
#include <string_view>

namespace dinero::orchard {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 15> kPrefix{
    static_cast<std::uint8_t>(kTransactionVersion),
    static_cast<std::uint8_t>(kTransactionVersion>>8),
    static_cast<std::uint8_t>(kTransactionVersion>>16),
    static_cast<std::uint8_t>(kTransactionVersion>>24), 0,0, 'D','N','O','R','C','H','T','X', 1};
constexpr std::size_t kHeaderSize = kPrefix.size() + 4;
constexpr std::size_t kMaxInputs = 4096, kMaxOutputs = 4096;
constexpr std::size_t kMaxScript = 10000, kMaxWitnessItems = 4096;
constexpr std::size_t kMaxWitnessPerInput = 100, kMaxBundle = 64 * 1024;
[[noreturn]] void Invalid() { throw std::invalid_argument("invalid draft Orchard transaction envelope"); }
void Size(std::size_t n, std::size_t& sum) {
    if (n > kMaxTransactionBytes || sum > kMaxTransactionBytes - n) Invalid();
    sum += n;
}
void Money(std::uint64_t n, std::uint64_t& sum) {
    if (n > kMaxMoneyUna || sum > kMaxMoneyUna - n) Invalid();
    sum += n;
}
void U32(Bytes& b, std::uint32_t n) {
    for (unsigned i=0;i<4;++i) b.push_back(static_cast<std::uint8_t>(n>>(i*8)));
}
void U64(Bytes& b, std::uint64_t n) {
    for (unsigned i=0;i<8;++i) b.push_back(static_cast<std::uint8_t>(n>>(i*8)));
}
void Blob(Bytes& b, std::span<const std::uint8_t> v) {
    U32(b,static_cast<std::uint32_t>(v.size())); b.insert(b.end(),v.begin(),v.end());
}
// Validate aggregate counts and sizes BEFORE copying nested buffers.
std::size_t Check(const std::vector<EnvelopeInput>& inputs,
                  const std::vector<TransparentOutput>& outputs,
                  std::uint64_t fee, std::size_t bundle_size) {
    if (inputs.size()>kMaxInputs || outputs.size()>kMaxOutputs || bundle_size>kMaxBundle) Invalid();
    std::size_t size=kHeaderSize+4+4+1+8+4+4, witnesses=0;
    std::uint64_t total=0;
    std::set<std::pair<Hash,std::uint32_t>> outpoints;
    for (const auto& in:inputs) {
        if (!outpoints.emplace(in.txid_wire,in.output_index).second) Invalid();
        if (in.output_index==0xffffffff &&
            std::all_of(in.txid_wire.begin(),in.txid_wire.end(),[](auto x){return x==0;})) Invalid();
        if (in.script_sig.size()>kMaxScript || in.witness.size()>kMaxWitnessPerInput ||
            in.witness.size()>kMaxWitnessItems-witnesses) Invalid();
        witnesses+=in.witness.size();
        Size(32+4+4+4+4,size); Size(in.script_sig.size(),size);
        for (const auto& item:in.witness) {
            if (item.size()>kMaxScript) Invalid();
            Size(4,size); Size(item.size(),size);
        }
    }
    for (const auto& out:outputs) {
        Money(out.amount_una,total);
        if (out.script_pub_key.size()>kMaxScript) Invalid();
        Size(12,size); Size(out.script_pub_key.size(),size);
    }
    Money(fee,total); Size(bundle_size,size);
    return size;
}
Bytes Encode(std::uint32_t lock_time, const std::vector<EnvelopeInput>& inputs,
             const std::vector<TransparentOutput>& outputs, std::uint64_t fee,
             std::span<const std::uint8_t> bundle, bool witness) {
    const auto bound=Check(inputs,outputs,fee,bundle.size());
    Bytes b; b.reserve(bound); b.insert(b.end(),kPrefix.begin(),kPrefix.end()); U32(b,0);
    U32(b,static_cast<std::uint32_t>(inputs.size()));
    for (const auto& in:inputs) {
        b.insert(b.end(),in.txid_wire.begin(),in.txid_wire.end()); U32(b,in.output_index);
        Blob(b,in.script_sig); U32(b,in.sequence);
        U32(b,witness ? static_cast<std::uint32_t>(in.witness.size()) : 0);
        if (witness) for (const auto& item:in.witness) Blob(b,item);
    }
    U32(b,static_cast<std::uint32_t>(outputs.size()));
    for (const auto& out:outputs) { U64(b,out.amount_una); Blob(b,out.script_pub_key); }
    b.push_back(1); U64(b,fee); Blob(b,bundle); U32(b,lock_time);
    const auto payload=static_cast<std::uint32_t>(b.size()-kHeaderSize);
    for (unsigned i=0;i<4;++i) b[kPrefix.size()+i]=static_cast<std::uint8_t>(payload>>(8*i));
    return b;
}
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes): bytes_(bytes) {}
    std::span<const std::uint8_t> Take(std::size_t n) {
        if (n>bytes_.size()-offset_) Invalid();
        auto out=bytes_.subspan(offset_,n); offset_+=n; return out;
    }
    std::uint32_t U32() {
        auto b=Take(4); std::uint32_t x=0;
        for (unsigned i=0;i<4;++i) x|=std::uint32_t(b[i])<<(8*i); return x;
    }
    std::uint64_t U64() {
        auto b=Take(8); std::uint64_t x=0;
        for (unsigned i=0;i<8;++i) x|=std::uint64_t(b[i])<<(8*i); return x;
    }
    Bytes Blob(std::size_t max) { auto n=U32(); if(n>max) Invalid(); auto b=Take(n); return {b.begin(),b.end()}; }
    std::size_t Remaining() const { return bytes_.size()-offset_; }
private:
    std::span<const std::uint8_t> bytes_; std::size_t offset_=0;
};
Hash Hash256(std::span<const std::uint8_t> bytes) {
    Hash first{}, second{}; unsigned n=0;
    if (EVP_Digest(bytes.data(),bytes.size(),first.data(),&n,EVP_sha256(),nullptr)!=1 || n!=32 ||
        EVP_Digest(first.data(),first.size(),second.data(),&n,EVP_sha256(),nullptr)!=1 || n!=32)
        throw std::runtime_error("Orchard transaction identity hash failed");
    return second;
}
} // namespace
TransactionEnvelope::TransactionEnvelope(std::uint32_t lock_time, std::vector<EnvelopeInput> inputs,
    std::vector<TransparentOutput> outputs, std::uint64_t fee, Bytes bundle_bytes,
    ParsedBundle bundle, Bytes bytes)
    : lock_time_(lock_time),inputs_(std::move(inputs)),outputs_(std::move(outputs)),fee_(fee),
      bundle_bytes_(std::move(bundle_bytes)),bundle_(std::move(bundle)),bytes_(std::move(bytes)) {}
TransactionEnvelope TransactionEnvelope::Create(std::uint32_t lock_time,
    const std::vector<EnvelopeInput>& inputs, const std::vector<TransparentOutput>& outputs,
    std::uint64_t fee, std::span<const std::uint8_t> bundle) {
    (void)Check(inputs,outputs,fee,bundle.size());
    auto parsed=ParsedBundle::Decode(bundle);
    auto bytes=Encode(lock_time,inputs,outputs,fee,bundle,true);
    return TransactionEnvelope(lock_time,inputs,outputs,fee,{bundle.begin(),bundle.end()},
                               std::move(parsed),std::move(bytes));
}
std::pair<TransactionEnvelope,std::size_t> TransactionEnvelope::DecodePrefix(std::span<const std::uint8_t> bytes) {
    Reader frame(bytes);
    auto prefix=frame.Take(kPrefix.size());
    if (!std::equal(prefix.begin(),prefix.end(),kPrefix.begin())) Invalid();
    const auto payload=frame.U32();
    if (payload>kMaxTransactionBytes-kHeaderSize) Invalid();
    auto body=frame.Take(payload); Reader r(body);
    const auto ni=r.U32();
    if (ni>kMaxInputs || ni>r.Remaining()/48) Invalid();
    std::vector<EnvelopeInput> inputs; inputs.reserve(ni);
    std::size_t witnesses=0;
    for (std::uint32_t i=0;i<ni;++i) {
        EnvelopeInput in;
        auto h=r.Take(32); std::copy(h.begin(),h.end(),in.txid_wire.begin());
        in.output_index=r.U32(); in.script_sig=r.Blob(kMaxScript); in.sequence=r.U32();
        const auto nw=r.U32();
        if (nw>kMaxWitnessPerInput || nw>kMaxWitnessItems-witnesses || nw>r.Remaining()/4) Invalid();
        witnesses+=nw; in.witness.reserve(nw);
        for (std::uint32_t j=0;j<nw;++j) in.witness.push_back(r.Blob(kMaxScript));
        inputs.push_back(std::move(in));
    }
    const auto no=r.U32();
    if (no>kMaxOutputs || no>r.Remaining()/12) Invalid();
    std::vector<TransparentOutput> outputs; outputs.reserve(no);
    for (std::uint32_t i=0;i<no;++i) { auto amount=r.U64(); outputs.push_back({amount,r.Blob(kMaxScript)}); }
    if (r.Take(1)[0]!=1) Invalid();
    const auto fee=r.U64(); const auto bundle=r.Blob(kMaxBundle); const auto lock_time=r.U32();
    if (r.Remaining()!=0) Invalid();
    auto result=Create(lock_time,inputs,outputs,fee,bundle);
    const auto consumed=kHeaderSize+payload;
    if (result.CanonicalBytes().size()!=consumed ||
        !std::equal(result.CanonicalBytes().begin(),result.CanonicalBytes().end(),bytes.begin())) Invalid();
    return {std::move(result),consumed};
}
TransactionEnvelope TransactionEnvelope::DecodeExact(std::span<const std::uint8_t> bytes) {
    if (bytes.size()>kMaxTransactionBytes) Invalid();
    auto [result,consumed]=DecodePrefix(bytes);
    if (consumed!=bytes.size()) Invalid();
    return result;
}
SigningContext TransactionEnvelope::Context(SigningDomain domain, std::span<const PreviousOutput> coins) const {
    if (coins.size()!=inputs_.size()) Invalid();
    std::size_t size=0;
    std::uint64_t total=0;
    for (const auto& coin:coins) {
        if (coin.script_pub_key.size()>kMaxScript) Invalid();
        Size(52,size); Size(coin.script_pub_key.size(),size); Money(coin.amount_una,total);
    }
    std::vector<ResolvedInput> inputs; inputs.reserve(inputs_.size());
    for (std::size_t i=0;i<inputs_.size();++i) {
        const auto& in=inputs_[i]; const auto& coin=coins[i];
        if (in.txid_wire!=coin.txid_wire || in.output_index!=coin.output_index) Invalid();
        inputs.push_back({in.txid_wire,in.output_index,in.sequence,coin.amount_una,coin.script_pub_key});
    }
    return SigningContext::Create(domain,lock_time_,inputs,outputs_,fee_);
}
Hash TransactionEnvelope::SigningDigest(SigningDomain domain, std::span<const PreviousOutput> coins) const {
    return bundle_.SigningDigest(Context(domain,coins));
}
VerifiedEnvelopeAuthorization TransactionEnvelope::VerifyAuthorization(SigningDomain domain,
    std::span<const PreviousOutput> coins) const {
    auto authorization=bundle_.VerifyAuthorization(Context(domain,coins));
    return VerifiedEnvelopeAuthorization(std::move(authorization),Txid(),Wtxid(),bytes_);
}
Hash TransactionEnvelope::Txid() const { return Hash256(Encode(lock_time_,inputs_,outputs_,fee_,bundle_bytes_,false)); }
Hash TransactionEnvelope::Wtxid() const { return Hash256(bytes_); }
} // namespace dinero::orchard
