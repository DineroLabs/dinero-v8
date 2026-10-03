#include "wallet/orchard_proof_jobs.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace dinero::wallet {
using namespace orchard;
namespace {
[[noreturn]] void Reject() { throw std::runtime_error("Orchard proof job rejected"); }
bool SameInputs(const std::vector<ResolvedInput>& a, const std::vector<ResolvedInput>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].txid_wire != b[i].txid_wire || a[i].output_index != b[i].output_index ||
            a[i].sequence != b[i].sequence || a[i].amount_una != b[i].amount_una ||
            a[i].script_pub_key != b[i].script_pub_key) return false;
    return true;
}
}
struct OrchardProofJobs::Impl {
    struct Task {
        WalletBundlePlan plan;
        SigningContext context;
    };
    struct Job {
        State state = State::Queued;
        std::unique_ptr<Task> task;
        std::unique_ptr<ProvedWalletBundle> result;
    };
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    std::mutex join_mutex;
    std::map<Hash, Job> jobs;
    std::deque<Hash> queued;
    bool started = false, stopping = false;
    std::thread worker;

    std::optional<State> Find(const Hash& id) const {
        const auto it = jobs.find(id);
        return it == jobs.end() ? std::nullopt : std::optional<State>(it->second.state);
    }
    void Run() {
        for (;;) {
            Hash id;
            std::unique_ptr<Task> task;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || !queued.empty(); });
                if (stopping) return;
                id = queued.front(); queued.pop_front();
                auto& job = jobs.at(id);
                task = std::move(job.task);
                job.state = State::Running;
                changed.notify_all();
            }
            std::unique_ptr<ProvedWalletBundle> result;
            try {
                // No wallet, queue, SQLite or chainstate lock held while proving.
                // The Rust backend independently limits the prover to two workers.
                result = std::make_unique<ProvedWalletBundle>(std::move(task->plan).Prove(task->context));
            } catch (...) {
                // Report a generic failure; never expose exception text that
                // could contain private wallet data through a future status RPC.
            }
            task.reset();
            {
                std::lock_guard lock(mutex);
                auto& job = jobs.at(id); // Running entries cannot be removed.
                if (job.state == State::CancelRequested || stopping) job.state = State::Cancelled;
                else if (result) { job.result = std::move(result); job.state = State::Succeeded; }
                else job.state = State::Failed;
                changed.notify_all();
            }
            // A cancelled proof result is destroyed outside the queue lock.
        }
    }
};
OrchardProofJobs::OrchardProofJobs() : impl_(std::make_unique<Impl>()) {}
OrchardProofJobs::~OrchardProofJobs() { Shutdown(); }
void OrchardProofJobs::Start() {
    std::lock_guard join_lock(impl_->join_mutex);
    std::lock_guard lock(impl_->mutex);
    if (impl_->started || impl_->stopping) Reject();
    impl_->worker = std::thread([p = impl_.get()] { p->Run(); });
    impl_->started = true;
}
void OrchardProofJobs::Submit(const Hash& id, const OrchardOperationQueue& reserved,
    WalletBundlePlan plan, SigningContext context) {
    const auto intent = plan.Intent(context);
    const auto it = reserved.Entries().find(id);
    if (id == Hash{} || it == reserved.Entries().end() ||
        it->second.phase != OrchardOperationQueue::Phase::Reserved ||
        it->second.message != intent.Message() || it->second.nullifiers != intent.Nullifiers() ||
        !SameInputs(it->second.inputs, intent.Inputs())) Reject();
    auto task = std::make_unique<Impl::Task>(Impl::Task{std::move(plan), std::move(context)});
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->jobs.size() >= kMaxJobs || impl_->jobs.contains(id)) Reject();
    // Reserve both containers before publishing; allocation failure cannot
    // strand an unqueued entry or consume a visible capacity slot.
    impl_->queued.push_back(id);
    try { impl_->jobs.emplace(id, Impl::Job{State::Queued, std::move(task), {}}); }
    catch (...) { impl_->queued.pop_back(); throw; }
    impl_->changed.notify_all();
}
std::optional<OrchardProofJobs::State> OrchardProofJobs::Query(const Hash& id) const {
    std::lock_guard lock(impl_->mutex); return impl_->Find(id);
}
std::optional<OrchardProofJobs::State> OrchardProofJobs::WaitForChange(
    const Hash& id, State previous, std::chrono::milliseconds timeout) const {
    if (timeout < std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(30)) Reject();
    std::unique_lock lock(impl_->mutex);
    impl_->changed.wait_for(lock, timeout, [&] { return impl_->Find(id) != previous; });
    return impl_->Find(id);
}
bool OrchardProofJobs::Cancel(const Hash& id) {
    std::unique_ptr<Impl::Task> discard;
    {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->jobs.find(id);
        if (it == impl_->jobs.end()) return false;
        auto& job = it->second;
        if (job.state == State::Queued) {
            std::erase(impl_->queued, id);
            discard = std::move(job.task); job.state = State::Cancelled;
        } else if (job.state == State::Running) job.state = State::CancelRequested;
        else if (job.state != State::CancelRequested && job.state != State::Cancelled) return false;
        impl_->changed.notify_all();
    }
    return true;
}
std::unique_ptr<ProvedWalletBundle> OrchardProofJobs::TakeResult(const Hash& id) {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->jobs.find(id);
    if (it == impl_->jobs.end() || it->second.state != State::Succeeded) Reject();
    auto result = std::move(it->second.result); impl_->jobs.erase(it);
    impl_->changed.notify_all(); return result;
}
void OrchardProofJobs::Forget(const Hash& id) {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->jobs.find(id);
    if (it == impl_->jobs.end() ||
        (it->second.state != State::Failed && it->second.state != State::Cancelled)) Reject();
    impl_->jobs.erase(it); impl_->changed.notify_all();
}
void OrchardProofJobs::RequestStop() {
    std::array<std::unique_ptr<Impl::Task>, kMaxJobs> discard;
    size_t discarded = 0;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        impl_->queued.clear();
        for (auto& [id, job] : impl_->jobs) {
            if (job.state == State::Queued) {
                discard[discarded++] = std::move(job.task); job.state = State::Cancelled;
            } else if (job.state == State::Running) job.state = State::CancelRequested;
        }
        impl_->changed.notify_all();
    }
}
void OrchardProofJobs::Shutdown() {
    std::lock_guard join_lock(impl_->join_mutex);
    RequestStop();
    if (impl_->worker.joinable()) impl_->worker.join();
}
} // namespace dinero::wallet
