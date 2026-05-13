#pragma once
#include <functional>
#include <chrono>
#include <random>
#include <thread>
#include "compat/jsoncpp_compat.h"

namespace dinero::cli {

// Retry helper with jittered exponential backoff
template<typename T>
class RetryHelper {
private:
    int maxRetries_;
    int timeoutMs_;
    std::mt19937 rng_;
    
public:
    RetryHelper(int maxRetries, int timeoutMs) 
        : maxRetries_(maxRetries), timeoutMs_(timeoutMs), rng_(std::random_device{}()) {}
    
    // Execute function with retries and exponential backoff
    // Returns pair<success, result_or_error>
    template<typename Func>
    std::pair<bool, T> execute(Func&& func) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs_);
        
        for (int attempt = 0; attempt <= maxRetries_; ++attempt) {
            if (std::chrono::steady_clock::now() >= deadline) {
                break; // Timeout exceeded
            }
            
            auto result = func();
            if (result.first) { // Success
                return result;
            }
            
            // Last attempt - don't sleep
            if (attempt == maxRetries_) {
                return result;
            }
            
            // Jittered exponential backoff: 250ms * 2^attempt ± 10%
            int baseDelayMs = 250 * (1 << attempt);
            std::uniform_int_distribution<int> jitter(-baseDelayMs / 10, baseDelayMs / 10);
            int delayMs = baseDelayMs + jitter(rng_);
            
            auto sleepUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
            if (sleepUntil > deadline) {
                sleepUntil = deadline;
            }
            
            std::this_thread::sleep_until(sleepUntil);
        }
        
        return {false, T{}}; // All retries exhausted
    }
};

// Wait for daemon readiness within timeoutSec. Returns ok + human message.
std::pair<bool, std::string> waitForReady(
    std::function<std::pair<bool, Json::Value>(const std::string&, const Json::Value&)> rpcCall,
    int timeoutSec, 
    int retriesPerProbe = 1
);

} // namespace dinero::cli
