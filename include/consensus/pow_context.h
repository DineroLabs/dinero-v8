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

// Null chain-db adapter for header-only difficulty validation.
//
// HeaderChainSelector::ValidateHeader computes expected difficulty purely from a
// candidate's parent HeaderIndexEntry (its in-memory ancestry supplies MTP and
// the genesis-anchored reference), so it has no ChainDB. Instantiating the
// shared GetNextWorkRequiredForCandidate with the real ChainDB type would force
// the header-validation TU to link ChainDB's storage methods. Passing a null
// NoChainDb* instead keeps ONE shared expected-bits function while letting the
// chain_db fallback branch resolve to trivial inline no-ops — they are never
// called at runtime (the pointer is null and the parent_entry path is taken).
struct NoChainDb {
    StatusOr<uint256> getBlockHashByHeight(int) const {
        return StatusOr<uint256>(Status::NotFound);
    }
    StatusOr<BlockHeader> getHeader(const uint256&) const {
        return StatusOr<BlockHeader>(Status::NotFound);
    }
    StatusOr<Block> getBlock(const uint256&) const {
        return StatusOr<Block>(Status::NotFound);
    }
};

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

// #314: robust fallback for the ASERT time anchor (block 1's timestamp).
// On a node resynced from an AssumeUTXO snapshot, block 1 (pre-snapshot) is not
// in ChainDB's height index until background backfill reaches it, so the ChainDB
// lookups below return 0. Falling back to genesis.nTime yields a WRONG anchor
// (~37h too early on mainnet) -> getblocktemplate produces too-easy difficulty
// -> solo blocks rejected bad-diffbits, while validation (which resolves block 1
// via the in-memory header ancestry) uses the correct time. Return block 1's
// verified timestamp here so template == validation regardless of backfill state.
inline int64_t CanonicalAsertAnchorTimeFallback() {
    // Block 1 (000000194ff6bff58b929bc41978ef1b329a1d3737598eac86a57ec481c4d643)
    // mainnet timestamp = the time anchor the live network's ASERT runs on.
    static constexpr int64_t kMainnetAsertAnchorTime = 1776518061;
    if (dinero::Params().name == "mainnet") {
        return kMainnetAsertAnchorTime;
    }
    return static_cast<int64_t>(dinero::Params().genesis.nTime);
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

    return CanonicalAsertAnchorTimeFallback();  // #314: not genesis.nTime
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

    return CanonicalAsertAnchorTimeFallback();  // #314: not genesis.nTime
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
inline std::optional<AsertInput> BuildAsertInputForCandidateTimes(
    int64_t known_parent_mtp,
    int64_t known_block1_time,
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
    } else if (known_parent_mtp > 0) {
        parent_mtp = known_parent_mtp;
    } else {
        parent_mtp = GetMedianTimePastAtHeight(chain_db, static_cast<uint32_t>(target_height - 1));
    }

    if (target_height > 1 && parent_mtp == 0) {
        return std::nullopt;
    }

    AsertInput input;
    input.target_height = target_height;
    input.reference_time = GetConsensusReferenceTime(target_height, parent_mtp, candidate_timestamp);
    const int64_t anchor_time = (target_height <= 1)
        ? static_cast<int64_t>(dinero::Params().genesis.nTime)
        : (known_block1_time > 0
            ? known_block1_time
            : GetCanonicalAsertAnchorTime(chain_db, target_height));
    input.anchor = GetCanonicalAsertAnchor(
        consensus,
        anchor_time);
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
    // Internal header validation calls this while HeaderChainSelector holds its
    // lock. External callers must use BuildAsertInputForCandidateTimes with a
    // value-only context obtained through GetAsertContextByHash().
    int64_t known_parent_mtp = 0;
    if (parent_index) {
        known_parent_mtp = static_cast<int64_t>(parent_index->GetMedianTimePast());
    } else if (parent_entry) {
        known_parent_mtp = static_cast<int64_t>(parent_entry->GetMedianTimePast());
    }
    const int64_t known_block1_time =
        GetKnownAncestryTimestamp(parent_index, parent_entry, 1);
    return BuildAsertInputForCandidateTimes(
        known_parent_mtp,
        known_block1_time,
        chain_db,
        target_height,
        candidate_timestamp,
        consensus);
}

} // namespace dinero
