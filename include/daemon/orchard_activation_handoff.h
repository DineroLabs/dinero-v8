#pragma once
#include <filesystem>
#include <memory>

namespace dinero {
class OrchardSnapshotStore;
class ChainDB;
class ChainstateService;
// Normal completed-root startup, distinct from a pending PUBLIC-candidate
// lifecycle receipt. Holds only existing read-only stores until the service
// supplies its real mutex, archives and freshly validated current headers.
class CompletedOrchardActivationStartup final {
public:
    ~CompletedOrchardActivationStartup();
    CompletedOrchardActivationStartup(const CompletedOrchardActivationStartup&)=delete;
    CompletedOrchardActivationStartup& operator=(const CompletedOrchardActivationStartup&)=delete;
private:
    friend class OrchardSnapshotStore;friend class ChainstateService;
    friend struct OrchardSnapshotStoreTestAccess;
    struct Data;std::unique_ptr<Data> data_;
    explicit CompletedOrchardActivationStartup(std::unique_ptr<Data>);
};
// An unpublished, exclusively owned conversion candidate. It is NOT an owner,
// a snapshot floor, completed replay or permission to start services. Original
// ChainDB/cohort and wallet files stay selected and unchanged. Destruction
// releases the checked DB locks, retaining the candidate for investigation;
// no directory scan/adoption, promotion or cleanup happens implicitly.
class PreparedOrchardActivationHandoff final {
public:
    ~PreparedOrchardActivationHandoff();
    PreparedOrchardActivationHandoff(const PreparedOrchardActivationHandoff&)=delete;
    PreparedOrchardActivationHandoff& operator=(const PreparedOrchardActivationHandoff&)=delete;
    const std::filesystem::path& CandidatePath()const;
    const std::filesystem::path& OriginalPath()const;
    const ChainDB& OriginalForInspection()const;
    const ChainDB& CandidateForInspection()const;
private:
    friend class OrchardSnapshotStore;
    friend struct OrchardSnapshotStoreTestAccess;
    struct Data;std::unique_ptr<Data> data_;
    explicit PreparedOrchardActivationHandoff(std::unique_ptr<Data>);
};
} // namespace dinero
