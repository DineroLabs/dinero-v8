#pragma once
#include "wallet/orchard_operation_queue.h"
#include <chrono>
#include <memory>

namespace dinero::wallet {
class OrchardAccountDelivery;
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
    // Preallocate the exact task and claim capacity before committing Reserved.
    // Unpublished submissions cannot run or appear in Query. Destruction frees
    // only this in-memory slot; it never cancels a durable wallet reservation.
    class Submission {
    public:
        ~Submission();
        Submission(const Submission&) = delete;
        Submission& operator=(const Submission&) = delete;
        const orchard::WalletProvingIntent& Intent() const noexcept;
        void Bind(const OrchardOperationQueue& staged);
        // Host calls only after its checked SQLite COMMIT. Nonallocating; false
        // on stop or an unbound ticket. Durable Reserved remains in that case.
        [[nodiscard]] bool Publish() noexcept;
    private:
        friend class OrchardProofJobs;
        struct Data;
        explicit Submission(std::unique_ptr<Data>);
        std::unique_ptr<Data> data_;
    };
    [[nodiscard]] std::unique_ptr<Submission> Prepare(const orchard::Hash&,
        orchard::WalletBundlePlan, orchard::SigningContext);
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
    // Copy a successful result without removing its job or freeing capacity.
    // The host must supply a freshly authenticated durable queue from the
    // selected wallet/account; the ID alone is not RPC authorization. Exact
    // intent binding prevents collecting another job that reused this ID.
    // A failed Ready commit can retry this copy. Only collect with TakeResult
    // after the host has durably retained the exact signed transaction.
    [[nodiscard]] std::unique_ptr<orchard::ProvedWalletBundle> CopyResult(
        const orchard::Hash&, const OrchardOperationQueue&) const;
    [[nodiscard]] std::unique_ptr<orchard::ProvedWalletBundle> TakeResult(const orchard::Hash&);
    void Forget(const orchard::Hash&); // Failed/Cancelled only, frees its slot.
    // Reject new submissions, cancel queued/running jobs. RequestStop is
    // nonjoining; Shutdown/destruction join the worker after an active proof
    // returns. This is NOT a bounded daemon-shutdown/watchdog guarantee.
    void RequestStop();
    void Shutdown();
private:
    friend class OrchardAccountDelivery;
    // Only the authenticated account owner constructs this host binding.
    // Keeping the token alive prevents allocator-address reuse masquerading as
    // the same manager; it does not own the manager or its database.
    struct Binding {
        orchard::WalletStorageIdentity identity;
        uint32_t branch;
        uint64_t session;
        std::shared_ptr<const void> instance;
    };
    struct Capture {
        std::optional<State> state;
        std::unique_ptr<orchard::ProvedWalletBundle> proof;
    };
    [[nodiscard]] std::unique_ptr<Submission> PrepareOwned(const orchard::Hash&,
        const Binding&,orchard::WalletBundlePlan,orchard::SigningContext);
    [[nodiscard]] Capture CaptureOwned(const orchard::Hash&,const Binding&,
        const OrchardOperationQueue&) const;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
} // namespace dinero::wallet
