#pragma once
/**
 * V5 Freeze Fork Activation Parameters
 *
 * See docs/consensus/V5_FREEZE_FORK_SPEC.md for the design rationale.
 *
 * At and above FREEZE_FORK_ACTIVATION_HEIGHT, every non-coinbase transaction
 * in a block MUST satisfy these output gates:
 *
 *   Gate 1. No confidential outputs.
 *           For every output in tx.vout: output.is_confidential == false.
 *
 *   Gate 2. (Historical) No ring / ring-covenant transaction formats.
 *           v3 and v4 tx versions were excised on Apr 17 2026 (v7 launch);
 *           rejection now happens at standardness/version-range check time
 *           in mempool_policy and validation_mempool. The runtime gate is
 *           gone but the version-range allowlist enforces the same outcome.
 *
 *   Gate 3. Taproot-only spendable outputs.
 *           For every output in tx.vout, output.scriptPubKey MUST be either:
 *             - witness_v1_taproot (0x51 0x20 <32 bytes>), or
 *             - OP_RETURN (0x6a ...)  — provably unspendable, never enters Utreexo.
 *
 * Existing pre-activation transparent / non-ring UTXOs remain spendable so
 * long as the spending transaction itself satisfies the freeze gates.
 * Pre-activation confidential UTXOs do NOT gain a post-freeze drain path:
 * Gate 1 rejects them outright.
 *
 * Initial mainnet / testnet values are UINT32_MAX so the gates compile but do
 * not activate on any live chain. Regtest = 200 so the regtest harness can
 * exercise the activation boundary deterministically.
 */

#include <cstdint>
#include <vector>
#include "consensus/chainparams.h"

namespace dinero {
namespace consensus {

struct FreezeForkActivationParams {
    static constexpr uint32_t MAINNET_ACTIVATION_HEIGHT = 4000;
    static constexpr uint32_t TESTNET_ACTIVATION_HEIGHT = UINT32_MAX;
    static constexpr uint32_t REGTEST_ACTIVATION_HEIGHT = 200;

    static bool IsFreezeForkActive(uint32_t height, Chain chain) {
        return height >= GetActivationHeight(chain);
    }

    static uint32_t GetActivationHeight(Chain chain) {
        switch (chain) {
            case Chain::MAINNET: return MAINNET_ACTIVATION_HEIGHT;
            case Chain::TESTNET: return TESTNET_ACTIVATION_HEIGHT;
            case Chain::REGTEST: return REGTEST_ACTIVATION_HEIGHT;
            default: return MAINNET_ACTIVATION_HEIGHT;
        }
    }
};

inline bool IsFreezeForkAllowedScript(const std::vector<uint8_t>& script_pubkey) {
    if (script_pubkey.empty()) {
        return false;
    }

    if (script_pubkey[0] == 0x6a) {
        return true;
    }

    if (script_pubkey.size() == 34 &&
        script_pubkey[0] == 0x51 &&
        script_pubkey[1] == 0x20) {
        return true;
    }

    return false;
}

} // namespace consensus
} // namespace dinero
