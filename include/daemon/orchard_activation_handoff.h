#pragma once
#include <filesystem>
#include <memory>

namespace dinero {
class OrchardSnapshotStore;
class ChainDB;
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
    const ChainDB& OriginalForInspection()const;
    const ChainDB& CandidateForInspection()const;
private:
    friend class OrchardSnapshotStore;
    friend struct OrchardSnapshotStoreTestAccess;
    struct Data;std::unique_ptr<Data> data_;
    explicit PreparedOrchardActivationHandoff(std::unique_ptr<Data>);
};
} // namespace dinero
