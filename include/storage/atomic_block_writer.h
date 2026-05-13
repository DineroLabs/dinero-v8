#pragma once

#include "storage/storage_interface.h"
#include <memory>
#include <string>
#include <vector>

namespace dinero { namespace storage { class BackpressureManager; } }

namespace dinero {
namespace storage {

/**
 * Atomic block writer ensures crash consistency by writing all block-related
 * data in a single atomic batch with tip update last.
 * 
 * Usage:
 *   AtomicBlockWriter writer(storage);
 *   writer.addBlock(hash, block);
 *   writer.addUTXOs(utxos);
 *   writer.removeUTXOs(spent_utxos);
 *   writer.commitWithTip(new_tip_hash, new_height, new_work);
 */
class AtomicBlockWriter {
public:
    explicit AtomicBlockWriter(StorageInterface& storage);
    explicit AtomicBlockWriter(StorageInterface& storage, BackpressureManager* backpressure_manager);
    ~AtomicBlockWriter();
    
    // Add block data to the batch
    void addBlock(const std::string& hash, const Block& block);
    void addBlockHeader(const std::string& hash, const BlockHeader& header);
    void addTransaction(const std::string& hash, const Transaction& tx);
    
    // UTXO operations
    void addUTXO(const std::string& outpoint, const std::vector<uint8_t>& utxo_data);
    void removeUTXO(const std::string& outpoint);
    void addUTXOBatch(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& utxos);
    void removeUTXOBatch(const std::vector<std::string>& outpoints);
    
    // Chain state updates
    void updateChainState(const std::string& key, const std::vector<uint8_t>& value);
    
    // Height index
    void updateHeightIndex(uint32_t height, const std::string& block_hash);
    
    // Phase E.1.c: Atomic commit with tip update (MUST be called last)
    // CRITICAL: Tip commits are ALWAYS synchronous (sync=true hardcoded for safety)
    StorageResult commitWithTip(const std::string& tip_hash, uint32_t tip_height,
                               const std::string& cumulative_work);
    
    // Abort the batch (automatic on destruction if not committed)
    void abort();
    
    // Get batch size for monitoring
    size_t getBatchSize() const;

private:
    StorageInterface& storage_;
    std::unique_ptr<WriteBatch> batch_;
    BackpressureManager* backpressure_manager_;
    bool committed_;
    bool aborted_;
    
    // Prevent copy/move
    AtomicBlockWriter(const AtomicBlockWriter&) = delete;
    AtomicBlockWriter& operator=(const AtomicBlockWriter&) = delete;
};

} // namespace storage
} // namespace dinero
