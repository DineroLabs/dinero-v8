#pragma once
#include "wallet/orchard_operation_queue.h"
#include <chrono>
#include <memory>

namespace dinero::wallet {
// One executor per wallet service, shared by its accounts. This is an in-memory
// execution queue, not the durable transaction queue. The host commits Reserved
// and ordinary input locks BEFORE submitting, and must commit Ready before relay.
// Neither cancellation nor result collection releases any durable reservation.
class OrchardProofJobs {
public:
    enum class State { Queued, Running, CancelRequested, Succeeded, Failed, Cancelled };
    static constexpr size_t kMaxJobs = 4; // Includes retained terminal results.
    OrchardProofJobs(); // Initially idle, so the service can finish restoring.
    ~OrchardProofJobs();
    OrchardProofJobs(const OrchardProofJobs&) = delete;
    OrchardProofJobs& operator=(const OrchardProofJobs&) = delete;
    void Start(); // Once. No per-request thread creation.
    // Consumes the plan even on rejection. Reservation must match this exact
    // randomized plan/context. Caller supplies a stable committed queue copy.
    void Submit(const orchard::Hash& operation_id, const OrchardOperationQueue&,
        orchard::WalletBundlePlan, orchard::SigningContext);
    [[nodiscard]] std::optional<State> Query(const orchard::Hash&) const;
    [[nodiscard]] std::optional<State> WaitForChange(const orchard::Hash&, State,
        std::chrono::milliseconds timeout) const;
    // Queued work is discarded; running proof computation is not preemptible.
    // Its result is discarded at completion. Completed results cannot cancel.
    [[nodiscard]] bool Cancel(const orchard::Hash&);
    [[nodiscard]] std::unique_ptr<orchard::ProvedWalletBundle> TakeResult(const orchard::Hash&);
    void Forget(const orchard::Hash&); // Failed/Cancelled only, frees its slot.
    // Reject new submissions, cancel queued/running jobs. RequestStop is
    // nonjoining; Shutdown/destruction join the worker after an active proof
    // returns. This is NOT a bounded daemon-shutdown/watchdog guarantee.
    void RequestStop();
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dinero::wallet
