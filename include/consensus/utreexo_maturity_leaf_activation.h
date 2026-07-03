#pragma once

#include <cstdint>
#include <limits>
#include "consensus/chainparams.h"

namespace dinero {
namespace consensus {

// Utreexo maturity-bound leaf fork.
//
// This is a hard fork because outputs created at/after the activation height
// use a different accumulator leaf preimage:
//   v1: txid | vout | amount | scriptPubKey
//   v2: txid | vout | amount | scriptPubKey | created_height | flags
//
// Existing UTXOs are not rehashed in-place. A UTXO's leaf version is determined
// by the height where that UTXO was created.
constexpr uint32_t UTREEXO_MATURITY_LEAF_HEIGHT_MAINNET = 60000;
constexpr uint32_t UTREEXO_MATURITY_LEAF_HEIGHT_TESTNET = 0;
constexpr uint32_t UTREEXO_MATURITY_LEAF_HEIGHT_REGTEST = 20;

// Conservative safety window for stateless validation of legacy leaves.
// Before activation+100, a pre-v2 leaf could still be an immature coinbase
// whose coinbase flag is not authenticated by the accumulator.
constexpr uint32_t UTREEXO_STATELESS_COINBASE_MATURITY = 100;
constexpr uint32_t UTREEXO_MATURITY_LEAF_LEGACY_GRACE_BLOCKS = 100;

enum class UtreexoStatelessMaturityStatus : uint8_t {
    SPENDABLE,
    IMMATURE_COINBASE,
    LEGACY_DEFERRED,
};

inline uint32_t GetUtreexoMaturityLeafActivationHeight() {
    switch (GetActiveChain()) {
        case Chain::REGTEST: return UTREEXO_MATURITY_LEAF_HEIGHT_REGTEST;
        case Chain::TESTNET: return UTREEXO_MATURITY_LEAF_HEIGHT_TESTNET;
        case Chain::MAINNET: return UTREEXO_MATURITY_LEAF_HEIGHT_MAINNET;
    }
    return std::numeric_limits<uint32_t>::max();
}

inline bool IsUtreexoMaturityLeafActive(uint32_t created_height) {
    return created_height >= GetUtreexoMaturityLeafActivationHeight();
}

inline uint8_t GetUtreexoProofFormatVersion(uint32_t block_height) {
    return block_height >= GetUtreexoMaturityLeafActivationHeight() ? 6 : 5;
}

inline bool IsLegacyUtreexoLeafStatelessSafe(uint32_t block_height) {
    const uint32_t activation = GetUtreexoMaturityLeafActivationHeight();
    if (activation == 0 || activation == std::numeric_limits<uint32_t>::max()) {
        return true;
    }
    if (activation > std::numeric_limits<uint32_t>::max() -
                         UTREEXO_MATURITY_LEAF_LEGACY_GRACE_BLOCKS) {
        return false;
    }
    return block_height >= activation + UTREEXO_MATURITY_LEAF_LEGACY_GRACE_BLOCKS;
}

inline UtreexoStatelessMaturityStatus EvaluateUtreexoStatelessMaturity(
    uint32_t validation_height,
    uint32_t created_height,
    bool is_coinbase) {
    if (!IsUtreexoMaturityLeafActive(created_height)) {
        return IsLegacyUtreexoLeafStatelessSafe(validation_height)
            ? UtreexoStatelessMaturityStatus::SPENDABLE
            : UtreexoStatelessMaturityStatus::LEGACY_DEFERRED;
    }

    if (!is_coinbase) {
        return UtreexoStatelessMaturityStatus::SPENDABLE;
    }

    if (validation_height < created_height ||
        validation_height - created_height < UTREEXO_STATELESS_COINBASE_MATURITY) {
        return UtreexoStatelessMaturityStatus::IMMATURE_COINBASE;
    }

    return UtreexoStatelessMaturityStatus::SPENDABLE;
}

}  // namespace consensus
}  // namespace dinero
