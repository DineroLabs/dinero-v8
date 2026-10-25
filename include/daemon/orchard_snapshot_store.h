#pragma once
#include "consensus/orchard_snapshot.h"
#include "consensus/header_chain.h"
#include "daemon/orchard_chainstate_write.h"
#include "storage/orchard_snapshot_base.h"

namespace dinero {
class ChainstateService;
class OrchardBranchReplay;
// Private selected-state authority. Decoding, raw catalog staging and a local
// checksum cannot enroll a compact owner. Only the service may invoke this
// installer while holding its real activation mutex and datadir ownership.
class OrchardSnapshotStore final {
public:
    // Read-only proof check. A successful return grants no publication, replay
    // or compact-owner capability; installation still rechecks selected state.
    static void VerifyCatalogTransferProof(const consensus::OrchardSnapshotSection&,
        const consensus::HeaderChainSelector&);
private:
    friend class ChainstateService;
    friend class DaemonApp;
    friend struct OrchardSnapshotStoreTestAccess;
    struct Publication {
        std::unique_ptr<consensus::ConsensusUTXOSet> full;
        std::unique_ptr<OrchardCompactChainstate> compact;
    };
    class CommitUncertain final:public std::runtime_error {
    public:CommitUncertain():std::runtime_error("v6 state commit outcome is uncertain; restart required"){}
    };
    // New directories only, under the caller's datadir guard. No existing
    // database is opened/migrated/replaced and no chainstate is enrolled.
    static void CreateFreshStore(const std::filesystem::path&);
    static void SeedRetainedHeaders(const ChainDB&,consensus::HeaderChainSelector&);
    static consensus::OrchardSnapshotSection Capture(const ChainDB&,
        consensus::OrchardSnapshotProfile,const std::vector<uint8_t>& accumulator);
    // Detached, read-only reconstruction of the FIRST Orchard block, even
    // after the selected tip has advanced. Requires retained bodies/catalog
    // nodes and the locally selected buried activation header. Never rolls
    // back the source DB, copies its current sets, or installs a reorg floor.
    // Caller pins DB lifetime and serializes preparation; cancellation is
    // checked on each source-node read and between bounded proof phases.
    static consensus::OrchardSnapshotSection CaptureActivation(const ChainDB&,
        const consensus::HeaderChainSelector&,const std::function<bool()>& cancelled);
    static consensus::OrchardBlockContext Authenticate(const consensus::OrchardSnapshotSection&,
        const consensus::HeaderChainSelector&,uint32_t burial,bool require_best_ancestry=true);
    static Publication Install(AnnotatedRecursiveMutex&,ChainDB&,const ChainWriteToken&,
        BlockStorage&,const consensus::HeaderChainSelector&,const consensus::OrchardSnapshotFile&);
    static consensus::OrchardSnapshotSection ReadBaseline(const ChainDB&);
    static std::unique_ptr<OrchardBranchReplay> ReplayFromBaseline(const ChainDB&,
        const consensus::HeaderChainSelector&,uint32_t,const uint256&,const arith_uint256&);
    static bool RecoverBaseMarkers(AnnotatedRecursiveMutex&,ChainDB&,const ChainWriteToken&,
        const consensus::HeaderChainSelector&);
    // Re-authenticates retained base and selected state. Does not assert any
    // pre-base transaction history was executed. The suffix audit is separate.
    static Publication Restore(AnnotatedRecursiveMutex&,const ChainDB&,
        const consensus::HeaderChainSelector&);
};
} // namespace dinero
