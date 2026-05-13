#include "consensus/sigops.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "util/hex.h"
#include "common/logger.h"
#include <algorithm>

// Use dinero::Transaction from wallet/transaction.h
using Transaction = dinero::Transaction;
using TxInput = dinero::TxInput;
using TxOutput = dinero::TxOutput;

namespace dinero {
namespace consensus {

// ═══════════════════════════════════════════════════════════════════════════
// Legacy Sigop Counting
// ═══════════════════════════════════════════════════════════════════════════

unsigned int GetLegacySigOpCount(const std::vector<uint8_t>& script, bool accurate) {
    unsigned int sigops = 0;
    const uint8_t* pc = script.data();
    const uint8_t* end = pc + script.size();

    while (pc < end) {
        uint8_t opcode = *pc;
        pc++;

        // OP_CHECKSIG and OP_CHECKSIGVERIFY
        if (opcode == OP_CHECKSIG || opcode == OP_CHECKSIGVERIFY) {
            sigops++;
        }
        // OP_CHECKMULTISIG and OP_CHECKMULTISIGVERIFY
        else if (opcode == OP_CHECKMULTISIG || opcode == OP_CHECKMULTISIGVERIFY) {
            if (accurate && pc > script.data() + 1) {
                // Try to extract the n value from previous opcode
                const uint8_t* prev_pc = pc - 2;
                if (prev_pc >= script.data()) {
                    uint8_t n_opcode = *prev_pc;
                    // OP_1 through OP_16 encode values 1-16
                    if (n_opcode >= OP_1 && n_opcode <= (OP_1 + 15)) {
                        unsigned int n = n_opcode - OP_1 + 1;
                        sigops += std::min(n, MAX_PUBKEYS_PER_MULTISIG);
                    } else {
                        // Worst case: assume MAX_PUBKEYS_PER_MULTISIG
                        sigops += MAX_PUBKEYS_PER_MULTISIG;
                    }
                } else {
                    sigops += MAX_PUBKEYS_PER_MULTISIG;
                }
            } else {
                // Worst case: assume MAX_PUBKEYS_PER_MULTISIG
                sigops += MAX_PUBKEYS_PER_MULTISIG;
            }
        }
        // Handle push opcodes (skip pushed data)
        else if (opcode > 0 && opcode <= 75) {
            // Direct push of N bytes
            pc += opcode;
        }
        else if (opcode == 0x4c) {  // OP_PUSHDATA1
            if (pc < end) {
                uint8_t len = *pc;
                pc += 1 + len;
            }
        }
        else if (opcode == 0x4d) {  // OP_PUSHDATA2
            if (pc + 1 < end) {
                uint16_t len = pc[0] | (pc[1] << 8);
                pc += 2 + len;
            }
        }
        else if (opcode == 0x4e) {  // OP_PUSHDATA4
            if (pc + 3 < end) {
                uint32_t len = pc[0] | (pc[1] << 8) | (pc[2] << 16) | (pc[3] << 24);
                pc += 4 + len;
            }
        }
    }

    return sigops;
}

unsigned int GetLegacySigOpCount(const std::string& script_hex, bool accurate) {
    std::vector<uint8_t> script = util::HexToBytes(script_hex);
    return GetLegacySigOpCount(script, accurate);
}

// ═══════════════════════════════════════════════════════════════════════════
// Witness Script Type Detection
// ═══════════════════════════════════════════════════════════════════════════

bool IsP2WPKH(const std::vector<uint8_t>& script) {
    // P2WPKH: OP_0 <20 bytes>
    return script.size() == 22 &&
           script[0] == OP_0 &&
           script[1] == 20;
}

bool IsP2WSH(const std::vector<uint8_t>& script) {
    // P2WSH: OP_0 <32 bytes>
    return script.size() == 34 &&
           script[0] == OP_0 &&
           script[1] == 32;
}

bool IsP2TR(const std::vector<uint8_t>& script) {
    // P2TR: OP_1 <32 bytes>
    return script.size() == 34 &&
           script[0] == OP_1 &&
           script[1] == 32;
}

// ═══════════════════════════════════════════════════════════════════════════
// Witness Sigop Counting
// ═══════════════════════════════════════════════════════════════════════════

unsigned int GetWitnessSigOpCost(const Transaction& tx,
                                  const std::vector<std::vector<uint8_t>>& utxo_scripts) {
    // Check if transaction has witness data (witness_version != 0xFF)
    if (tx.witness_version == 0xFF) {
        return 0;  // Legacy transactions have no witness sigops
    }

    unsigned int sigop_cost = 0;

    for (size_t i = 0; i < tx.vin.size() && i < utxo_scripts.size(); i++) {
        const auto& input = tx.vin[i];
        const auto& prev_script = utxo_scripts[i];

        // P2WPKH: Always 1 sigop
        if (IsP2WPKH(prev_script)) {
            sigop_cost += 1;
        }
        // P2WSH: Count sigops in witness script
        else if (IsP2WSH(prev_script)) {
            if (!input.witness.empty()) {
                // Last witness item is the script
                const auto& witness_script = input.witness.back();
                unsigned int witness_sigops = GetLegacySigOpCount(witness_script, true);
                sigop_cost += witness_sigops;
            }
        }
        // P2TR (Taproot): No sigops counted (uses different validation)
        else if (IsP2TR(prev_script)) {
            // Taproot transactions don't contribute to sigop count
            // (uses schnorr signature batching with different limits)
            continue;
        }
    }

    return sigop_cost;
}

// ═══════════════════════════════════════════════════════════════════════════
// Transaction Sigop Cost
// ═══════════════════════════════════════════════════════════════════════════

unsigned int GetTransactionSigOpCost(const Transaction& tx,
                                      const std::vector<std::vector<uint8_t>>& utxo_scripts) {
    unsigned int sigop_cost = 0;

    // Count legacy sigops in inputs
    for (const auto& input : tx.vin) {
        // Count sigops in scriptSig (already a vector<uint8_t>)
        sigop_cost += GetLegacySigOpCount(input.scriptSig, false);
    }

    // Count legacy sigops in outputs
    for (const auto& output : tx.vout) {
        // Count sigops in scriptPubKey (already a vector<uint8_t>)
        sigop_cost += GetLegacySigOpCount(output.scriptPubKey, true);
    }

    // Count witness sigops (if UTXO scripts provided)
    if (!utxo_scripts.empty()) {
        unsigned int witness_sigops = GetWitnessSigOpCost(tx, utxo_scripts);
        // Witness sigops are not scaled in the cost calculation
        // (the 4x scaling is for weight, not sigop count)
        sigop_cost += witness_sigops;
    }

    return sigop_cost;
}

// ═══════════════════════════════════════════════════════════════════════════
// Block Sigop Validation
// ═══════════════════════════════════════════════════════════════════════════

bool CheckBlockSigops(const dinero::Block& block,
                       unsigned int& sigop_cost,
                       std::string& error) {
    sigop_cost = 0;

    // Iterate through all transactions in the block
    for (const auto& tx : block.vtx) {
        // For block validation, we don't have UTXO scripts readily available,
        // so we count only legacy sigops (conservative approach)
        // Full nodes should maintain UTXO set for accurate witness sigop counting
        unsigned int tx_sigops = GetTransactionSigOpCost(tx, {});

        sigop_cost += tx_sigops;

        // Early exit if we exceed the limit
        if (sigop_cost > MAX_BLOCK_SIGOPS_COST) {
            error = "Block exceeds maximum signature operations: " +
                    std::to_string(sigop_cost) + " > " + std::to_string(MAX_BLOCK_SIGOPS_COST);
            dinero::g_logger.warning("Block sigops validation failed: " + error);
            return false;
        }
    }

    // Log successful validation
    if (sigop_cost > MAX_BLOCK_SIGOPS_COST * 0.8) {
        // Warn if block is using >80% of sigop budget
        dinero::g_logger.warning("Block using high sigop count: " + std::to_string(sigop_cost) +
                                " / " + std::to_string(MAX_BLOCK_SIGOPS_COST) +
                                " (" + std::to_string((sigop_cost * 100) / MAX_BLOCK_SIGOPS_COST) + "%)");
    }

    return true;
}

bool CheckTransactionSigops(const dinero::Transaction& tx,
                             unsigned int& sigop_cost,
                             std::string& error) {
    sigop_cost = GetTransactionSigOpCost(tx, {});

    if (sigop_cost > MAX_TX_SIGOPS_COST) {
        error = "Transaction exceeds maximum signature operations: " +
                std::to_string(sigop_cost) + " > " + std::to_string(MAX_TX_SIGOPS_COST);
        dinero::g_logger.debug("Transaction sigops validation failed: " + error);
        return false;
    }

    return true;
}

} // namespace consensus
} // namespace dinero
