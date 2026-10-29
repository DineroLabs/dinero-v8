#include "primitives/transaction_reader.h"
#include "consensus/shielded/resource_limits.h"
#include <algorithm>
#include <stdexcept>

namespace dinero {
namespace {
[[noreturn]] void Invalid() { throw std::invalid_argument("invalid typed transaction encoding"); }
uint256 WireHash(const orchard::Hash& bytes) {
    uint256 hash;
    std::copy(bytes.begin(),bytes.end(),hash.begin());
    return hash;
}
} // namespace
ParsedTransaction::ParsedTransaction(Transaction tx): value_(std::move(tx)) {}
ParsedTransaction::ParsedTransaction(orchard::TransactionEnvelope tx): value_(std::move(tx)) {}
std::pair<ParsedTransaction,size_t> ParsedTransaction::DecodePrefix(
    std::span<const uint8_t> bytes, TransactionReadMode mode) {
    if (mode != TransactionReadMode::HistoricalOnly && mode != TransactionReadMode::StagedOrchard)
        Invalid();
    if (TransactionSerializer::HasOrchardEnvelopeMarker(bytes.data(),bytes.size())) {
        if (mode != TransactionReadMode::StagedOrchard) Invalid();
        // A claimed family never falls back, including bad magic/profile/body.
        auto [envelope,consumed]=orchard::TransactionEnvelope::DecodePrefix(bytes);
        return {ParsedTransaction(std::move(envelope)),consumed};
    }
    // Reuse the host's historical allocation ceiling and actual parser. Copy
    // only one bounded window, never the entire remaining block/network buffer.
    const auto header=bytes.first(std::min<size_t>(4,bytes.size()));
    const auto limit=consensus::shielded::WireTxByteLimit({header.begin(),header.end()});
    const auto window=bytes.first(std::min(bytes.size(),limit));
    const std::vector<uint8_t> input(window.begin(),window.end());
    Transaction tx;
    size_t consumed=0;
    if (!TransactionSerializer::Deserialize(tx,input,consumed) || consumed==0 || consumed>window.size())
        Invalid();
    return {ParsedTransaction(std::move(tx)),consumed};
}
ParsedTransaction ParsedTransaction::DecodeExact(std::span<const uint8_t> bytes, TransactionReadMode mode) {
    auto [tx,consumed]=DecodePrefix(bytes,mode);
    if (consumed!=bytes.size()) Invalid();
    return tx;
}
bool ParsedTransaction::IsOrchard() const noexcept {
    return std::holds_alternative<orchard::TransactionEnvelope>(value_);
}
const Transaction& ParsedTransaction::Historical() const { return std::get<Transaction>(value_); }
const orchard::TransactionEnvelope& ParsedTransaction::Orchard() const {
    return std::get<orchard::TransactionEnvelope>(value_);
}
std::vector<uint8_t> ParsedTransaction::Serialize(TxSerializationMode mode) const {
    if (!IsOrchard()) return Historical().Serialize(mode);
    return mode==TxSerializationMode::WithWitness ? Orchard().CanonicalBytes() : Orchard().TxidPreimage();
}
TxId ParsedTransaction::GetTxid() const {
    return IsOrchard() ? TxId(WireHash(Orchard().Txid())) : Historical().GetTxid();
}
WTxId ParsedTransaction::GetWtxid() const {
    return IsOrchard() ? WTxId(WireHash(Orchard().Wtxid())) : Historical().GetWtxid();
}
size_t ParsedTransaction::GetSize() const { return Serialize(TxSerializationMode::WithWitness).size(); }
size_t ParsedTransaction::GetBaseSize() const { return Serialize(TxSerializationMode::WithoutWitness).size(); }
size_t ParsedTransaction::GetWeight() const { return 3*GetBaseSize()+GetSize(); }
} // namespace dinero
