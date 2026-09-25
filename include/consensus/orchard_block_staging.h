#pragma once
#include "consensus/orchard_state_transition.h"

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
    std::span<const VerifiedOrchardAuthorizations>, rocksdb::WriteBatch&);
} // namespace dinero::consensus
