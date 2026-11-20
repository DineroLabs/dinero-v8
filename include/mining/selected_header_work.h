#pragma once

#include "consensus/header_chain.h"
#include "consensus/pow.hpp"

namespace dinero::mining {

// Mining must use the same parent ancestry that validates the candidate
// header. A snapshot-origin ChainDB may not index the pre-snapshot timing
// anchor even though the authenticated header selector still retains it.
template <typename ChainDBType>
uint32_t NextWorkRequiredForParent(
    const consensus::HeaderChainSelector* headers,
    const uint256& parent_hash,
    int32_t target_height,
    int64_t candidate_timestamp,
    const Consensus& rules,
    ChainDBType* chain_db)
{
    if (!headers) {
        return GetNextWorkRequiredWithChainDB(
            target_height, candidate_timestamp, rules, chain_db);
    }
    if (rules.skipProofOfWork && target_height >= 1) return rules.powLimitBits;
    if (target_height == 0) return rules.genesisBits;
    if (target_height < 0 || parent_hash.IsNull()) return 0;

    consensus::HeaderAsertContext context;
    if (!headers->GetAsertContextByHash(
            parent_hash, context, TimingUpgradeAnchorHeight(target_height, rules)) ||
        context.parent_height != static_cast<uint32_t>(target_height - 1)) {
        return 0;
    }
    const auto input = BuildAsertInputForCandidateTimes(
        context.parent_mtp, context.block1_time, chain_db,
        target_height, candidate_timestamp, rules, context.timing_anchor);
    return input ? ComputeAsertBits(*input) : 0;
}

inline uint32_t NextWorkRequiredForParent(
    const consensus::HeaderChainSelector* headers,
    const uint256& parent_hash,
    int32_t target_height,
    int64_t candidate_timestamp,
    const Consensus& rules,
    ChainDB* chain_db,
    BlockStorage* block_storage)
{
    if (!headers) {
        return GetNextWorkRequiredWithChainDB(
            target_height, candidate_timestamp, rules, chain_db, block_storage);
    }
    return NextWorkRequiredForParent(
        headers, parent_hash, target_height, candidate_timestamp, rules, chain_db);
}

} // namespace dinero::mining
