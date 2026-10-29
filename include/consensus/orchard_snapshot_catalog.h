#pragma once
#include "storage/orchard_catalog_state.h"
#include <functional>
#include <filesystem>
#include <memory>

namespace dinero::consensus {
// Disposable, process-local, MAC-checked disk inventory. It is never a selected
// chainstate, cannot be reopened by filename and grants no consensus authority.
// Once sealed, copies share immutable records; mutable section metadata is
// checked separately. SQLite owns/removes its temporary file on final close.
class OrchardSnapshotCatalog final {
public:
    explicit OrchardSnapshotCatalog(const std::filesystem::path& work_directory={});
    ~OrchardSnapshotCatalog();
    OrchardSnapshotCatalog(const OrchardSnapshotCatalog&)=delete;
    OrchardSnapshotCatalog& operator=(const OrchardSnapshotCatalog&)=delete;
    // Duplicate capture paths may share a node only with byte-identical data.
    void Insert(const uint256&,const std::string&);
    void InsertUnique(const uint256&,const std::string&);
    void Seal();
    uint64_t Size()const;
    uint64_t WireBytes()const;
    const std::filesystem::path& WorkDirectory()const;
    std::optional<std::string> Find(const uint256&)const;
    std::string Read(const uint256&)const;
    void ForEach(const std::function<void(const uint256&,const std::string&)>&)const;
    void ValidateReachability(const storage::catalog::State&,const storage::catalog::State&)const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dinero::consensus
