#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dinero {
class CoreReopenReceipt;class DaemonApp;class MempoolService;class ReopenServiceFence;
struct ReopenIntentTestAccess;
namespace daemon {
class DatadirGuard;
// Durable PUBLIC wire candidates, never cached admission/fee/ownership flags.
// Capture requires the actual drained pool fence and held operator-root lock.
// Files, including interrupted prefixes, are retained, never silently replaced.
// The host pauses RPC before fencing, and must not commit Stop before Capture
// succeeds. Wallet private intents remain in their original encrypted storage.
class ReopenCandidateJournal final {
public:
    struct Candidate {std::vector<uint8_t> body,proof;};
    static constexpr uint64_t MaximumBytes=uint64_t(1)<<30;
    static constexpr uint64_t MaximumCandidates=100000;
    static constexpr uint64_t MaximumWireBytes=uint64_t(32)<<20;
    static std::unique_ptr<ReopenCandidateJournal> Capture(const DatadirGuard&,
        const std::shared_ptr<MempoolService>&,const ReopenServiceFence&,
        const std::function<bool()>& cancelled={});
    static std::unique_ptr<ReopenCandidateJournal> Open(const DatadirGuard&,const std::string& name);
    // Only a durably selected lifecycle intent is eligible for automatic
    // readmission. Ordinary abandoned captures are NOT pending work. Missing
    // means neither control name exists; malformed/ambiguous controls throw.
    // This opens public candidates only, never wallet or chainstate authority.
    static std::unique_ptr<ReopenCandidateJournal> OpenPending(const DatadirGuard&,std::string& wallet_name);
    ~ReopenCandidateJournal();
    ReopenCandidateJournal(const ReopenCandidateJournal&)=delete;
    ReopenCandidateJournal& operator=(const ReopenCandidateJournal&)=delete;
    const std::string& Name()const;
    uint64_t Count()const;
    // Full digest/structure audit BEFORE any visitor, then bounded row-by-row
    // replay. Visitor must use ordinary canonical admission/relay; retain this
    // journal for unresolved/dependent/stale-proof candidates. No accepted
    // state or trusted input metadata is supplied. Thread-affine, nonrecursive.
    void ForEach(const DatadirGuard&,const std::function<void(const Candidate&)>&)const;
private:
    friend class ::dinero::DaemonApp;
    friend class ::dinero::CoreReopenReceipt;
    friend struct ::dinero::ReopenIntentTestAccess;
    struct Data;std::unique_ptr<Data> data_;
    explicit ReopenCandidateJournal(std::unique_ptr<Data>);
    static int OpenRoot(const DatadirGuard&);
    // Only a genuine current Capture may select an intent. The callback
    // irreversibly latches RPC CLOSED before no-replace publication starts;
    // uncertain writes retain both control/evidence names and fail closed.
    void SelectForReopen(const DatadirGuard&,const std::string&,const std::function<void()>& before_publish);
    void CheckPending(const DatadirGuard&,const std::string&)const;
    void ArchivePending(const DatadirGuard&,const std::string&);
};
} // namespace daemon
} // namespace dinero
