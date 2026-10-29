#pragma once
#include "consensus/orchard_block_coins.h"
#include "consensus/block_filter.h"
namespace dinero::consensus {
// Exact ordered resolved inputs include same-block spends. Created scripts
// exclude OP_RETURN; spent scripts include every nonempty script. The key is
// the parent hash so committing the result in coinbase is not circular.
[[nodiscard]] GCSFilter BuildOrchardBlockFilter(const PreparedOrchardBlockCoins&);
// Requires the coins to describe this exact candidate. A filter commitment is
// required under the existing height policy, including the empty-filter case.
// This is a pre-commit gate, not a post-commit warning or full block validation.
[[nodiscard]] GCSFilter CheckOrchardBlockFilter(const OrchardBlockCandidate&,
    const PreparedOrchardBlockCoins&);
} // namespace dinero::consensus
