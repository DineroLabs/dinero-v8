#include "daemon/relay_transaction_reader.h"
#include "consensus/shielded/resource_limits.h"
#ifdef DINERO_HAS_ORCHARD_RELAY_READER
#include "primitives/transaction_reader.h"
#endif
#include <algorithm>
#include <stdexcept>

namespace dinero {
MempoolTransaction DecodeRelayTransaction(
    std::span<const uint8_t> bytes, RelayTransactionReadMode mode) {
    if (mode != RelayTransactionReadMode::HistoricalOnly &&
        mode != RelayTransactionReadMode::AvailableFamilies)
        throw std::invalid_argument("Unknown relay transaction read mode");
#ifdef DINERO_HAS_ORCHARD_RELAY_READER
    const auto parsed = ParsedTransaction::DecodeExact(bytes,
        mode == RelayTransactionReadMode::HistoricalOnly
            ? TransactionReadMode::HistoricalOnly : TransactionReadMode::StagedOrchard);
    return parsed.IsOrchard() ? MempoolTransaction::FromOrchard(parsed.Orchard())
                             : MempoolTransaction(parsed.Historical());
#else
    if (TransactionSerializer::HasOrchardEnvelopeMarker(bytes.data(), bytes.size()))
        throw std::invalid_argument("Orchard transaction reader unavailable");
    const auto header = bytes.first(std::min<size_t>(4, bytes.size()));
    const auto limit = consensus::shielded::WireTxByteLimit({header.begin(), header.end()});
    if (bytes.empty() || bytes.size() > limit)
        throw std::invalid_argument("Invalid historical relay transaction size");
    const std::vector<uint8_t> input(bytes.begin(), bytes.end());
    Transaction tx; size_t consumed = 0;
    if (!TransactionSerializer::Deserialize(tx, input, consumed) || consumed != input.size())
        throw std::invalid_argument("Invalid historical relay transaction encoding");
    return MempoolTransaction(tx);
#endif
}
}
