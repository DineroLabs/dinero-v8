#pragma once
#include "daemon/runtime_block_outbox.h"
#include <optional>
#include <string>

namespace dinero {
class UTXOIndex;
struct RuntimeIndexProgress {
    RuntimeOutboxCursor cursor;
    uint256 origin_hash, tip_hash;
    uint32_t origin_height = 0, tip_height = 0;
};
// Owns real UTXOIndex effects and one progress row in its SQLite transaction.
// The event MUST come from the selected service's checked delivery reader.
// This receipt covers only this index after the source origin, not historical
// baseline completeness, other wallet stores or all-consumer readiness.
class RuntimeIndexDelivery {
public:
    static std::optional<RuntimeIndexProgress> Read(UTXOIndex&, const std::string& wallet_identity);
    static RuntimeIndexProgress Apply(UTXOIndex&, const std::string& wallet_identity,
                                      const RuntimeOutboxEvent&);
};
} // namespace dinero
