#pragma once
#include "consensus/outpoint.h"
#include "consensus/utxo_entry.h"
#include "primitives/transaction.h"
#include <memory>
#include <optional>
#include <stdexcept>

namespace dinero {
namespace orchard { class TransactionEnvelope; }

// An immutable transaction body, not an admission result. Each family retains
// its real representation; wrong-family access throws instead of converting.
// The default state has no body and cannot be read as an empty transaction.
class MempoolTransaction {
private:
    struct Body {
        virtual ~Body() = default;
        virtual const Transaction* Historical() const noexcept { return nullptr; }
        virtual const orchard::TransactionEnvelope* Orchard() const noexcept { return nullptr; }
        virtual TxId GetTxid() const = 0;
        virtual WTxId GetWtxid() const = 0;
        virtual std::vector<uint8_t> Serialize(TxSerializationMode) const = 0;
        virtual size_t GetSize() const = 0;
        virtual size_t GetBaseSize() const = 0;
        virtual size_t GetWeight() const = 0;
        virtual const std::vector<OutPoint>& Inputs() const noexcept = 0;
        virtual size_t OutputCount() const noexcept = 0;
        virtual consensus::UTXOEntry OutputCoin(size_t, uint32_t) const = 0;
        virtual std::optional<uint64_t> ExplicitFee() const = 0;
    };
    struct HistoricalBody final : Body {
        explicit HistoricalBody(const Transaction& value) : transaction(value) {
            for (const auto& input : transaction.vin)
                inputs.emplace_back(input.prevout.txid, input.prevout.vout);
        }
        const Transaction* Historical() const noexcept override { return &transaction; }
        TxId GetTxid() const override { return transaction.GetTxid(); }
        WTxId GetWtxid() const override { return transaction.GetWtxid(); }
        std::vector<uint8_t> Serialize(TxSerializationMode mode) const override { return transaction.Serialize(mode); }
        size_t GetSize() const override { return transaction.GetSize(); }
        size_t GetBaseSize() const override { return transaction.GetBaseSize(); }
        size_t GetWeight() const override { return transaction.GetWeight(); }
        const std::vector<OutPoint>& Inputs() const noexcept override { return inputs; }
        size_t OutputCount() const noexcept override { return transaction.vout.size(); }
        consensus::UTXOEntry OutputCoin(size_t index, uint32_t height) const override {
            const auto& output = transaction.vout.at(index);
            return {output.value, output.scriptPubKey, height, false,
                    output.is_confidential, output.commitment};
        }
        std::optional<uint64_t> ExplicitFee() const override {
            return transaction.HasExplicitFee() ? std::optional<uint64_t>(transaction.GetExplicitFee()) : std::nullopt;
        }
        const Transaction transaction;
        std::vector<OutPoint> inputs;
    };
    struct OrchardBody; // Implementation is linked only with the Orchard backend.
    explicit MempoolTransaction(std::shared_ptr<const Body> body) : body_(std::move(body)) {}
    const Body& Checked() const {
        if (!body_) throw std::logic_error("Mempool transaction body unavailable");
        return *body_;
    }
    std::shared_ptr<const Body> body_;
public:
    MempoolTransaction() = default;
    explicit MempoolTransaction(const Transaction& tx) : body_(std::make_shared<HistoricalBody>(tx)) {}
    static MempoolTransaction FromOrchard(const orchard::TransactionEnvelope&);
    bool HasBody() const noexcept { return bool(body_); }
    bool IsOrchard() const noexcept { return body_ && body_->Orchard(); }
    const Transaction& Historical() const {
        const auto* tx = Checked().Historical();
        if (!tx) throw std::logic_error("Orchard transaction has no historical body");
        return *tx;
    }
    const orchard::TransactionEnvelope& Orchard() const {
        const auto* tx = Checked().Orchard();
        if (!tx) throw std::logic_error("Historical transaction has no Orchard body");
        return *tx;
    }
    TxId GetTxid() const { return Checked().GetTxid(); }
    WTxId GetWtxid() const { return Checked().GetWtxid(); }
    std::vector<uint8_t> Serialize(TxSerializationMode mode = TxSerializationMode::WithWitness) const {
        if (mode != TxSerializationMode::WithWitness && mode != TxSerializationMode::WithoutWitness)
            throw std::invalid_argument("Unknown mempool transaction serialization mode");
        return Checked().Serialize(mode);
    }
    size_t GetSize() const { return Checked().GetSize(); }
    size_t GetBaseSize() const { return Checked().GetBaseSize(); }
    size_t GetWeight() const { return Checked().GetWeight(); }
    size_t GetVirtualSize() const { return (GetWeight() + 3) / 4; }
    const std::vector<OutPoint>& Inputs() const { return Checked().Inputs(); }
    size_t OutputCount() const { return Checked().OutputCount(); }
    // Structural output metadata for the existing pool overlay. The caller
    // supplies the entry height; this is not proof of admission or provenance.
    consensus::UTXOEntry OutputCoin(size_t index, uint32_t height) const {
        return Checked().OutputCoin(index, height);
    }
    std::optional<uint64_t> ExplicitFee() const { return Checked().ExplicitFee(); }
};
} // namespace dinero
