#pragma once
#include "primitives/uint256.h"
#include <cstdint>

namespace dinero { class ChainDB; class BlockStorage; }
namespace dinero::consensus {
struct SelectedLegacyPoolAccounting {
    uint32_t epoch_height;
    uint32_t parent_height;
    uint256 parent_hash;
    uint64_t value_una;
    uint64_t blocks_read;
    uint64_t shielded_transactions;
};
// Read-only accounting over the last legacy epoch of an ALREADY VALIDATED
// selected archival ChainDB. Caller holds the selected writer lock and keeps
// Params fixed throughout. This does not establish historical consensus
// validity, unspentness, proof soundness or snapshot provenance. Those remain
// prerequisites; a validated-tip marker alone is not that evidence.
//
// Requires the exact parent of selected Orchard activation, complete bodies
// for this epoch and selected txindex/body coverage for its funding prevouts.
// Uses transparent input/output amounts and explicit fees, never a wallet
// balance or a bundle's claimed value balance. Confidential amounts, unbound
// historical bundle formats, absent fees and incomplete sources fail closed.
// The work budget is local policy, not consensus; exceeding it is unavailable
// accounting, not an invalid block. No partial amount is returned. Does not
// write or construct the frozen-state retirement receipt; the caller must bind
// this result to that same parent/epoch before staging retirement.
[[nodiscard]] SelectedLegacyPoolAccounting DeriveSelectedLegacyPoolAccountingUnderLock(
    const ChainDB&, const BlockStorage*, uint32_t maximum_epoch_blocks);
} // namespace dinero::consensus
