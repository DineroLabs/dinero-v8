#pragma once
#include "wallet/orchard_account_state.h"
namespace dinero { class ChainDB; class BlockStorage; }
namespace dinero::wallet {
// Read-only, stateful archival adapter for an already authenticated selected
// ChainDB. Caller holds chain/wallet snapshot locks throughout; audit/replay of
// the database is a prerequisite, not performed by this function. Payload must
// already be authenticated/decrypted by WalletSnapshotStore.
//
// Restore only at the current validated Orchard checkpoint. A stale/divergent
// wallet must use retained undo or explicit rescan; never silently promote it.
// Transaction-index entries are locators only: selected block/header/body and
// exact transaction identity are checked, and previous outputs are read from
// their original authenticated bodies, not today's spent/unspent coin cache.
// Requires archival bodies and active txindex coverage. Missing/pruned data and
// corruption are local lookup failures. No writes, repairs, relay, or activation.
[[nodiscard]] OrchardAccountState RestoreOrchardAccountFromChainUnderLock(
    const ChainDB&, const BlockStorage* archival_blocks,
    const orchard::WalletStateBytes&, orchard::SigningDomain,
    const orchard::FullViewingKeyBytes&, uint32_t activation_height);
} // namespace dinero::wallet
