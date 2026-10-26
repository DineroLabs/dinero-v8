#pragma once
#include "consensus/orchard_snapshot.h"
#include "consensus/header_chain.h"
#include "daemon/orchard_chainstate_write.h"
#include "storage/orchard_snapshot_base.h"
#include "daemon/orchard_activation_handoff.h"
#include "storage/orchard_handoff_names.h"
#include "storage/orchard_activation_origin.h"

namespace dinero {
class ChainstateService;
class OrchardBranchReplay;
class OrchardBoundaryReplay;
class ClosedCoreReopenIntent;
class OrchardCompactStartup;
struct RuntimeOutboxEvent;
namespace daemon {class DatadirGuard;}
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
    struct ActivationPublication {
        // The bound DB outlives its typed compact owner. The caller retains
        // the SAME root guard and mutex before admitting any readers.
        std::unique_ptr<ChainDB> database;
        std::unique_ptr<OrchardCompactChainstate> compact;
    };
    // Deterministic unpublished conversion DATA, also used to reconstruct the
    // exact parent before-image for completed-state audits. Creates its own
    // exclusive destination; never clears/reuses an existing engine or root.
    struct ActivationParentCopy {
        std::unique_ptr<ChainDB> database;
        storage::OrchardActivationOrigin origin;
    };
    // Opaque, freshly checked closed-source binding, never deserialized from
    // origin/SQL/sidecar bytes. Original and candidate tips remain distinct.
    struct ActivationSource;
    static std::unique_ptr<const ActivationSource> CheckActivationSource(const ChainDB&,
        const consensus::HeaderChainSelector&,const consensus::OrchardSnapshotSection&,
        const std::function<bool()>& cancelled);
    static ActivationParentCopy CopyActivationParent(const ChainDB&,const ChainWriteToken&,
        AnnotatedRecursiveMutex&,const consensus::HeaderChainSelector&,
        const consensus::OrchardSnapshotSection&,const uint256& source_digest,
        const std::filesystem::path& destination,const std::function<void()>& on_created,
        const std::function<void()>& before_choice,const std::function<bool()>& cancelled);
    static ActivationPublication OpenActivationHandoff(
        const daemon::DatadirGuard&,const ClosedCoreReopenIntent&,AnnotatedRecursiveMutex&,
        const ChainWriteToken&,const BlockStorage&,
        std::shared_ptr<const consensus::HeaderChainSelector> current_headers,
        std::unique_ptr<PreparedOrchardActivationHandoff>);
    static std::unique_ptr<CompletedOrchardActivationStartup> ReadCompletedActivationRoot(const daemon::DatadirGuard&);
    static std::shared_ptr<ChainDB> CompletedActivationDatabase(const CompletedOrchardActivationStartup&);
    static std::shared_ptr<consensus::HeaderChainSelector> CompletedActivationHeaders(
        const ChainDB&,consensus::HeaderStore&);
    static void CheckCompletedActivationRoot(const daemon::DatadirGuard&,const CompletedOrchardActivationStartup&);
    static std::unique_ptr<OrchardCompactStartup> OpenCompletedActivationRoot(
        const daemon::DatadirGuard&,AnnotatedRecursiveMutex&,const ChainWriteToken&,const BlockStorage&,
        std::shared_ptr<const consensus::HeaderChainSelector>,CompletedOrchardActivationStartup&);
    // Read-only state audit, NOT root selection, lifecycle or owner authority.
    // Replays ALL activation-to-tip bodies, compares complete active Orchard/
    // retirement namespaces and exact archives/indexes/catalog closure. Caller
    // still owes completed root/backup/cohort/before-image/current-header checks.
    static storage::catalog::State AuditActivationSuffix(AnnotatedRecursiveMutex&,const ChainDB&,
        const ChainWriteToken&,const BlockStorage&,
        std::shared_ptr<const consensus::HeaderChainSelector>,
        std::shared_ptr<const consensus::OrchardSnapshotSection>,const std::function<bool()>& cancelled={});
    // Revalidates the exact post-conversion delivery suffix, including every
    // connect/undo/reconnect and the first Orchard block. The retained original
    // fixes the coverage origin; its old marker is NOT promoted to execution.
    // DATA audit only, no root choice, writer or lifecycle authority.
    static storage::catalog::State AuditActivationEvents(AnnotatedRecursiveMutex&,const ChainDB& original,
        const ChainDB& selected,const BlockStorage&,
        std::shared_ptr<const consensus::HeaderChainSelector>,
        std::shared_ptr<const consensus::OrchardSnapshotSection>,const std::function<bool()>& cancelled={},
        const std::function<void(const RuntimeOutboxEvent&,const OrchardBlockCandidate&,
            const consensus::ValidatedOrchardBlock*)>& observed={});
    // Reconstruct the selected parent from the immutable original, then apply
    // only authenticated actual delivery events through the canonical writer
    // in private scratch. Every CF row must match; no mutable-prefix exemption.
    // The scratch owner never escapes and archive appends are forbidden.
    static storage::catalog::State AuditActivationStore(AnnotatedRecursiveMutex&,const ChainDB& original,
        const ChainDB& selected,const ChainWriteToken&,const BlockStorage&,const uint256& parent_rows,
        std::shared_ptr<const consensus::HeaderChainSelector>,
        std::shared_ptr<const consensus::OrchardSnapshotSection>,const std::function<bool()>& cancelled={});
    class CommitUncertain final:public std::runtime_error {
    public:CommitUncertain():std::runtime_error("v6 state commit outcome is uncertain; restart required"){}
    };
    // Closed old services only, under the SAME real root guard. Read-only
    // source holds the checked ChainDB LOCK throughout copying and audit.
    // Converts a new exclusive candidate only; no rename, service/owner/tip
    // publication, lifecycle rewrite or original-file cleanup occurs here.
    static std::unique_ptr<PreparedOrchardActivationHandoff> PrepareActivationHandoff(
        const daemon::DatadirGuard&,const ChainWriteToken&,
        std::shared_ptr<const consensus::HeaderChainSelector>,
        std::shared_ptr<const consensus::OrchardSnapshotSection>,
        const std::function<bool()>& cancelled={});
    static void CheckActivationHandoff(const daemon::DatadirGuard&,
        const PreparedOrchardActivationHandoff&);
    // Durable closed-store name selection only, never typed state authority.
    // Exact lifecycle receipt, same retained root, destroyed old App/readers.
    // An immutable intent precedes no-replace renames. Original is retained;
    // cancellation after intent leaves explicitly recoverable closed work.
    static std::unique_ptr<PreparedOrchardActivationHandoff> SelectActivationHandoff(
        const daemon::DatadirGuard&,const ClosedCoreReopenIntent&,
        std::unique_ptr<PreparedOrchardActivationHandoff>,const std::function<bool()>& cancelled={});
    static std::unique_ptr<PreparedOrchardActivationHandoff> RecoverActivationHandoff(
        const daemon::DatadirGuard&,const ClosedCoreReopenIntent&,
        std::shared_ptr<const consensus::HeaderChainSelector>,const std::function<bool()>& cancelled={});
    // A null recovery selector uses only the retained, freshly revalidated
    // as-of-intent header closure to finish that explicit filesystem choice.
    // It never establishes current best-work selection or live state authority;
    // startup must merge/recheck current header branches before owner enrollment.
    // Until authenticated selected-origin startup is wired, generic startup
    // must refuse even a missing live name instead of creating a new store.
    static void RequireNoActivationHandoff(const std::filesystem::path& root){
        if(!storage::OrchardHandoffControlsAbsent(root/"blockchain"))
            throw std::runtime_error("Authenticated activation handoff requires dedicated closed-store recovery");
    }
    static int OpenHandoffRoot(const daemon::DatadirGuard&);
    static uint256 HandoffStoreDigest(const ChainDB&,const std::function<bool()>&);
    // Header DATA closure, not compact/state/replay authority. Persist only
    // independently checked raw header rows through exact configured burial;
    // never add active heights, body/undo locators or CHAIN/SCRIPTS flags.
    static void RetainActivationHeaders(ChainDB&,const ChainWriteToken&,
        const consensus::HeaderChainSelector&,const uint256& first,const std::function<bool()>&);
    static std::shared_ptr<consensus::HeaderChainSelector> ReadActivationHeaders(
        const ChainDB&,const uint256& first,const std::function<bool()>&);
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
    static std::unique_ptr<OrchardBoundaryReplay> ReplayActivation(
        std::shared_ptr<const consensus::OrchardSnapshotSection>,
        std::shared_ptr<const consensus::HeaderChainSelector>,uint32_t,const uint256&);
    // Private durable DATA cache, not a state/lifecycle/owner certificate.
    // The caller pins the real datadir/DB lifetime and selected writer mutex.
    // Bounded synced immutable chunks precede an atomic immutable descriptor
    // keyed by the exact activation block. No "latest"/active proof pointer,
    // state, catalog binding, replay flag or snapshot floor is installed.
    // Reads require the explicitly requested locally selected/buried block and
    // rerun the complete DNORST02 verifier. Incomplete chunks are never read
    // as a completed proof; conflicting/corrupt retained bytes are not repaired.
    static void RetainActivationProof(AnnotatedRecursiveMutex&,ChainDB&,const ChainWriteToken&,
        const consensus::HeaderChainSelector&,const consensus::OrchardSnapshotSection&,
        const std::function<bool()>& cancelled={});
    static std::optional<consensus::OrchardSnapshotSection> ReadActivationProof(
        AnnotatedRecursiveMutex&,const ChainDB&,const consensus::HeaderChainSelector&,
        const uint256& activation_block,const std::function<bool()>& cancelled={});
    // Startup-only re-enrollment at activation MINUS ONE, NOT a v6 snapshot
    // floor. Requires a locally selected/buried joint-activation proof, exact
    // retained parent catalog/nodes/stump and full frozen legacy before-image.
    // Source must ALREADY be compact, root-locked and quiescent; this method
    // does not convert full/assumed storage, install proof bytes, repair rows,
    // mark pre-parent history validated, or publish any service/tip/readers.
    // The ordinary first connect/undo/runtime writer remains mandatory.
    static std::unique_ptr<OrchardCompactChainstate> RestoreActivationParent(
        AnnotatedRecursiveMutex&,const ChainDB&,const consensus::HeaderChainSelector&,
        const consensus::OrchardSnapshotSection&);
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
