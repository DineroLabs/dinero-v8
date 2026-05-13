#pragma once
#include "consensus.hpp"
#include "consensus/asert.h"
#include "consensus/pow_context.h"
#include "consensus/pow_compact.h"
#include "consensus/pow_asert_native.hpp"
#include "consensus/chainparams.h"
#include <functional>
#include <algorithm>
#include <cstdio>
#include <iostream>

using namespace dinero;

// Helper for diagnostic logging
static inline std::string BitsHex(uint32_t b) {
    char s[11];
    std::snprintf(s, sizeof(s), "0x%08x", b);
    return s;
}

/**
 * Get required difficulty for next block (CONSENSUS-CRITICAL)
 *
 * Compatibility wrapper around the explicit ASERT contract.
 *
 * Hidden policy is intentionally forbidden here:
 * - no chain lookups
 * - no anchor derivation
 * - no fallback semantics beyond the existing genesis/regtest gates
 *
 * Callers must already know the consensus-correct reference time and anchor time.
 */
inline uint32_t GetNextWorkRequired(
    uint32_t height,
    uint32_t prevBits,
    int64_t prevMTP,
    int64_t currentMTP,
    int64_t anchorTime,
    const Consensus& c,
    std::function<int64_t(uint32_t)> getBlockTime = nullptr)
{
    // Legacy parameters are accepted for API stability, but ASERT math is driven by
    // the explicit target height, reference time, and anchor tuple only.
    (void)prevBits;
    (void)prevMTP;
    (void)getBlockTime;

    // Regtest: easy difficulty from block 1+
    try {
        const auto& params = dinero::Params();
        if (params.name == "regtest" && height >= 1) {
            return c.powLimitBits;
        }
    } catch (...) {
        // Continue with normal logic
    }

    // Genesis: fixed difficulty
    if (height == 0) {
        return c.genesisBits;
    }

    return ComputeAsertBits(AsertInput{
        static_cast<int32_t>(height),
        currentMTP,
        GetCanonicalAsertAnchor(c, anchorTime),
        GetAsertParams(c),
    });
}

/**
 * Legacy function signature for backwards compatibility
 */
inline uint32_t GetNextWorkRequired(
    uint32_t height,
    uint32_t prevBits,
    int64_t firstTs,
    int64_t lastTs,
    const Consensus& c)
{
    return GetNextWorkRequired(height, prevBits, firstTs, lastTs, lastTs, c);
}

/**
 * Canonical mining/RPC helper: build consensus context from ChainDB once, then
 * delegate into the pure ASERT helper.
 *
 * This is the only wrapper allowed to derive reference time / anchor time from
 * chain state for active-chain mining/template use.
 */
template<typename ChainDBType>
inline uint32_t GetNextWorkRequiredWithChainDB(
    int32_t height,
    int64_t candidateTime,
    const Consensus& c,
    ChainDBType* chain_db)
{
    try {
        const auto& params = dinero::Params();
        if (params.name == "regtest" && height >= 1) {
            return c.powLimitBits;
        }
    } catch (...) {
    }

    if (height == 0) {
        return c.genesisBits;
    }

    const auto input = BuildAsertInputForNextBlockOnChainDB(
        chain_db,
        height,
        candidateTime,
        c);
    if (!input.has_value()) {
        return 0;
    }
    return ComputeAsertBits(*input);
}

inline uint32_t GetNextWorkRequiredWithChainDB(
    int32_t height,
    int64_t candidateTime,
    const Consensus& c,
    ChainDB* chain_db,
    BlockStorage* block_storage)
{
    try {
        const auto& params = dinero::Params();
        if (params.name == "regtest" && height >= 1) {
            return c.powLimitBits;
        }
    } catch (...) {
    }

    if (height == 0) {
        return c.genesisBits;
    }

    const auto input = BuildAsertInputForNextBlockOnChainDB(
        chain_db,
        block_storage,
        height,
        candidateTime,
        c);
    if (!input.has_value()) {
        return 0;
    }
    return ComputeAsertBits(*input);
}

template<typename ChainDBType>
inline uint32_t GetNextWorkRequiredWithChainDB(
    uint32_t height,
    uint32_t prevBits,
    int64_t prevMTP,
    int64_t currentMTP,
    int64_t anchorTime,
    const Consensus& c,
    ChainDBType* chain_db)
{
    (void)prevBits;
    (void)prevMTP;
    (void)anchorTime;

    if (!chain_db) {
        return GetNextWorkRequired(height, prevBits, prevMTP, currentMTP, anchorTime, c);
    }

    return GetNextWorkRequiredWithChainDB(
        static_cast<int32_t>(height),
        currentMTP,
        c,
        chain_db);
}

inline uint32_t GetNextWorkRequiredWithChainDB(
    uint32_t height,
    uint32_t prevBits,
    int64_t prevMTP,
    int64_t currentMTP,
    int64_t anchorTime,
    const Consensus& c,
    ChainDB* chain_db,
    BlockStorage* block_storage)
{
    (void)prevBits;
    (void)prevMTP;
    (void)anchorTime;

    if (!chain_db) {
        return GetNextWorkRequired(height, prevBits, prevMTP, currentMTP, anchorTime, c);
    }

    return GetNextWorkRequiredWithChainDB(
        static_cast<int32_t>(height),
        currentMTP,
        c,
        chain_db,
        block_storage);
}

template<typename ChainDBType>
inline uint32_t GetNextWorkRequiredForCandidate(
    int32_t target_height,
    int64_t candidate_timestamp,
    const Consensus& c,
    const CBlockIndex* parent_index,
    const dinero::consensus::HeaderIndexEntry* parent_entry,
    ChainDBType* chain_db)
{
    // Validation path wrapper: derive reference/anchor context from the candidate
    // chain ancestry, then delegate into the pure helper. No alternate anchor policy
    // is allowed here.
    try {
        const auto& params = dinero::Params();
        if (params.name == "regtest" && target_height >= 1) {
            return c.powLimitBits;
        }
    } catch (...) {
    }

    if (target_height == 0) {
        return c.genesisBits;
    }

    const auto input = BuildAsertInputForCandidate(
        parent_index,
        parent_entry,
        chain_db,
        target_height,
        candidate_timestamp,
        c);
    if (!input.has_value()) {
        return 0;
    }
    return ComputeAsertBits(*input);
}
