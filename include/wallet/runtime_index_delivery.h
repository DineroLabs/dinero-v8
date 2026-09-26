#pragma once
#include "daemon/runtime_block_outbox.h"
#include <optional>
#include <string>

namespace dinero {
class UTXOIndex;
class WalletManager;
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
    // Pins the selected wallet and durably establishes its database binding
    // before touching the index. Source acquisition precedes this call; the
    // caller captures the intended session before releasing wallet ownership.
    static std::optional<RuntimeIndexProgress> ReadForWallet(WalletManager&, UTXOIndex&, uint64_t expected_session);
    static RuntimeIndexProgress ApplyForWallet(WalletManager&, UTXOIndex&, uint64_t expected_session, const RuntimeOutboxEvent&);
private:
    friend struct RuntimeIndexDeliveryTestAccess;
    static std::optional<RuntimeIndexProgress> Read(UTXOIndex&, const std::string& wallet_identity);
    static RuntimeIndexProgress Apply(UTXOIndex&, const std::string& wallet_identity,
                                      const RuntimeOutboxEvent&);
};
// Ordinary wallet UTXOs/history and their source progress share the selected
// wallet SQLite transaction. This is independent of the index/account receipts:
// a coordinator must reconcile every store before declaring wallet readiness.
// Acquire the selected checked source before this call. Initial adoption does
// not certify the pre-origin baseline or key ownership.
class RuntimeOrdinaryDelivery {
public:
    static std::optional<RuntimeIndexProgress> ReadForWallet(WalletManager&, uint64_t expected_session);
    static RuntimeIndexProgress ApplyForWallet(WalletManager&, uint64_t expected_session, const RuntimeOutboxEvent&);
};
} // namespace dinero
