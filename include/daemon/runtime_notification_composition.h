#pragma once
#include "daemon/runtime_block_notifications.h"
#include <array>
#include <memory>
#include <span>

namespace dinero {
class RuntimeDeliveryWorker;
// Chainstate already owns transparent pool/bridge/relay-tip publication. These
// are the remaining consumer families; omission is different from explicit
// absence. The daemon composition owner must derive bindings from real config.
enum class RuntimeConsumerKind : size_t {
    WalletAndReadmission, PoolAccounting, ChainOracle, TimeOracle,
    TransactionOracle, Vault, ExtraWalletNotifiers, Count
};
struct RuntimeConsumerBinding {
    RuntimeConsumerKind kind;
    // Null declares that family explicitly absent. It is not a success token
    // for a configured but unsupported consumer.
    std::shared_ptr<RuntimeBlockNotifications> consumer;
};

// Owns an immutable complete declaration supplied by the trusted daemon owner.
// It cannot discover optional registrations or certify their absence itself.
// Not installed by DaemonApp until all configured families are implemented.
class RuntimeNotificationComposition final : public RuntimeBlockNotifications {
public:
    explicit RuntimeNotificationComposition(std::span<const RuntimeConsumerBinding>);
    std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(
        const RuntimeBlockBody&,uint32_t,RuntimeBlockDirection) override;
    std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(
        std::shared_ptr<const RuntimeReorgPlan>) override;
private:
    std::array<std::shared_ptr<RuntimeBlockNotifications>,static_cast<size_t>(RuntimeConsumerKind::Count)> consumers_{};
};
// The adapter and prepared handoffs own only a thread-independent wake mailbox.
// Neither can retain or destroy a worker, join a thread, or own the source.
// The canonical source already owns durable event/reorg retention. Neither
// adapter nor composition writes another journal or acknowledges a consumer.
std::shared_ptr<RuntimeBlockNotifications> MakeRuntimeWalletNotifications(
    const RuntimeDeliveryWorker&);
} // namespace dinero
