#pragma once
#include "consensus/orchard_state_transition.h"
#include "primitives/orchard_block_reader.h"
#include "consensus/orchard_block_coins.h"

namespace rocksdb { class WriteBatch; }
namespace dinero { class ChainDB; class ChainWriteToken; }

namespace dinero::consensus {
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
void StageOrchardBlockCoinsAndStateDisconnectUnderChainstateLock(
    ChainDB&, const ChainWriteToken&, const OrchardBlockContext&,
    const OrchardBlockCandidate&, bool require_witness_commitment, rocksdb::WriteBatch&);
} // namespace dinero::consensus
