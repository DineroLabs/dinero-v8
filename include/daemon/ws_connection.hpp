#pragma once
#include <string>
#include <deque>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <mutex>
#include <set> // Added for std::set

namespace dinero {

// WebSocket connection with backpressure protection
class WsConnection {
public:
    static constexpr size_t MAX_QUEUE_BYTES = 2 * 1024 * 1024; // 2MB
    static constexpr size_t FRAME_OVERHEAD = 16; // Approximate WebSocket frame overhead
    
    struct QueuedMessage {
        std::string json;
        std::string channel;
        std::chrono::steady_clock::time_point timestamp;
        size_t frame_size;
    };
    
    explicit WsConnection(int fd) : fd_(fd), bytes_queued_(0), last_ping_(std::chrono::steady_clock::now()) {}
    
    // Enqueue message with backpressure protection
    bool enqueue_message(const std::string& json, const std::string& channel) {
        size_t needed = json.size() + FRAME_OVERHEAD;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Check if we would exceed the queue limit
        if (bytes_queued_ + needed > MAX_QUEUE_BYTES) {
            // For miningInfo, just drop the new one (keep latest)
            if (channel == "miningInfo") {
                return false; // Drop newest miningInfo
            }
            
            // Try to shed non-critical messages first
            if (!shed_noncritical_messages()) {
                // Still over limit, must close connection
                return false; // Will trigger close_for_backpressure
            }
            
            // Check again after shedding
            if (bytes_queued_ + needed > MAX_QUEUE_BYTES) {
                return false; // Will trigger close_for_backpressure
            }
        }
        
        // Add message to queue
        queue_.push_back({json, channel, std::chrono::steady_clock::now(), needed});
        bytes_queued_ += needed;
        
        return true;
    }
    
    // Get next message to send
    bool dequeue_message(QueuedMessage& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        
        msg = std::move(queue_.front());
        queue_.pop_front();
        bytes_queued_ -= msg.frame_size;
        return true;
    }
    
    // Check if queue is empty
    bool is_queue_empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }
    
    // Get current queue size in bytes
    size_t get_queue_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return bytes_queued_;
    }
    
    // Get queue length (number of messages)
    size_t get_queue_length() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
    // Update ping timestamp
    void update_ping() {
        last_ping_ = std::chrono::steady_clock::now();
    }
    
    // Check if ping timeout exceeded
    bool is_ping_timeout() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_ping_);
        return elapsed.count() > 50; // 2 * 25s ping interval
    }
    
    // Get file descriptor
    int get_fd() const { return fd_; }
    
    // Add subscription
    void add_subscription(const std::string& channel) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions_.insert(channel);
    }
    
    // Remove subscription
    void remove_subscription(const std::string& channel) {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriptions_.erase(channel);
    }
    
    // Check if subscribed to channel
    bool is_subscribed(const std::string& channel) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return subscriptions_.find(channel) != subscriptions_.end();
    }
    
    // Get subscription count
    size_t get_subscription_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return subscriptions_.size();
    }
    
    // Mark connection for removal (e.g., due to backpressure)
    void mark_for_removal() {
        std::lock_guard<std::mutex> lock(mutex_);
        marked_for_removal_ = true;
    }
    
    // Check if marked for removal
    bool is_marked_for_removal() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return marked_for_removal_;
    }

private:
    // Try to shed non-critical messages to make room
    bool shed_noncritical_messages() {
        // Remove oldest miningInfo messages first (they're lossy)
        auto it = queue_.begin();
        while (it != queue_.end() && bytes_queued_ > MAX_QUEUE_BYTES * 3 / 4) {
            if (it->channel == "miningInfo") {
                bytes_queued_ -= it->frame_size;
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
        
        return bytes_queued_ <= MAX_QUEUE_BYTES * 3 / 4;
    }
    
    int fd_;
    std::deque<QueuedMessage> queue_;
    std::set<std::string> subscriptions_;
    size_t bytes_queued_;
    std::chrono::steady_clock::time_point last_ping_;
    mutable std::mutex mutex_;
    bool marked_for_removal_ = false;
};

} // namespace dinero
