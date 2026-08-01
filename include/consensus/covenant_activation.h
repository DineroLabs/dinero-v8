#pragma once
/**
 * Covenant Activation Parameters
 *
 * Authoritative, per-feature height activation for Taproot script-path
 * spending and covenant opcodes.
 *
 * Activation rules:
 *   - Script-path activation is independent from covenant activation.
 *   - CTV retains NOP4 behavior before its own activation.
 *   - Opcodes in BIP342 OP_SUCCESS slots retain immediate-success behavior
 *     before their own activation.
 *   - Key-path Taproot spending is always allowed regardless of activation
 *   - Transparent (non-Taproot) transactions are never affected
 *
 * Covenant activation must come AFTER ring activation to avoid overlapping
 * consensus changes during the same activation window.
 */

#include <cstdint>
#include "consensus/chainparams.h"
#include "consensus/script_interpreter.h"

namespace dinero {
namespace consensus {

struct CovenantActivationParams {
    static bool IsActive(uint32_t height, uint32_t activation_height) {
        return activation_height != UINT32_MAX &&
               height >= activation_height;
    }

    static bool IsScriptPathActive(uint32_t height,
                                   const ChainParams& params) {
        return IsActive(height,
                        params.taproot_scriptpath_activation_height);
    }

    static uint32_t CovenantFlags(uint32_t height,
                                  const ChainParams& params) {
        uint32_t flags = SCRIPT_VERIFY_NONE;
        if (IsActive(height, params.ctv_activation_height)) {
            flags |= SCRIPT_VERIFY_CHECKTEMPLATEVERIFY;
        }
        if (IsActive(height, params.csfs_activation_height)) {
            flags |= SCRIPT_VERIFY_CHECKSIGFROMSTACK;
        }
        if (IsActive(height, params.txhash_activation_height)) {
            flags |= SCRIPT_VERIFY_TXHASH;
        }
        if (IsActive(height, params.ccv_activation_height)) {
            flags |= SCRIPT_VERIFY_CHECKCONTRACT;
        }
        return flags;
    }

    static uint32_t StandardFlags(uint32_t height,
                                  const ChainParams& params) {
        return SCRIPT_VERIFY_STANDARD | CovenantFlags(height, params);
    }
};

} // namespace consensus
} // namespace dinero
