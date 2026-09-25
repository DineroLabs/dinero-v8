#pragma once
#include "consensus/orchard_state_transition.h"
#include "primitives/orchard_block_reader.h"
#include "consensus/orchard_block_coins.h"
#include "consensus/orchard_forest_transition.h"
#include "storage/legacy_retirement.h"

namespace rocksdb { class WriteBatch; }
namespace dinero { class ChainDB; class ChainWriteToken; class BlockStorage; }

namespace dinero::consensus {
// Typed stored-body read. Checks exact framing, key/header identity, transaction
// and witness commitments, and size. Not replay/PoW/proof/transaction validity.
[[nodiscard]] OrchardBlockCandidate ReadStoredOrchardBlock(
    const ChainDB&, const uint256& hash, bool require_witness_commitment,
    const BlockStorage* archival_blocks = nullptr);
// Startup consistency check under the writer lock, after restoring the forest
// against authenticated selected headers. Validates the exact local body,
// markers, commit record, frontier and reversibility of current-tip rows/undo.
// Never commits, repairs, rewinds a pointer alone, or establishes historical
// consensus validity. Does not audit every older UTXO/nullifier in the database.
void AuditOrchardChainstateTipUnderLock(ChainDB&, const ChainWriteToken&,
    const OrchardBlockContext&, const BlockHeader& parent, const UtreexoForest&,
    bool require_witness_commitment);
// Caller holds the chainstate writer lock from coin resolution/authorization
// through commit. All supplied authorizations must come from that held view.
// This stages ONLY Orchard state in the caller's batch. UTXO/forest/tip/index
// and ordinary undo must be staged in that same batch by the block connector.
// Neither success nor a ChainWriteToken proves full-block validity or locking.
// Local storage failures (including an inconsistent persisted parent) use
// OrchardStateLookupError, never a peer consensus-invalid classification.
[[nodiscard]] PreparedOrchardState StageOrchardBlockUnderChainstateLock(
    ChainDB&, const ChainWriteToken&, const OrchardBlockContext&,
    const OrchardBlockCandidate&, bool require_witness_commitment,
    std::span<const VerifiedOrchardAuthorizations>, rocksdb::WriteBatch&);

struct StagedOrchardBlock {
    PreparedOrchardBlockCoins coins;
    PreparedOrchardState orchard;
};
// Stateful ChainDB adapter. Resolve all coins from this same selected database,
// verify both transaction families, and stage net UTXO changes, coin undo and
// Orchard state in one batch. Requires an EMPTY batch on entry; on failure it
// remains empty. Does not commit. Caller must validate the remaining block
// obligations and add forest, tip/index and forest-undo records to this batch,
// commit once, then publish memory. Not a stateless/CSN authentication adapter.
[[nodiscard]] StagedOrchardBlock StageOrchardBlockCoinsAndStateUnderChainstateLock(
    ChainDB&, const ChainWriteToken&, const OrchardBlockContext&,
    const OrchardBlockCandidate&, const OrchardBranchMtpLookup&,
    bool require_witness_commitment, rocksdb::WriteBatch&);

// Reverse the exact net coin changes and Orchard undo for the selected tip.
// Authenticate the supplied body against that tip, check conventional undo
// coverage/current coins, and leave retained conventional undo for reconnect.
// Same empty-batch/lock/outer-commit contract; forest and tip restoration remain
// the caller's responsibility in this same batch. No historical-path change.
// Returns the exact reverse coin patch from the checked persisted undo/body.
// This partial helper does not authenticate those restored coins to a forest;
// use the full chainstate disconnect before preparing runtime publication.
std::vector<OrchardCoinChange> StageOrchardBlockCoinsAndStateDisconnectUnderChainstateLock(
    ChainDB&, const ChainWriteToken&, const OrchardBlockContext&,
    const OrchardBlockCandidate&, bool require_witness_commitment, rocksdb::WriteBatch&);

struct StagedOrchardChainstate {
    StagedOrchardBlock block;
    PreparedOrchardForest forest;
};
// Stage authoritative state, body, active transaction indexes, delta, forest
// marker, height index and both tip markers together. The caller supplies
// authenticated selected-branch headers with persisted header/work records,
// holds the writer lock, and must complete header/PoW, remaining resource,
// service/flatfile-index obligations before committing. The exact mixed compact
// filter is verified before commit and shares the batch; a retained mismatching
// filter is local corruption. The versioned commit record
// shares the batch. Peer proof data
// is checked against the resolved coins and full parent forest in this path.
// This neither commits nor publishes memory and is not full-block admission.
// An optional checkpoint is in the SAME batch; every block has a durable delta.
// DNRS v2 and frozen legacy contents are mandatory in this full adapter. At the
// first boundary only, authenticated_boundary must come from validated selected
// history (especially amount/epoch), never a block, RPC or wallet claim. Local
// legacy contents/markers are independently checked here, but this is NOT an
// accounting-history derivation API. On descendants omit the boundary input.
// Retirement receipt/undo/marker share this batch; append no legacy writes.
[[nodiscard]] StagedOrchardChainstate StageOrchardChainstateConnectUnderLock(
    ChainDB&, const ChainWriteToken&, const OrchardBlockContext&,
    const OrchardBlockCandidate&, const BlockHeader& parent, const UtreexoForest&,
    const OrchardBranchMtpLookup&,
    bool require_witness_commitment, bool checkpoint, rocksdb::WriteBatch&,
    const std::optional<storage::LegacyRetirementRecord>& authenticated_boundary = std::nullopt);

struct StagedOrchardDisconnect {
    std::vector<OrchardCoinChange> coins;
    UtreexoForest forest;
};
// Reads the stored delta and conventional undo; no pre-restart transition
// object is required. The reverse coin patch is bound to the exact delta leaves
// and authenticated parent forest. Prepare memory publication from this patch
// and private restored forest, then publish only after the outer commit.
// Retains block/header/undo/delta/filter records for reconnect; removes the disconnected
// active transaction/height indexes and checkpoint. Existing transaction-index
// rows are never overwritten on connect. Same empty-batch/remaining-obligations contract.
[[nodiscard]] StagedOrchardDisconnect StageOrchardChainstateDisconnectUnderLock(
    ChainDB&, const ChainWriteToken&, const OrchardBlockContext&,
    const OrchardBlockCandidate&, const BlockHeader& parent, const UtreexoForest&,
    bool require_witness_commitment,
    rocksdb::WriteBatch&);
} // namespace dinero::consensus
