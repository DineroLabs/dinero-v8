#pragma once
#include "consensus/orchard_block_staging.h"
#include <memory>

namespace dinero {
class AnnotatedRecursiveMutex;
class CBlockIndex;
namespace consensus { class ConsensusUTXOSet; }

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
        const std::optional<storage::LegacyRetirementRecord>& authenticated_boundary = std::nullopt);
    [[nodiscard]] static std::unique_ptr<PreparedOrchardChainstateWrite> Disconnect(
        AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&,
        consensus::ConsensusUTXOSet&, const consensus::OrchardBlockContext&,
        const OrchardBlockCandidate&, const BlockHeader& parent,
        const consensus::UtreexoForest&, bool require_witness);

    // Indexed service variants: fsync exact body/undo first, stage their checked
    // locators in the same private batch, and publish the index's availability
    // fields only after durability. The CBlockIndex must outlive the owner.
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
        bool contextual_header_validated = false);
    [[nodiscard]] static std::unique_ptr<PreparedOrchardChainstateWrite> DisconnectIndexed(
        AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&, BlockStorage&,
        CBlockIndex&, consensus::ConsensusUTXOSet&, const consensus::OrchardBlockContext&,
        const OrchardBlockCandidate&, const BlockHeader& parent,
        const consensus::UtreexoForest&, bool require_witness);

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
    struct Impl;
    PreparedOrchardChainstateWrite(AnnotatedRecursiveMutex&, ChainDB&, const ChainWriteToken&);
    std::unique_ptr<Impl> impl_;
};
} // namespace dinero
