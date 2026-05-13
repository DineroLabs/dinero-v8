#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <string>
#include "primitives/uint256.h"

namespace dinero {

// Represents a spent UTXO that needs to be restored during block disconnection
// Phase M.0: prev_txid is uint256 (canonical identity), not hex string
struct SpentCoin {
    uint256 prev_txid;          // Transaction ID (Phase M.0: uint256 identity)
    uint32_t prev_vout;         // Output index
    uint64_t value;             // Amount in una
    std::vector<uint8_t> scriptPubKey;  // Script public key (binary, not hex)
    bool is_coinbase;           // True if this was a coinbase output
    uint32_t height;            // Block height where UTXO was created
    bool is_confidential = false;       // True if the spent UTXO was confidential
    std::vector<uint8_t> commitment;    // 33-byte Pedersen commitment (if confidential)

    SpentCoin() = default;
    SpentCoin(const uint256& txid_, uint32_t vout_, uint64_t value_,
             const std::vector<uint8_t>& scriptPubKey_, bool is_coinbase_, uint32_t height_,
             bool is_confidential_ = false,
             const std::vector<uint8_t>& commitment_ = {})
        : prev_txid(txid_), prev_vout(vout_), value(value_),
          scriptPubKey(scriptPubKey_), is_coinbase(is_coinbase_), height(height_),
          is_confidential(is_confidential_), commitment(commitment_) {}
};

// Represents an output created by this block (for deletion during disconnect)
// Phase M.0: txid is uint256 (canonical identity), not hex string
struct CreatedOut {
    uint256 txid;               // Transaction ID (Phase M.0: uint256 identity)
    uint32_t vout;              // Output index

    CreatedOut() = default;
    CreatedOut(const uint256& txid_, uint32_t vout_)
        : txid(txid_), vout(vout_) {}
};

// Undo record for a single block - stores all state changes needed to reverse block application
// Storage: RocksDB key "U:<blockhash>" → serialized UndoRecord
struct UndoRecord {
    std::vector<SpentCoin> spent;       // All UTXOs spent by this block (restore on disconnect)
    std::vector<CreatedOut> created;    // All UTXOs created by this block (delete on disconnect)
    // v7 shielded rollback snapshot for archival undo. Mirrors BlockUndo's
    // pre_block_shielded_frontier so flatfile-backed reorg paths can restore
    // the exact pre-block shielded frontier after reindex-built blocks.
    std::optional<std::vector<uint8_t>> pre_block_shielded_frontier;

    UndoRecord() = default;

    // Serialize to binary format for RocksDB storage
    std::vector<uint8_t> Serialize() const;

    // Deserialize from binary format
    static UndoRecord Deserialize(const std::vector<uint8_t>& data);
};

} // namespace dinero
