#include "storage/atomic_block_writer.h"
#include "storage/backpressure_manager.h"
#include "storage/storage_metrics.h"
#include <iostream>

namespace dinero {
namespace storage {

AtomicBlockWriter::AtomicBlockWriter(StorageInterface& storage) 
    : storage_(storage), backpressure_manager_(nullptr), committed_(false), aborted_(false) {
    batch_ = storage_.createWriteBatch();
    if (!batch_) {
        throw std::runtime_error("Failed to create write batch");
    }
}

AtomicBlockWriter::AtomicBlockWriter(StorageInterface& storage, BackpressureManager* backpressure_manager)
    : storage_(storage), backpressure_manager_(backpressure_manager), committed_(false), aborted_(false) {
    batch_ = storage_.createWriteBatch();
    if (!batch_) {
        throw std::runtime_error("Failed to create write batch");
    }
}

AtomicBlockWriter::~AtomicBlockWriter() {
    if (!committed_ && !aborted_) {
        abort();
    }
}

void AtomicBlockWriter::addBlock(const std::string& hash, const Block& block) {
    if (committed_ || aborted_) {
        throw std::runtime_error("Cannot add to committed/aborted batch");
    }
    batch_->putBlock(hash, block);
}

void AtomicBlockWriter::addBlockHeader(const std::string& hash, const BlockHeader& header) {
    if (committed_ || aborted_) {
        throw std::runtime_error("Cannot add to committed/aborted batch");
    }
    // TODO: Implement block header storage in WriteBatch interface
    // batch_->putBlockHeader(hash, header);
}

void AtomicBlockWriter::addTransaction(const std::string& hash, const Transaction& tx) {
    if (committed_ || aborted_) {
        throw std::runtime_error("Cannot add to committed/aborted batch");
    }
    batch_->putTransaction(hash, tx);
}

void AtomicBlockWriter::addUTXO(const std::string& outpoint, const std::vector<uint8_t>& utxo_data) {
    if (committed_ || aborted_) {
        throw std::runtime_error("Cannot add to committed/aborted batch");
    }
    batch_->putUTXO(outpoint, utxo_data);
}

void AtomicBlockWriter::removeUTXO(const std::string& outpoint) {
    if (committed_ || aborted_) {
        throw std::runtime_error("Cannot remove from committed/aborted batch");
    }
    batch_->deleteUTXO(outpoint);
}

void AtomicBlockWriter::addUTXOBatch(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& utxos) {
    if (committed_ || aborted_) {
        throw std::runtime_error("Cannot add to committed/aborted batch");
    }
    for (const auto& [outpoint, utxo_data] : utxos) {
        batch_->putUTXO(outpoint, utxo_data);
    }
}

void AtomicBlockWriter::removeUTXOBatch(const std::vector<std::string>& outpoints) {
    if (committed_ || aborted_) {
        throw std::runtime_error("Cannot remove from committed/aborted batch");
    }
    for (const auto& outpoint : outpoints) {
        batch_->deleteUTXO(outpoint);
    }
}

void AtomicBlockWriter::updateChainState(const std::string& key, const std::vector<uint8_t>& value) {
    if (committed_ || aborted_) {
        throw std::runtime_error("Cannot update committed/aborted batch");
    }
    batch_->putChainState(key, value);
}

void AtomicBlockWriter::updateHeightIndex(uint32_t height, const std::string& block_hash) {
    if (committed_ || aborted_) {
        throw std::runtime_error("Cannot update committed/aborted batch");
    }
    
    // Encode height as big-endian 4-byte key for proper ordering
    std::vector<uint8_t> height_key(4);
    height_key[0] = (height >> 24) & 0xFF;
    height_key[1] = (height >> 16) & 0xFF;
    height_key[2] = (height >> 8) & 0xFF;
    height_key[3] = height & 0xFF;
    
    std::vector<uint8_t> hash_value(block_hash.begin(), block_hash.end());
    
    std::string key_str(height_key.begin(), height_key.end());
    batch_->putChainState("height:" + key_str, hash_value);
}

StorageResult AtomicBlockWriter::commitWithTip(const std::string& tip_hash, uint32_t tip_height,
                                              const std::string& cumulative_work) {
    if (committed_ || aborted_) {
        return StorageResult::INVALID_OPERATION;
    }

    // Apply backpressure if manager is configured
    if (backpressure_manager_) {
        if (!backpressure_manager_->checkAndApplyBackpressure()) {
            std::cerr << "BACKPRESSURE: Block commit rejected due to resource constraints" << std::endl;
            return StorageResult::RESOURCE_EXHAUSTED;
        }
    }

    // Add tip update LAST to ensure atomicity
    // If crash happens before tip update, block is not considered applied
    std::vector<uint8_t> tip_data;

    // Serialize tip info: hash + height + work
    tip_data.insert(tip_data.end(), tip_hash.begin(), tip_hash.end());

    // Add height as 4-byte big-endian
    tip_data.push_back((tip_height >> 24) & 0xFF);
    tip_data.push_back((tip_height >> 16) & 0xFF);
    tip_data.push_back((tip_height >> 8) & 0xFF);
    tip_data.push_back(tip_height & 0xFF);

    // Add cumulative work
    tip_data.insert(tip_data.end(), cumulative_work.begin(), cumulative_work.end());

    batch_->putChainState("tip", tip_data);

    // Phase E.1.c: CRITICAL - Tip commits are ALWAYS synchronous (sync=true)
    // Without fsync, tip can be lost on power failure → corrupted chain state
    // This is hardcoded to true - tip durability is non-negotiable
    constexpr bool sync = true;
    StorageTimer timer;
    StorageResult result = batch_->commitSync(sync);
    uint64_t commit_latency = timer.elapsedMicros();

    // Record metrics
    if (g_storage_metrics) {
        g_storage_metrics->recordBatchCommit(commit_latency, result == StorageResult::SUCCESS,
                                           batch_->size(), sync);
        g_storage_metrics->recordTipUpdate(commit_latency, result == StorageResult::SUCCESS, sync);
    }

    if (result == StorageResult::SUCCESS) {
        committed_ = true;
        std::cout << "Block committed atomically: tip=" << tip_hash
                  << " height=" << tip_height
                  << " sync=true (ENFORCED)"
                  << " batch_size=" << batch_->size()
                  << " latency=" << commit_latency << "μs" << std::endl;
    } else {
        std::cerr << "Failed to commit atomic block batch: " << static_cast<int>(result) << std::endl;
        if (g_storage_metrics) {
            g_storage_metrics->recordError("batch_commit", "commit_failed");
        }
    }

    return result;
}

void AtomicBlockWriter::abort() {
    if (!aborted_ && !committed_) {
        size_t batch_size = batch_->size();
        batch_->clear();
        aborted_ = true;
        
        // Record metrics for aborted batch
        if (g_storage_metrics) {
            g_storage_metrics->recordBatchAbort(batch_size);
        }
        
        std::cout << "Atomic block batch aborted (size=" << batch_size << ")" << std::endl;
    }
}

size_t AtomicBlockWriter::getBatchSize() const {
    return batch_ ? batch_->size() : 0;
}

} // namespace storage
} // namespace dinero
