#pragma once

#include "consensus/chainparams.h"
#include "consensus/covenant_activation.h"
#include "consensus/script.h"
#include "primitives/transaction.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dinero::policy {
namespace detail {

inline bool SkipPushData(
    const std::vector<uint8_t>& script,
    size_t& offset,
    uint8_t opcode) {
    uint64_t size = 0;
    if (opcode < consensus::OP_PUSHDATA1) {
        size = opcode;
    } else if (opcode == consensus::OP_PUSHDATA1) {
        if (offset >= script.size()) return false;
        size = script[offset++];
    } else if (opcode == consensus::OP_PUSHDATA2) {
        if (script.size() - offset < 2) return false;
        size = static_cast<uint64_t>(script[offset]) |
            (static_cast<uint64_t>(script[offset + 1]) << 8);
        offset += 2;
    } else if (opcode == consensus::OP_PUSHDATA4) {
        if (script.size() - offset < 4) return false;
        size = static_cast<uint64_t>(script[offset]) |
            (static_cast<uint64_t>(script[offset + 1]) << 8) |
            (static_cast<uint64_t>(script[offset + 2]) << 16) |
            (static_cast<uint64_t>(script[offset + 3]) << 24);
        offset += 4;
    }
    if (size > script.size() - offset) return false;
    offset += static_cast<size_t>(size);
    return true;
}

inline bool IsPrematureOpcode(
    uint8_t opcode,
    uint32_t spendHeight,
    const ChainParams& params,
    const char*& name) {
    uint32_t activation = UINT32_MAX;
    switch (opcode) {
        case consensus::OP_CHECKTEMPLATEVERIFY:
            activation = params.ctv_activation_height;
            name = "OP_CHECKTEMPLATEVERIFY";
            break;
        case consensus::OP_CHECKSIGFROMSTACK:
        case consensus::OP_CHECKSIGFROMSTACKVERIFY:
            activation = params.csfs_activation_height;
            name = "OP_CHECKSIGFROMSTACK";
            break;
        case consensus::OP_TXHASH:
            activation = params.txhash_activation_height;
            name = "OP_TXHASH";
            break;
        case consensus::OP_CHECKCONTRACTVERIFY:
            activation = params.ccv_activation_height;
            name = "OP_CHECKCONTRACTVERIFY";
            break;
        default:
            return false;
    }
    return !consensus::CovenantActivationParams::IsActive(
        spendHeight, activation);
}

inline const std::vector<uint8_t>* RevealedTapscript(
    const std::vector<std::vector<uint8_t>>& witness) {
    size_t effectiveItems = witness.size();
    if (effectiveItems >= 2 &&
        !witness.back().empty() &&
        witness.back()[0] == 0x50) {
        --effectiveItems;
    }
    if (effectiveItems < 2) return nullptr;

    // A script-path witness ends in a Taproot control block. Do not classify
    // arbitrary two-item witnesses (notably P2WSH) as Taproot: signature bytes
    // are unconstrained and can coincidentally equal a covenant opcode.
    const auto& controlBlock = witness[effectiveItems - 1];
    if (controlBlock.size() < 33 || controlBlock.size() > 33 + 32 * 128 ||
        (controlBlock.size() - 33) % 32 != 0 ||
        (controlBlock[0] & 0xfe) != 0xc0) {
        return nullptr;
    }
    return &witness[effectiveItems - 2];
}

inline bool IsP2TR(const std::vector<uint8_t>& scriptPubKey) {
    return scriptPubKey.size() == 34 &&
        scriptPubKey[0] == consensus::OP_1 &&
        scriptPubKey[1] == 32;
}

} // namespace detail

/**
 * Relay/mining policy for independently activated Taproot extensions.
 *
 * Consensus retains NOP/OP_SUCCESS semantics before each opcode's activation
 * so historical blocks remain valid. Nodes nevertheless refuse to relay or
 * mine a revealed script that depends on those permissive semantics. This
 * prevents a transaction signed after activation from becoming an
 * anyone-can-spend transaction after a deep reorg below the opcode boundary.
 */
inline bool IsCovenantRelayStandard(
    const Transaction& tx,
    const std::vector<std::vector<uint8_t>>& spentScripts,
    uint32_t spendHeight,
    const ChainParams& params,
    std::string* reason = nullptr) {
    if (spentScripts.size() != tx.vin.size()) {
        if (reason != nullptr) {
            *reason = "covenant relay policy missing spent output scripts";
        }
        return false;
    }

    for (size_t inputIndex = 0; inputIndex < tx.vin.size(); ++inputIndex) {
        if (!detail::IsP2TR(spentScripts[inputIndex])) continue;
        const auto& input = tx.vin[inputIndex];
        const auto* tapscript =
            detail::RevealedTapscript(input.witness);
        if (tapscript == nullptr) continue;

        size_t offset = 0;
        while (offset < tapscript->size()) {
            const uint8_t opcode = (*tapscript)[offset++];
            if (opcode <= consensus::OP_PUSHDATA4) {
                if (!detail::SkipPushData(*tapscript, offset, opcode)) {
                    break; // Canonical script validation reports truncation.
                }
                continue;
            }
            const char* name = nullptr;
            if (detail::IsPrematureOpcode(
                    opcode, spendHeight, params, name)) {
                if (reason != nullptr) {
                    *reason = "premature revealed " + std::string(name) +
                        " at candidate height " +
                        std::to_string(spendHeight);
                }
                return false;
            }
        }
    }
    return true;
}

} // namespace dinero::policy
