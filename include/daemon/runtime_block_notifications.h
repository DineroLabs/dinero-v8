#pragma once
#include <cstdint>
#include <cstddef>
#include <exception>
#include <memory>

namespace dinero {
class RuntimeBlockBody;
class RuntimeReorgPlan;
struct RuntimeReorgProgress {
    size_t disconnected = 0, connected = 0;
    // False means canonical recovery is required even in the current process.
    // A legacy transition may throw after durability but before returning.
    // Counts then give a confirmed lower bound, NEVER permission to cancel.
    bool complete = false;
};

// Preparation must durably retain the complete plan before returning. On restart
// the consumer resolves committed progress from canonical chainstate, not from
// the last callback: the process can stop between commit and notification.
class PreparedRuntimeReorgNotifications {
public:
    virtual ~PreparedRuntimeReorgNotifications() = default;
    virtual void Finish(RuntimeReorgProgress) noexcept = 0;
};

// Reports confirmed prefixes and distinguishes a completed plan from interruption.
// Disconnect order is tip-first; readmission reverses that committed prefix.
class RuntimeReorgTransition final {
public:
    RuntimeReorgTransition(std::unique_ptr<PreparedRuntimeReorgNotifications> prepared,
                          size_t disconnects, size_t connects)
        : prepared_(std::move(prepared)), disconnects_(disconnects), connects_(connects) {
        if (!prepared_) std::terminate();
    }
    RuntimeReorgTransition(const RuntimeReorgTransition&) = delete;
    RuntimeReorgTransition& operator=(const RuntimeReorgTransition&) = delete;
    ~RuntimeReorgTransition() { prepared_->Finish(progress_); }
    void Disconnected() noexcept {
        if (progress_.complete || progress_.connected || progress_.disconnected == disconnects_) std::terminate();
        ++progress_.disconnected;
    }
    void Connected() noexcept {
        if (progress_.complete || progress_.disconnected != disconnects_ || progress_.connected == connects_) std::terminate();
        ++progress_.connected;
    }
    void Complete() noexcept {
        if (progress_.disconnected != disconnects_ || progress_.connected != connects_) std::terminate();
        progress_.complete = true;
    }
private:
    std::unique_ptr<PreparedRuntimeReorgNotifications> prepared_;
    const size_t disconnects_, connects_;
    RuntimeReorgProgress progress_;
};
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
    // Default refusal keeps a per-block-only consumer from silently losing
    // disconnected transactions. The owned plan includes historical transactions
    // when the fork crosses the activation boundary. No admission is implied.
    [[nodiscard]] virtual std::unique_ptr<PreparedRuntimeReorgNotifications> PrepareReorg(
        std::shared_ptr<const RuntimeReorgPlan>) { return {}; }
    [[nodiscard]] virtual std::unique_ptr<PreparedRuntimeBlockNotifications> Prepare(
        const RuntimeBlockBody&, uint32_t height, RuntimeBlockDirection) = 0;
};
} // namespace dinero
