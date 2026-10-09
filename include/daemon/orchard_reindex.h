#pragma once
#include "consensus/reindexer.h"
#include "primitives/block.h"
#include "daemon/runtime_block_outbox.h"
#include <optional>

namespace dinero {
class ChainWriteToken;
class AnnotatedRecursiveMutex;
class OrchardCompactChainstate;
class CBlockIndex;
// Created only after independent compact reconstruction and final source
// rechecks. Selected entries are values with no live graph pointers. This is
// startup handoff material, not permission to admit readers or publish a tip.
class OrchardCompactStartup final {
public:
    // Values are constructible; only this private result can bind them to a
    // completed reconstruction. No flags, undo pointers or live graph links.
    struct SelectedHeader {
        BlockHeader header;
        uint32_t height;
        arith_uint256 work;
        uint32_t file_number,data_pos,data_size;
    };
    // Original archive coordinates, retained only after the actual undo bytes
    // agree with independent replay. No raw metadata availability certificate.
    struct RetainedUndoLocation { uint32_t file_number,data_pos,data_size; };
    ~OrchardCompactStartup();
    OrchardCompactStartup(const OrchardCompactStartup&)=delete;
    OrchardCompactStartup& operator=(const OrchardCompactStartup&)=delete;
private:
    friend class OrchardReindexOwner;
    friend class ChainstateService;
    friend struct OrchardCompactStartupTestAccess;
    OrchardCompactStartup();
    // Consumes only this independently reconstructed result. Caller holds the
    // startup writer and datadir ownership; this never admits readers or a tip.
    std::vector<CBlockIndex*> PublishIndexUnderLock();
    ChainDB* source_=nullptr;
    AnnotatedRecursiveMutex* mutex_=nullptr;
    std::unique_ptr<OrchardCompactChainstate> owner_;
    std::vector<SelectedHeader> selected_headers_;
    // Aligned by selected height. Present pre-Orchard locations require exact
    // original/reconstructed flatfile byte equality; absence claims no undo.
    // This does not certify deletion, backup or historical undo completeness.
    std::vector<std::optional<RetainedUndoLocation>> selected_undo_;
    // Unpublished nodes derived from the verified selected history. Ownership
    // stays with this result until service startup takes the complete vector.
    // No global graph pointer or raw persisted status/undo field is imported.
    std::vector<std::unique_ptr<CBlockIndex>> selected_index_;
};
// Startup-only owner. The caller holds the datadir guard, keeps Params fixed,
// and has not started services. Source remains open and unmodified; candidate
// is unpublished and is promoted only by DaemonApp's existing cohort journal.
class OrchardReindexOwner final {
public:
    static StatusOr<consensus::BlockReindexer::Stats> Run(
        const ChainDB& source, ChainDB& candidate, const ChainWriteToken&,
        BlockStorage&, const std::filesystem::path& datadir,
        const std::filesystem::path& candidate_path,
        const consensus::BlockReindexer::Config&);
    // Startup only, while the caller owns the datadir and before services.
    // Reconstructs history into newly created, isolated scratch storage; never
    // repairs source rows/files. Returns a NEW owner bound to source/mutex only
    // after completed independent replay and all retained-state checks. Throws
    // on refusal. Neither a decoded catalog nor a successful Stats is authority.
    // source and mutex must outlive the returned owner. No other writer, Params
    // change, or callback may run during this operation. scratch_path must not
    // exist; only this newly created directory is removed on exit.
    static std::unique_ptr<OrchardCompactChainstate> RestoreCompact(
        AnnotatedRecursiveMutex&, ChainDB& source, const ChainWriteToken&,
        const BlockStorage& source_files, const std::filesystem::path& datadir,
        const std::filesystem::path& scratch_path);
private:
    friend class ChainstateService;
    friend struct OrchardCompactStartupTestAccess;
    static std::unique_ptr<OrchardCompactStartup> RestoreCompactPrepared(
        AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&,
        const BlockStorage&, const std::filesystem::path&,
        const std::filesystem::path&, bool retain_selected_headers);
    struct CompactReconstruction;
    static StatusOr<consensus::BlockReindexer::Stats> Reconstruct(
        const ChainDB&, ChainDB&, const ChainWriteToken&, BlockStorage&,
        const std::filesystem::path&, const std::filesystem::path&,
        const consensus::BlockReindexer::Config&, CompactReconstruction*);
    // Only Run may carry an independently replayed prefix into a replacement
    // historical candidate. These bytes preserve existing consumer identities;
    // copying them does not establish consensus validity or consumer readiness.
    static void CopyValidatedOutboxPrefix(const ChainDB& source, ChainDB& candidate,
        const ChainWriteToken&, const consensus::OrchardBlockContext&,
        RuntimeOutboxCursor through, RuntimeOutboxCursor source_head);
};
} // namespace dinero
