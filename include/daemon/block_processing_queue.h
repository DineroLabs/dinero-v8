#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <iostream>
#include <vector>
#include <string>

// Block must be fully defined before including this header!
// Include simple_blockchain.h (or equivalent) before this header.
// Forward declaration won't work because we need complete type for std::queue<Block>

/**
 * Async Block Processing Queue
 * 
 * Decouples block submission (fast RPC response) from block processing (slow validation/storage).
 * This prevents RPC thread deadlocks during mining.
 * 
 * Usage:
 *   auto queue = std::make_shared<BlockProcessingQueue>(blockchain, tx_pool, p2p_manager);
 *   queue->start();  // Start worker thread
 *   
 *   // In submitblock RPC handler:
 *   queue->submit_block(block);  // Returns immediately
 *   
 *   // Cleanup:
 *   queue->stop();
 */
class BlockProcessingQueue {
public:
    using BlockProcessor = std::function<bool(const Block&)>;
    using BlockCallback = std::function<void(const Block&, bool success)>;
    
    BlockProcessingQueue() 
        : running_(false), blocks_processed_(0), blocks_queued_(0) {}
    
    ~BlockProcessingQueue() {
        stop();
    }
    
    /**
     * Set the block processing function
     * This function should validate and add the block to the blockchain
     */
    void set_processor(BlockProcessor processor) {
        processor_ = processor;
    }
    
    /**
     * Set callback for when block processing completes
     * Useful for broadcasting, removing txs from mempool, etc.
     */
    void set_callback(BlockCallback callback) {
        callback_ = callback;
    }
    
    /**
     * Start the worker thread
     */
    void start() {
        if (running_.exchange(true)) {
            return;  // Already running
        }
        
        worker_thread_ = std::thread(&BlockProcessingQueue::worker_loop, this);
        std::cout << "✅ Block processing queue started" << std::endl;
    }
    
    /**
     * Stop the worker thread gracefully
     */
    void stop() {
        if (!running_.exchange(false)) {
            return;  // Already stopped
        }
        
        cv_.notify_all();
        
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        
        std::cout << "✅ Block processing queue stopped (processed " 
                  << blocks_processed_.load() << " blocks)" << std::endl;
    }
    
    /**
     * Submit a block for async processing
     * Returns immediately without blocking
     */
    void submit_block(const Block& block) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push(block);
            blocks_queued_++;
        }
        cv_.notify_one();
        
        std::cout << "📥 Block queued for processing: height=" << block.height 
                  << ", hash=" << block.hash.substr(0, 16) << "..." << std::endl;
    }
    
    /**
     * Get queue statistics
     */
    struct Stats {
        size_t queue_size;
        uint64_t blocks_processed;
        uint64_t blocks_queued;
    };
    
    Stats get_stats() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return {
            queue_.size(),
            blocks_processed_.load(),
            blocks_queued_.load()
        };
    }
    
    /**
     * Check if queue is empty
     */
    bool is_empty() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return queue_.empty();
    }
    
private:
    /**
     * Worker thread loop - processes blocks from queue
     */
    void worker_loop() {
        std::cout << "🔧 Block processing worker thread started (TID: " 
                  << std::this_thread::get_id() << ")" << std::endl;
        
        while (running_) {
            Block block;
            
            // Wait for block to process
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this] { 
                    return !running_ || !queue_.empty(); 
                });
                
                if (!running_) {
                    break;  // Shutdown requested
                }
                
                if (queue_.empty()) {
                    continue;
                }
                
                block = queue_.front();
                queue_.pop();
            }
            
            // Process block (outside of queue mutex!)
            bool success = false;
            
            try {
                if (processor_) {
                    std::cout << "⚙️  Processing block: height=" << block.height 
                              << ", hash=" << block.hash.substr(0, 16) << "..." << std::endl;
                    
                    success = processor_(block);
                    
                    if (success) {
                        std::cout << "✅ Block processed successfully: height=" << block.height << std::endl;
                        blocks_processed_++;
                    } else {
                        std::cerr << "❌ Block processing failed: height=" << block.height << std::endl;
                    }
                } else {
                    std::cerr << "❌ No processor function set!" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "❌ Block processing exception: " << e.what() << std::endl;
            }
            
            // Invoke callback
            if (callback_) {
                try {
                    callback_(block, success);
                } catch (const std::exception& e) {
                    std::cerr << "❌ Block callback exception: " << e.what() << std::endl;
                }
            }
        }
        
        std::cout << "🔧 Block processing worker thread stopped" << std::endl;
    }
    
    std::queue<Block> queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    std::atomic<bool> running_;
    std::atomic<uint64_t> blocks_processed_;
    std::atomic<uint64_t> blocks_queued_;
    
    BlockProcessor processor_;
    BlockCallback callback_;
};
