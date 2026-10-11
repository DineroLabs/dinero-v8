#pragma once
#include "consensus/orchard_block_staging.h"
#include "daemon/orchard_pool_coin_view.h"
#include <memory>
#include <variant>
#include <functional>
#include "storage/historical_catalog_state.h"

namespace dinero {
class AnnotatedRecursiveMutex;
class CBlockIndex;
class PreparedOrchardCatalog;
class PreparedHistoricalCatalog;
class PreparedHistoricalCatalogRange;
namespace consensus { class ConsensusUTXOSet; }

// Selected root-only state. An empty object has no authority. Only the
// canonical writer can enroll it from completed independent parent replay and
// publish successors after the shared durable commit. It deliberately does
// not implement arbitrary coin lookup or full-forest access. Fresh startup
// enrollment requires OrchardReindexOwner's complete independent reconstruction.
// The bound writer mutex and database must outlive this object. Access is
// exclusively under that mutex; the service must not replace a live owner.
class OrchardCompactChainstate final {
public:
    OrchardCompactChainstate() = default;
    OrchardCompactChainstate(const OrchardCompactChainstate&) = delete;
    OrchardCompactChainstate& operator=(const OrchardCompactChainstate&) = delete;
    // Pointer remains valid only while the caller holds the bound mutex.
    // Null means unenrolled, not an empty chain or absent coins.
    const storage::catalog::State* SelectedUnderLock(AnnotatedRecursiveMutex&) const;
    const storage::catalog::HistoricalState* HistoricalUnderLock(AnnotatedRecursiveMutex&) const;
    // Same enrolled owner and pointer lifetime as SelectedUnderLock; this is
    // the independently derived record even at the activation parent, where
    // there is deliberately no durable active retirement marker yet.
    const storage::LegacyRetirementRecord* RetirementUnderLock(AnnotatedRecursiveMutex&) const;
    // Requires this enrolled owner's bound mutex. Rechecks its actual durable
    // selection/profile/storage binding before copying proved transaction inputs.
    // Caller retains selected ownership through all later validation/publication.
    [[nodiscard]] OrchardPoolCoinView CapturePoolCoinsUnderLock(AnnotatedRecursiveMutex&,
        const consensus::OrchardTransactionContext&,const BlockHeader&,
        std::span<const MempoolProofView>) const;

private:
    friend class PreparedOrchardChainstateWrite;
    friend class OrchardReindexOwner;
    struct Selected {
        const ChainDB* database;
        AnnotatedRecursiveMutex* mutex;
        struct Orchard {storage::catalog::State catalog;storage::LegacyRetirementRecord retirement;};
        std::variant<Orchard,storage::catalog::HistoricalState> value;
        Selected(const ChainDB* db,AnnotatedRecursiveMutex* lock,storage::catalog::State state,
            storage::LegacyRetirementRecord retired):database(db),mutex(lock),value(Orchard{std::move(state),std::move(retired)}){}
        Selected(const ChainDB* db,AnnotatedRecursiveMutex* lock,storage::catalog::HistoricalState state)
            :database(db),mutex(lock),value(std::move(state)){}
        const Orchard& OrchardState() const {return std::get<Orchard>(value);}
    };
    std::shared_ptr<const Selected> selected_;
};

// Owns one stateful chainstate batch AND the activation lock, from preparation
// through synchronous durability and memory publication. No mutable batch or
// publication object escapes. Destruction before Commit abandons the write.
// Nonmovable and thread-affine; Commit is single-use. The lock remains held
// until destruction so the service can publish its other prepared tip metadata.
//
// This is NOT full-block admission. The service must supply its actual writer
// mutex, authenticated selected headers/history and complete header/PoW and
// service/flatfile/index obligations. All writers must use that mutex; no
// reentrant database/memory mutations are allowed during this object's lifetime.
// This owner does not wire ConnectTip, CSN, wallet SQLite or notifications.
class PreparedOrchardChainstateWrite final {
public:
    [[nodiscard]] static std::unique_ptr<PreparedOrchardChainstateWrite> Connect(
        AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&,
        consensus::ConsensusUTXOSet&, const consensus::OrchardBlockContext&,
        const OrchardBlockCandidate&, const BlockHeader& parent,
        const consensus::UtreexoForest&, const consensus::OrchardBranchMtpLookup&,
        bool require_witness, bool checkpoint,
        const std::optional<storage::LegacyRetirementRecord>& authenticated_boundary = std::nullopt,
        const consensus::ValidatedOrchardBlock* detached = nullptr,
        const PreparedOrchardCatalog* initial_catalog = nullptr, bool require_catalog = false);
    [[nodiscard]] static std::unique_ptr<PreparedOrchardChainstateWrite> Disconnect(
        AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&,
        consensus::ConsensusUTXOSet&, const consensus::OrchardBlockContext&,
        const OrchardBlockCandidate&, const BlockHeader& parent,
        const consensus::UtreexoForest&, bool require_witness, bool require_catalog = false);

    // Indexed service variants: fsync exact body/undo first, stage their checked
    // locators in the same private batch, and publish the index's availability
    // fields only after durability. An exact delivery record and sequence head
    // share that batch; rollback retains history. This is durable handoff data,
    // not an installed notification consumer. The CBlockIndex must outlive the owner.
    // Abandonment can leave unreferenced append
    // records, never a locator pointing to an uncommitted transition. These do
    // not publish the active tip. Only the service after its contextual header
    // check may request validity publication in the same batch.
    [[nodiscard]] static std::unique_ptr<PreparedOrchardChainstateWrite> ConnectIndexed(
        AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&, BlockStorage&,
        CBlockIndex&, consensus::ConsensusUTXOSet&, const consensus::OrchardBlockContext&,
        const OrchardBlockCandidate&, const BlockHeader& parent,
        const consensus::UtreexoForest&, const consensus::OrchardBranchMtpLookup&,
        bool require_witness, bool checkpoint,
        const std::optional<storage::LegacyRetirementRecord>& authenticated_boundary = std::nullopt,
        bool contextual_header_validated = false,
        const consensus::ValidatedOrchardBlock* detached = nullptr,
        const PreparedOrchardCatalog* initial_catalog = nullptr, bool require_catalog = false);
    [[nodiscard]] static std::unique_ptr<PreparedOrchardChainstateWrite> DisconnectIndexed(
        AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&, BlockStorage&,
        CBlockIndex&, consensus::ConsensusUTXOSet&, const consensus::OrchardBlockContext&,
        const OrchardBlockCandidate&, const BlockHeader& parent,
        const consensus::UtreexoForest&, bool require_witness, bool require_catalog = false);

    // Root-only canonical connect. First use requires the private catalog from
    // genuine completed independent parent replay plus its retirement record.
    // Later use requires this exact live/durable selected owner. No full coin
    // or forest fallback. Body/index/undo/catalog/state/delivery share Commit.
    // This API does not yet wire production CSN startup, undo or consumers.
    [[nodiscard]] static std::unique_ptr<PreparedOrchardChainstateWrite> ConnectCompactIndexed(
        AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&, BlockStorage&,
        CBlockIndex&, OrchardCompactChainstate&, const consensus::OrchardBlockContext&,
        const OrchardBlockCandidate&, const BlockHeader& parent,
        const consensus::OrchardBranchMtpLookup&, bool require_witness,
        const std::optional<storage::LegacyRetirementRecord>& authenticated_boundary = std::nullopt,
        bool contextual_header_validated = false,
        const PreparedOrchardCatalog* initial_catalog = nullptr,
        const consensus::ValidatedOrchardBlock* detached = nullptr);

    [[nodiscard]] static std::unique_ptr<PreparedOrchardChainstateWrite> DisconnectCompactIndexed(
        AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&, BlockStorage&,
        CBlockIndex&, OrchardCompactChainstate&, const consensus::OrchardBlockContext&,
        const OrchardBlockCandidate&, const BlockHeader& parent,
        const consensus::OrchardBranchMtpLookup&, bool require_witness,
        const consensus::ValidatedOrchardBlock* detached = nullptr);

    // Both adjacent catalogs originate in completed independent replay. The
    // higher historical block is bound by its exact captured wire digest and
    // genuine validator undo. No guessed retirement or full-coin fallback.
    using HigherHistoricalCatalog=std::variant<std::reference_wrapper<const PreparedHistoricalCatalog>,
        std::reference_wrapper<const PreparedOrchardCatalog>>;
    [[nodiscard]] static std::unique_ptr<PreparedOrchardChainstateWrite> HistoricalCompactIndexed(
        AnnotatedRecursiveMutex&,ChainDB&,const ChainWriteToken&,BlockStorage&,
        CBlockIndex&,OrchardCompactChainstate&,const Block&,
        const PreparedHistoricalCatalog& lower,const HigherHistoricalCatalog& higher,bool connecting);

    // Adjacent intermediate historical checkpoints from one completed range;
    // an independently replayed activation parent is required only at its edge.
    // No caller-populated catalog, undo, stump or retirement is accepted.
    [[nodiscard]] static std::unique_ptr<PreparedOrchardChainstateWrite> HistoricalCompactRangeIndexed(
        AnnotatedRecursiveMutex&,ChainDB&,const ChainWriteToken&,BlockStorage&,
        CBlockIndex&,OrchardCompactChainstate&,const Block&,
        const PreparedHistoricalCatalogRange&,const PreparedOrchardCatalog* boundary,bool connecting);

    PreparedOrchardChainstateWrite(const PreparedOrchardChainstateWrite&) = delete;
    PreparedOrchardChainstateWrite& operator=(const PreparedOrchardChainstateWrite&) = delete;
    PreparedOrchardChainstateWrite(PreparedOrchardChainstateWrite&&) = delete;
    PreparedOrchardChainstateWrite& operator=(PreparedOrchardChainstateWrite&&) = delete;
    ~PreparedOrchardChainstateWrite();

    // Before writing, a failed readiness check invalidates this object and
    // throws without a database write. Once writing starts, any failure is
    // fatal: restart must recover the authoritative store, never continue on
    // potentially divergent memory. Database and token must outlive this object.
    void Commit();
private:
    static std::unique_ptr<PreparedOrchardChainstateWrite> HistoricalCompactPrepared(
        AnnotatedRecursiveMutex&,ChainDB&,const ChainWriteToken&,BlockStorage&,
        CBlockIndex&,OrchardCompactChainstate&,const Block&,
        const PreparedHistoricalCatalog*,const PreparedHistoricalCatalogRange*,
        const HigherHistoricalCatalog*,bool connecting);
    struct Impl;
    PreparedOrchardChainstateWrite(AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&);
    std::unique_ptr<Impl> impl_;
};
} // namespace dinero
