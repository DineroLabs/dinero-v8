#pragma once
#include <cstdint>
#include <memory>

namespace dinero {
class RuntimeBlockBody;
enum class RuntimeBlockDirection { Connect, Disconnect };

// Trusted daemon consumers prepare their complete typed block event before the
// chainstate write. Preparation may allocate or refuse; it must not mutate
// canonical state or publish any event. The returned object owns everything
// required to notify/reconcile all consumers after durability and tip publication.
// No historical Block/Transaction conversion is permitted for a mixed body.
class PreparedRuntimeBlockNotifications {
public:
    virtual ~PreparedRuntimeBlockNotifications() = default;
    virtual void PublishAfterCommit() noexcept = 0;
};
class RuntimeBlockNotifications {
public:
    virtual ~RuntimeBlockNotifications() = default;
    [[nodiscard]] virtual std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(
        const RuntimeBlockBody&, uint32_t height, RuntimeBlockDirection) = 0;
};
} // namespace dinero
