#include "wallet/orchard_proof_jobs.h"
#include <condition_variable>
#include <algorithm>
#include <list>
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
        bool unpublished = false;
    };
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    std::mutex join_mutex;
    std::map<Hash, Job> jobs;
    std::list<Hash> queued, prepared;
    bool started = false, stopping = false;
    std::thread worker;

    std::optional<State> Find(const Hash& id) const {
        const auto it = jobs.find(id);
        return it == jobs.end() || it->second.unpublished ? std::nullopt : std::optional<State>(it->second.state);
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
struct OrchardProofJobs::Submission::Data {
    std::shared_ptr<Impl> owner;
    Hash id;
    WalletProvingIntent intent;
    bool owns_slot = false, bound = false;
    Data(std::shared_ptr<Impl> parent, const Hash& operation, WalletProvingIntent value)
        : owner(std::move(parent)), id(operation), intent(std::move(value)) {}
    Data(const Data&) = delete;
    Data& operator=(const Data&) = delete;
    ~Data() {
        if (!owns_slot) return;
        std::unique_ptr<Impl::Task> discard;
        {
            std::lock_guard lock(owner->mutex);
            const auto found = owner->jobs.find(id);
            if (found != owner->jobs.end() && found->second.unpublished) {
                discard = std::move(found->second.task);
                owner->prepared.remove(id);
                owner->jobs.erase(found);
                owner->changed.notify_all();
            }
        }
    }
};
OrchardProofJobs::Submission::Submission(std::unique_ptr<Data> data) : data_(std::move(data)) {}
OrchardProofJobs::Submission::~Submission() = default;
const WalletProvingIntent& OrchardProofJobs::Submission::Intent() const noexcept { return data_->intent; }
void OrchardProofJobs::Submission::Bind(const OrchardOperationQueue& staged) {
    data_->bound = false;
    const auto found = staged.Entries().find(data_->id);
    if (!data_->owns_slot || found == staged.Entries().end() ||
        found->second.phase != OrchardOperationQueue::Phase::Reserved ||
        found->second.message != data_->intent.Message() ||
        found->second.nullifiers != data_->intent.Nullifiers() ||
        !SameInputs(found->second.inputs, data_->intent.Inputs())) Reject();
    data_->bound = true;
}
bool OrchardProofJobs::Submission::Publish() noexcept {
    if (!data_->owns_slot || !data_->bound) return false;
    try {
        std::lock_guard lock(data_->owner->mutex);
        auto& owner = *data_->owner;
        const auto found = owner.jobs.find(data_->id);
        if (owner.stopping || found == owner.jobs.end() || !found->second.unpublished) return false;
        const auto node = std::find(owner.prepared.begin(), owner.prepared.end(), data_->id);
        if (node == owner.prepared.end()) return false;
        // Both lists use the same allocator. Splice changes links only: the
        // plan, const-vector SigningContext, map and queue node already exist.
        owner.queued.splice(owner.queued.end(), owner.prepared, node);
        found->second.unpublished = false;
        data_->owns_slot = false;
        owner.changed.notify_all();
        return true;
    } catch (...) { return false; }
}
OrchardProofJobs::OrchardProofJobs() : impl_(std::make_shared<Impl>()) {}
std::unique_ptr<OrchardProofJobs::Submission> OrchardProofJobs::Prepare(
        const Hash& id, WalletBundlePlan plan, SigningContext context) {
    if (id == Hash{}) Reject();
    auto data = std::make_unique<Submission::Data>(impl_, id, plan.Intent(context));
    auto task = std::make_unique<Impl::Task>(Impl::Task{std::move(plan), std::move(context)});
    auto result = std::unique_ptr<Submission>(new Submission(std::move(data)));
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->jobs.size() >= kMaxJobs || impl_->jobs.contains(id)) Reject();
    impl_->prepared.push_back(id);
    try { impl_->jobs.emplace(id, Impl::Job{State::Queued, std::move(task), {}, true}); }
    catch (...) { impl_->prepared.pop_back(); throw; }
    result->data_->owns_slot = true;
    return result;
}
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
        if (it == impl_->jobs.end() || it->second.unpublished) return false;
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
std::unique_ptr<ProvedWalletBundle> OrchardProofJobs::CopyResult(
    const Hash& id, const OrchardOperationQueue& authenticated) const {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->jobs.find(id);
    const auto owner = authenticated.Entries().find(id);
    if (it == impl_->jobs.end() || it->second.state != State::Succeeded ||
        !it->second.result || owner == authenticated.Entries().end()) Reject();
    const auto& result = *it->second.result;
    const auto& entry = owner->second;
    const auto& facts = result.Authorization().Facts();
    if (entry.message != result.Authorization().SigningDigest() ||
        entry.nullifiers.size() != facts.action_count) Reject();
    for (size_t i = 0; i < entry.nullifiers.size(); ++i)
        if (!std::equal(entry.nullifiers[i].begin(), entry.nullifiers[i].end(),
                facts.nullifiers[i])) Reject();
    // Allocation/copy failure leaves the original result, state and capacity
    // unchanged. There are no wallet/SQLite callbacks under the executor lock.
    return std::make_unique<ProvedWalletBundle>(result);
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
            if (job.unpublished) continue;
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
