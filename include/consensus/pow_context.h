#pragma once

#include "consensus/asert.h"
#include "consensus/block_index.h"
#include "consensus/consensus.hpp"
#include "consensus/header_chain.h"
#include "consensus/chainparams.h"
#include "storage/archival_block_reader.h"
#include "storage/chain_db.h"
#include <algorithm>
#include <ctime>
#include <optional>
#include <vector>

namespace dinero {

inline Consensus GetConsensusForCurrentNetwork() {
    Consensus consensus;
    if (dinero::Params().name != "mainnet") {
        const uint32_t network_pow_limit = dinero::Params().pow_limit_bits;
        consensus.genesisBits = network_pow_limit;
        consensus.asertAnchorBits = network_pow_limit;
        consensus.powLimitBits = network_pow_limit;
    }
    return consensus;
}

inline bool IsAsertActive(int32_t target_height, const Consensus& consensus) {
    return consensus.daaType == DAAType::ASERT && target_height >= 1;
}

inline AsertParams GetAsertParams(const Consensus& consensus) {
    AsertParams params;
    params.target_spacing_secs = consensus.targetSpacingSec;
    params.half_life_secs = consensus.asertHalfLifeSec;
    params.pow_limit.SetCompact(consensus.powLimitBits);
    return params;
}

inline AsertAnchor GetCanonicalAsertAnchor(const Consensus& consensus, int64_t anchor_time) {
    return AsertAnchor{
        static_cast<int32_t>(consensus.asertAnchorHeight),
        anchor_time,
        consensus.asertAnchorBits,
    };
}

inline int64_t GetConsensusReferenceTime(
    int32_t target_height,
    int64_t parent_mtp,
    int64_t candidate_timestamp)
{
    if (target_height <= 1) {
        return candidate_timestamp;
    }
    return std::max(parent_mtp + 1, candidate_timestamp);
}

template <typename ChainDBType>
inline int64_t GetChainTimestampAtHeight(ChainDBType* chain_db, uint32_t height) {
    if (!chain_db) {
        return 0;
    }

    auto hash_result = chain_db->getBlockHashByHeight(height);
    if (hash_result.status() != dinero::Status::Ok) {
        return 0;
    }

    auto header_result = chain_db->getHeader(hash_result.value());
    if (header_result.status() == dinero::Status::Ok) {
        return static_cast<int64_t>(header_result.value().timestamp);
    }

    auto block_result = chain_db->getBlock(hash_result.value());
    if (block_result.status() == dinero::Status::Ok) {
        return static_cast<int64_t>(block_result.value().header.timestamp);
    }

    return 0;
}

inline int64_t GetChainTimestampAtHeight(ChainDB* chain_db,
                                         BlockStorage* block_storage,
                                         uint32_t height) {
    if (!chain_db) {
        return 0;
    }

    auto hash_result = chain_db->getBlockHashByHeight(height);
    if (hash_result.status() != dinero::Status::Ok) {
        return 0;
    }

    auto header_result = chain_db->getHeader(hash_result.value());
    if (header_result.status() == dinero::Status::Ok) {
        return static_cast<int64_t>(header_result.value().timestamp);
    }

    auto block_result = storage::ReadArchivalBlock(*chain_db, block_storage, hash_result.value());
    if (block_result.status() == dinero::Status::Ok) {
        return static_cast<int64_t>(block_result.value().header.timestamp);
    }

    return 0;
}

template <typename ChainDBType>
inline int64_t GetMedianTimePastAtHeight(ChainDBType* chain_db, uint32_t height) {
    if (!chain_db) {
        return 0;
    }

    std::vector<int64_t> timestamps;
    timestamps.reserve(11);

    for (int32_t h = static_cast<int32_t>(height);
         h >= 0 && timestamps.size() < 11;
         --h) {
        const int64_t ts = GetChainTimestampAtHeight(chain_db, static_cast<uint32_t>(h));
        if (ts == 0) {
            break;
        }
        timestamps.push_back(ts);
    }

    if (timestamps.empty()) {
        return 0;
    }

    std::sort(timestamps.begin(), timestamps.end());
    return timestamps[timestamps.size() / 2];
}

inline int64_t GetMedianTimePastAtHeight(ChainDB* chain_db,
                                         BlockStorage* block_storage,
                                         uint32_t height) {
    if (!chain_db) {
        return 0;
    }

    std::vector<int64_t> timestamps;
    timestamps.reserve(11);

    for (int32_t h = static_cast<int32_t>(height);
         h >= 0 && timestamps.size() < 11;
         --h) {
        const int64_t ts =
            GetChainTimestampAtHeight(chain_db, block_storage, static_cast<uint32_t>(h));
        if (ts == 0) {
            break;
        }
        timestamps.push_back(ts);
    }

    if (timestamps.empty()) {
        return 0;
    }

    std::sort(timestamps.begin(), timestamps.end());
    return timestamps[timestamps.size() / 2];
}

inline const CBlockIndex* GetBlockIndexAncestor(const CBlockIndex* tip, uint32_t ancestor_height) {
    const CBlockIndex* cursor = tip;
    while (cursor && cursor->height > ancestor_height) {
        cursor = cursor->pprev;
    }
    if (cursor && cursor->height == ancestor_height) {
        return cursor;
    }
    return nullptr;
}

inline int64_t GetKnownAncestryTimestamp(
    const CBlockIndex* parent_index,
    const dinero::consensus::HeaderIndexEntry* parent_entry,
    uint32_t height)
{
    if (parent_index) {
        if (const auto* ancestor = GetBlockIndexAncestor(parent_index, height)) {
            return static_cast<int64_t>(ancestor->timestamp);
        }
    }
    if (parent_entry) {
        if (const auto* ancestor = parent_entry->GetAncestor(height)) {
            return static_cast<int64_t>(ancestor->header.timestamp);
        }
    }
    return 0;
}

template <typename ChainDBType>
inline int64_t GetCanonicalAsertAnchorTime(
    ChainDBType* chain_db,
    int32_t target_height)
{
    if (target_height <= 1) {
        return static_cast<int64_t>(dinero::Params().genesis.nTime);
    }

    const int64_t block1_time = GetChainTimestampAtHeight(chain_db, 1);
    if (block1_time > 0) {
        return block1_time;
    }

    return static_cast<int64_t>(dinero::Params().genesis.nTime);
}

inline int64_t GetCanonicalAsertAnchorTime(
    ChainDB* chain_db,
    BlockStorage* block_storage,
    int32_t target_height)
{
    if (target_height <= 1) {
        return static_cast<int64_t>(dinero::Params().genesis.nTime);
    }

    const int64_t block1_time = GetChainTimestampAtHeight(chain_db, block_storage, 1);
    if (block1_time > 0) {
        return block1_time;
    }

    return static_cast<int64_t>(dinero::Params().genesis.nTime);
}

template <typename ChainDBType>
inline int64_t GetCanonicalAsertAnchorTime(
    const CBlockIndex* parent_index,
    const dinero::consensus::HeaderIndexEntry* parent_entry,
    ChainDBType* chain_db,
    int32_t target_height)
{
    if (target_height <= 1) {
        return static_cast<int64_t>(dinero::Params().genesis.nTime);
    }

    const int64_t ancestry_anchor_time = GetKnownAncestryTimestamp(parent_index, parent_entry, 1);
    if (ancestry_anchor_time > 0) {
        return ancestry_anchor_time;
    }

    return GetCanonicalAsertAnchorTime(chain_db, target_height);
}

template <typename ChainDBType>
inline std::optional<AsertInput> BuildAsertInputForNextBlockOnChainDB(
    ChainDBType* chain_db,
    int32_t target_height,
    int64_t candidate_timestamp,
    const Consensus& consensus)
{
    if (!IsAsertActive(target_height, consensus)) {
        return std::nullopt;
    }

    const int64_t parent_mtp = (target_height == 1)
        ? static_cast<int64_t>(dinero::Params().genesis.nTime)
        : GetMedianTimePastAtHeight(chain_db, static_cast<uint32_t>(target_height - 1));

    if (target_height > 1 && parent_mtp == 0) {
        return std::nullopt;
    }

    AsertInput input;
    input.target_height = target_height;
    input.reference_time = GetConsensusReferenceTime(target_height, parent_mtp, candidate_timestamp);
    input.anchor = GetCanonicalAsertAnchor(
        consensus,
        GetCanonicalAsertAnchorTime(chain_db, target_height));
    input.params = GetAsertParams(consensus);
    return input;
}

inline std::optional<AsertInput> BuildAsertInputForNextBlockOnChainDB(
    ChainDB* chain_db,
    BlockStorage* block_storage,
    int32_t target_height,
    int64_t candidate_timestamp,
    const Consensus& consensus)
{
    if (!IsAsertActive(target_height, consensus)) {
        return std::nullopt;
    }

    const int64_t parent_mtp = (target_height == 1)
        ? static_cast<int64_t>(dinero::Params().genesis.nTime)
        : GetMedianTimePastAtHeight(
              chain_db,
              block_storage,
              static_cast<uint32_t>(target_height - 1));

    if (target_height > 1 && parent_mtp == 0) {
        return std::nullopt;
    }

    AsertInput input;
    input.target_height = target_height;
    input.reference_time = GetConsensusReferenceTime(target_height, parent_mtp, candidate_timestamp);
    input.anchor = GetCanonicalAsertAnchor(
        consensus,
        GetCanonicalAsertAnchorTime(chain_db, block_storage, target_height));
    input.params = GetAsertParams(consensus);
    return input;
}

template <typename ChainDBType>
inline std::optional<AsertInput> BuildAsertInputForCandidate(
    const CBlockIndex* parent_index,
    const dinero::consensus::HeaderIndexEntry* parent_entry,
    ChainDBType* chain_db,
    int32_t target_height,
    int64_t candidate_timestamp,
    const Consensus& consensus)
{
    if (!IsAsertActive(target_height, consensus)) {
        return std::nullopt;
    }

    int64_t parent_mtp = 0;
    if (target_height == 1) {
        parent_mtp = static_cast<int64_t>(dinero::Params().genesis.nTime);
    } else if (parent_index) {
        parent_mtp = static_cast<int64_t>(parent_index->GetMedianTimePast());
    } else if (parent_entry) {
        parent_mtp = static_cast<int64_t>(parent_entry->GetMedianTimePast());
    } else {
        parent_mtp = GetMedianTimePastAtHeight(chain_db, static_cast<uint32_t>(target_height - 1));
    }

    if (target_height > 1 && parent_mtp == 0) {
        return std::nullopt;
    }

    AsertInput input;
    input.target_height = target_height;
    input.reference_time = GetConsensusReferenceTime(target_height, parent_mtp, candidate_timestamp);
    input.anchor = GetCanonicalAsertAnchor(
        consensus,
        GetCanonicalAsertAnchorTime(parent_index, parent_entry, chain_db, target_height));
    input.params = GetAsertParams(consensus);
    return input;
}

} // namespace dinero
