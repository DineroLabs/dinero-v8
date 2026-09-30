#pragma once
#include "daemon/runtime_outbox_cursor.h"
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace dinero {
class ChainstateService;
class WalletService;

// Replays durable source work; wakeups contain no event or acknowledgment.
// This is the wallet/readmission consumer only. It is deliberately not a
// RuntimeBlockNotifications provider and is not installed by DaemonApp until
// the remaining configured consumers have their own delivery owners.
class RuntimeDeliveryWorker final {
    struct WakeState;
public:
    enum class WalletOutcome { Deferred, ExplicitlyAbsent, NoActiveWallet, NoLog, AppliedPrefix };
    struct Report {
        uint64_t slices = 0;
        bool running = false;
        bool source_deferred = true;
        WalletOutcome wallet = WalletOutcome::Deferred;
        std::optional<RuntimeOutboxCursor> wallet_head;
        std::optional<RuntimeOutboxCursor> reorg_head;
        RuntimeOutboxCursor after_intent;
        size_t intents = 0, admission_attempts = 0, present_after_attempt = 0;
        // Only checked EOF of this pass at reorg_head. Not a durable cursor,
        // admission guarantee, all-consumer acknowledgment or readiness.
        bool reorg_eof = false;
    };
    struct Limits {
        size_t intents_per_slice = 32;
        std::chrono::milliseconds retry_interval{1000};
    };
    explicit RuntimeDeliveryWorker(std::shared_ptr<ChainstateService>,
                                   std::shared_ptr<WalletService>);
    RuntimeDeliveryWorker(std::shared_ptr<ChainstateService>,
                          std::shared_ptr<WalletService>, Limits);
    ~RuntimeDeliveryWorker();
    RuntimeDeliveryWorker(const RuntimeDeliveryWorker&)=delete;
    RuntimeDeliveryWorker& operator=(const RuntimeDeliveryWorker&)=delete;
    // A handoff owns only this mailbox, never the worker/thread or chain source.
    // Releasing it under selected ownership cannot join a worker. After Stop or
    // destruction requests are ignored; startup always scans durable work again.
    class WakeHandle final {
    public:
        void RequestReplay() const noexcept;
        [[nodiscard]] bool Running() const noexcept;
    private:
        friend class RuntimeDeliveryWorker;
        explicit WakeHandle(std::shared_ptr<WakeState> state):state_(std::move(state)) {}
        std::shared_ptr<WakeState> state_;
    };
    [[nodiscard]] WakeHandle CaptureWakeHandle() const noexcept { return WakeHandle(state_); }
    // Start always schedules a scan from the beginning, including after Stop.
    // Call lifecycle methods outside wallet, chain and runtime ownership.
    void Start();
    void Stop();
    // Suitable for a preallocated post-commit handoff. No source read or work
    // runs here; repeated requests coalesce and never acknowledge delivery.
    void RequestReplay() noexcept;
    [[nodiscard]] Report Snapshot() const;
private:
    void Run() noexcept;
    bool Stopping() const;
    void RecoverWallet(Report&) const;
    bool ReconcileSlice(Report&) const;
    std::shared_ptr<ChainstateService> source_;
    // Null is an explicit absence supplied by the composition owner.
    std::shared_ptr<WalletService> wallet_;
    const Limits limits_;
    std::mutex lifecycle_;
    struct WakeState {
        mutable std::mutex mutex;
        std::condition_variable wake;
        bool stopping = true, requested = false;
        Report report;
    };
    const std::shared_ptr<WakeState> state_=std::make_shared<WakeState>();
    std::thread thread_;
};
} // namespace dinero
