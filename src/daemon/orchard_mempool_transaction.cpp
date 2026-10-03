#include "daemon/mempool_transaction.h"
#include "orchard_transaction.h"
#include <algorithm>

namespace dinero {
namespace {
uint256 WireHash(const orchard::Hash& bytes) {
    uint256 hash;
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    return hash;
}
}
struct MempoolTransaction::OrchardBody final : MempoolTransaction::Body {
    explicit OrchardBody(const orchard::TransactionEnvelope& tx) : transaction(tx) {
        for (const auto& input : transaction.Inputs())
            inputs.emplace_back(TxId(WireHash(input.txid_wire)), input.output_index);
        const auto& facts = transaction.UnverifiedFacts();
        nullifiers.reserve(facts.action_count);
        for (size_t i = 0; i < facts.action_count; ++i) {
            std::array<uint8_t, 32> nullifier;
            std::copy_n(facts.nullifiers[i], nullifier.size(), nullifier.begin());
            nullifiers.push_back(nullifier);
        }
    }
    const orchard::TransactionEnvelope* Orchard() const noexcept override { return &transaction; }
    TxId GetTxid() const override { return TxId(WireHash(transaction.Txid())); }
    WTxId GetWtxid() const override { return WTxId(WireHash(transaction.Wtxid())); }
    std::vector<uint8_t> Serialize(TxSerializationMode mode) const override {
        return mode == TxSerializationMode::WithWitness ? transaction.CanonicalBytes() : transaction.TxidPreimage();
    }
    size_t GetSize() const override { return transaction.CanonicalBytes().size(); }
    size_t GetBaseSize() const override { return transaction.TxidPreimage().size(); }
    size_t GetWeight() const override { return 3 * GetBaseSize() + GetSize(); }
    const std::vector<OutPoint>& Inputs() const noexcept override { return inputs; }
    const std::vector<std::array<uint8_t, 32>>& OrchardNullifiers() const noexcept override { return nullifiers; }
    size_t OutputCount() const noexcept override { return transaction.Outputs().size(); }
    consensus::UTXOEntry OutputCoin(size_t index, uint32_t height) const override {
        const auto& output = transaction.Outputs().at(index);
        return {AmountUna::Una(output.amount_una), output.script_pub_key, height, false};
    }
    std::optional<uint64_t> ExplicitFee() const override { return transaction.ExplicitFee(); }
    const orchard::TransactionEnvelope transaction;
    std::vector<OutPoint> inputs;
    std::vector<std::array<uint8_t, 32>> nullifiers;
};
MempoolTransaction MempoolTransaction::FromOrchard(const orchard::TransactionEnvelope& tx) {
    return MempoolTransaction(std::make_shared<OrchardBody>(tx));
}
} // namespace dinero
