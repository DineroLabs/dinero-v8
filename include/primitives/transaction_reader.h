#pragma once

#include "primitives/transaction.h"
#include "orchard_transaction.h"
#include <span>
#include <variant>

namespace dinero {

// Parsing permission is explicit and is NOT a consensus activation setting.
enum class TransactionReadMode { HistoricalOnly, StagedOrchard };

// An immutable parsed value, not an admission/validation result. In particular,
// Orchard cannot implicitly convert to Transaction and enter a legacy validator.
class ParsedTransaction {
public:
    static ParsedTransaction DecodeExact(std::span<const uint8_t>, TransactionReadMode);
    static std::pair<ParsedTransaction, size_t> DecodePrefix(
        std::span<const uint8_t>, TransactionReadMode);

    bool IsOrchard() const noexcept;
    const Transaction& Historical() const; // throws for Orchard
    const orchard::TransactionEnvelope& Orchard() const; // throws for historical
    std::vector<uint8_t> Serialize(TxSerializationMode) const;
    TxId GetTxid() const;
    WTxId GetWtxid() const;
    size_t GetSize() const;
    size_t GetBaseSize() const;
    size_t GetWeight() const;
private:
    explicit ParsedTransaction(Transaction);
    explicit ParsedTransaction(orchard::TransactionEnvelope);
    const std::variant<Transaction, orchard::TransactionEnvelope> value_;
};
} // namespace dinero
