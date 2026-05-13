#pragma once

#include "consensus/outpoint.h"  // Phase M.0: Single canonical OutPoint
#include "primitives/amount.h"   // Phase M.6.2: Monetary type safety
#include <cstdint>
#include <vector>
#include <string>

namespace dinero {
namespace consensus {

/**
 * Phase 23.0: UTXOEntry - Represents a single unspent transaction output
 *
 * This is the fundamental unit of the UTXO set, similar to Bitcoin Core's Coin class.
 * Each entry contains everything needed to validate a spend:
 * - Output value (in una)
 * - ScriptPubKey (locking script)
 * - Block height (for coinbase maturity and BIP68 sequence locks)
 * - Coinbase flag (100-block maturity rule)
 * - Confidential flag and commitment (for ZK privacy)
 */
struct UTXOEntry {
    // Phase M.6.2: Output value wrapped in AmountUna for type safety
    AmountUna value;

    // Locking script (scriptPubKey)
    std::vector<uint8_t> scriptPubKey;

    // Block height where this UTXO was created
    // Used for:
    // - Coinbase maturity (must wait 100 blocks)
    // - BIP68 sequence locks (relative locktime)
    uint32_t height;

    // Is this a coinbase output?
    // Coinbase outputs require 100 confirmations before spending
    bool isCoinbase;

    // ═══════════════════════════════════════════════════════════════════════
    // Confidential Transaction Support (Dragon #1 Fix)
    // ═══════════════════════════════════════════════════════════════════════
    // These fields MUST be preserved during reorgs to prevent:
    // - Commitment loss (confidential → transparent corruption)
    // - Balance verification failures
    // - Consensus splits between reorged/non-reorged nodes
    // ═══════════════════════════════════════════════════════════════════════

    // Is this a confidential output?
    bool is_confidential = false;

    // Pedersen commitment (typically a 33-byte compressed point)
    // Hides the actual value: C = value*H + blinding*G
    std::vector<uint8_t> commitment;

    // Default constructor
    UTXOEntry()
        : value(AmountUna::Zero())
        , height(0)
        , isCoinbase(false)
        , is_confidential(false)
    {}

    // Phase M.6.2: Constructor now takes AmountUna parameter
    UTXOEntry(AmountUna val, const std::vector<uint8_t>& script, uint32_t h, bool coinbase)
        : value(val)
        , scriptPubKey(script)
        , height(h)
        , isCoinbase(coinbase)
        , is_confidential(false)
    {}

    // Constructor with confidential data
    UTXOEntry(AmountUna val, const std::vector<uint8_t>& script, uint32_t h, bool coinbase,
              bool confidential, const std::vector<uint8_t>& commit)
        : value(val)
        , scriptPubKey(script)
        , height(h)
        , isCoinbase(coinbase)
        , is_confidential(confidential)
        , commitment(commit)
    {}

    // Check if this UTXO is mature (spendable)
    // Coinbase outputs require 100 confirmations
    // Regular outputs are immediately spendable
    bool isMature(uint32_t current_height) const {
        if (!isCoinbase) {
            return true;  // Non-coinbase UTXOs are always mature
        }

        // Coinbase maturity: must have 100 blocks on top
        // (101 total confirmations including the block itself)
        return (current_height >= height + 100);
    }

    // Get the size in bytes when serialized
    size_t serializedSize() const {
        // 8 bytes (value) + 4 bytes (height) + 1 byte (flags) + varint(script_len) + script
        size_t size = 8 + 4 + 1;  // Fixed fields

        // Script length varint
        if (scriptPubKey.size() < 253) {
            size += 1;
        } else if (scriptPubKey.size() <= 0xFFFF) {
            size += 3;
        } else {
            size += 5;
        }

        size += scriptPubKey.size();

        // Confidential data is encoded behind bit 1 in the flags byte.
        if (is_confidential && !commitment.empty()) {
            if (commitment.size() < 253) {
                size += 1;
            } else if (commitment.size() <= 0xFFFF) {
                size += 3;
            } else {
                size += 5;
            }
            size += commitment.size();
        }

        return size;
    }

    // Check if this is a confidential UTXO
    bool IsConfidential() const { return is_confidential; }

    // Get commitment (empty if not confidential)
    const std::vector<uint8_t>& GetCommitment() const { return commitment; }
};

// Phase M.0: OutPoint now defined in consensus/outpoint.h (uint256-based)

/**
 * UndoCoins - Stores UTXOs that were spent in a block
 * Used for block disconnection during reorgs
 *
 * When applying a block:
 * - Spend UTXOs → save to UndoCoins
 * - Create UTXOs → add to UTXO set
 *
 * When undoing a block (reorg):
 * - Delete UTXOs created by this block
 * - Restore UTXOs spent by this block (from UndoCoins)
 */
struct UndoCoins {
    // Map: OutPoint → UTXOEntry
    // Stores all UTXOs spent in this block
    std::vector<std::pair<OutPoint, UTXOEntry>> spent_coins;

    // Add a spent coin to undo data
    void addSpentCoin(const OutPoint& outpoint, const UTXOEntry& coin) {
        spent_coins.emplace_back(outpoint, coin);
    }

    // Get the number of spent coins
    size_t size() const {
        return spent_coins.size();
    }

    // Clear all undo data
    void clear() {
        spent_coins.clear();
    }
};

} // namespace consensus
} // namespace dinero

// Phase M.0: Hash function for OutPoint now in consensus/outpoint.h (no longer needed here)
