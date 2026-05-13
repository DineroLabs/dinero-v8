#pragma once
/**
 * Covenant Activation Parameters
 *
 * Height-based activation for Taproot script-path spending with covenant opcodes
 * (OP_CTV, OP_CHECKSIGFROMSTACK, OP_TXHASH, OP_CHECKCONTRACTVERIFY).
 *
 * Follows the same pattern as RingActivationParams.
 *
 * Activation rules:
 *   - Before activation: Taproot script-path spends are rejected (key-path only)
 *   - After activation: Script-path spends are accepted, covenant opcodes execute
 *   - Key-path Taproot spending is always allowed regardless of activation
 *   - Transparent (non-Taproot) transactions are never affected
 *
 * Covenant activation must come AFTER ring activation to avoid overlapping
 * consensus changes during the same activation window.
 */

#include <cstdint>
#include "consensus/chainparams.h"

namespace dinero {
namespace consensus {

struct CovenantActivationParams {
    // Height at which Taproot script-path + covenant opcodes become valid.
    // Before this height: script-path spends rejected, key-path only.
    // After this height: full Tapscript execution with CTV/CSFS/TXHASH/CCV.
    static constexpr uint32_t MAINNET_ACTIVATION_HEIGHT = 1;
    static constexpr uint32_t TESTNET_ACTIVATION_HEIGHT = 200;
    static constexpr uint32_t REGTEST_ACTIVATION_HEIGHT = 20;

    /**
     * Check if Taproot script-path spending (covenants) is active at the given height.
     *
     * @param height  Block height being validated
     * @param chain   Network type (MAINNET, TESTNET, REGTEST)
     * @return true if script-path + covenant opcodes are valid
     */
    static bool IsCovenantActive(uint32_t height, Chain chain) {
        return height >= GetActivationHeight(chain);
    }

    /**
     * Get the activation height for a given network.
     */
    static uint32_t GetActivationHeight(Chain chain) {
        switch (chain) {
            case Chain::MAINNET: return MAINNET_ACTIVATION_HEIGHT;
            case Chain::TESTNET: return TESTNET_ACTIVATION_HEIGHT;
            case Chain::REGTEST: return REGTEST_ACTIVATION_HEIGHT;
            default: return MAINNET_ACTIVATION_HEIGHT;
        }
    }
};

} // namespace consensus
} // namespace dinero
