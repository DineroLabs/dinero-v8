#pragma once
#include "daemon/mempool_transaction.h"
#include <span>

namespace dinero {
// Parsing permission is separate from consensus activation and admission.
enum class RelayTransactionReadMode { HistoricalOnly, AvailableFamilies };
MempoolTransaction DecodeRelayTransaction(
    std::span<const uint8_t> bytes, RelayTransactionReadMode mode);
}
