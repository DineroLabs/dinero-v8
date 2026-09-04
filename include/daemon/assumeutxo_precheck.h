// Side-chain precheck exemptions for AssumeUTXO modes.
//
// BlockAcceptor classifies a block as a main-chain extension by comparing its
// parent against ChainDB::getTip(). Under AssumeUTXO there are two notions of
// "tip": the active snapshot tip (the compiled-in base) and ChainDB's tip,
// which still trails during background replay. Blocks that are perfectly
// canonical therefore land in the side-chain branch, where the accept-time
// precheck restores a historical Utreexo forest — work that is both useless
// here and pathologically repeated on every duplicate delivery.
//
// These predicates name the two cases where that precheck must be skipped.
// Kept free of daemon headers so they are unit-testable in isolation.
#pragma once

#include <cstdint>

#include "primitives/uint256.h"

namespace dinero {
namespace daemon {

// The AssumeUTXO facts the precheck decision depends on.
struct AssumeUTXOPrecheckContext {
    bool assumeutxo_active = false;
    // Mirrors GetConfig().assumeutxo_forward_connect — the same source the
    // deferral gate reads, so an exemption cannot fire in a mode where
    // deferral is not actually on.
    bool forward_connect_enabled = false;
    uint32_t base_height = 0;
    dinero::uint256 base_block;
};

// Pre-base historical body replayed by AssumeUtxoReplayEngine. Those bodies
// are deliberately not promoted into ChainDB until the replay proves the
// snapshot, so routing them through the live side-chain precheck misroutes
// them. (Pre-existing behaviour; named here, not changed.)
bool IsHistoricalPreBaseBody(const AssumeUTXOPrecheckContext& ctx,
                             uint32_t parent_height);

// Classic deferred mode only: a block whose parent IS the snapshot base.
// Its parent is the compiled-in trust anchor, so "confirm the parent is on
// the main chain" — the precheck's entire stated job — is already answered.
bool IsDeferredSnapshotBaseChild(const AssumeUTXOPrecheckContext& ctx,
                                 uint32_t parent_height,
                                 const dinero::uint256& parent_hash);

// The guard itself.
bool ShouldSkipSideChainPrecheck(const AssumeUTXOPrecheckContext& ctx,
                                 uint32_t parent_height,
                                 const dinero::uint256& parent_hash);

}  // namespace daemon
}  // namespace dinero
