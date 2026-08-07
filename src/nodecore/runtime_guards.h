#pragma once

#include "nodecore/nodecore_ffi.h"

#include <mutex>
#include <string>
#include <unordered_set>

namespace dinero::nodecore {

struct RuntimeQueryState {
    bool running = false;
    bool shutdown_requested = false;
    bool has_app = false;
};

inline bool IsQueryable(const RuntimeQueryState& state) {
    return state.running && !state.shutdown_requested && state.has_app;
}

struct AuthoritativeTipSnapshot {
    uint64_t height = 0;
    std::string hash;
};

/// Adapt ChainstateService's coherent, mutex-protected sync snapshot for the
/// NodeCore ABI. This deliberately does not accept a ChainDB/storage frontier:
/// during AssumeUTXO the storage tip may lag at genesis while the verified
/// active tip is already the snapshot base. Height and hash must come from the
/// same snapshot so a concurrent block connection cannot produce a torn pair.
template <typename SyncSnapshot>
AuthoritativeTipSnapshot CaptureAuthoritativeTip(const SyncSnapshot& sync) {
    if (!sync.has_active_tip) return {};
    return AuthoritativeTipSnapshot{
        static_cast<uint64_t>(sync.active_tip_height),
        sync.active_tip_hash.GetHex()};
}

struct EventCallbackSnapshot {
    nodecore_event_callback_t callback = nullptr;
    void* user_data = nullptr;
};

class EventCallbackSlot {
public:
    void Set(nodecore_event_callback_t callback, void* user_data) {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = callback;
        user_data_ = user_data;
    }

    EventCallbackSnapshot Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return EventCallbackSnapshot{callback_, user_data_};
    }

private:
    mutable std::mutex mutex_;
    nodecore_event_callback_t callback_ = nullptr;
    void* user_data_ = nullptr;
};

class WatchedScriptRegistry {
public:
    void Add(const std::string& script_hex) {
        std::lock_guard<std::mutex> lock(mutex_);
        scripts_.insert(script_hex);
    }

    void Remove(const std::string& script_hex) {
        std::lock_guard<std::mutex> lock(mutex_);
        scripts_.erase(script_hex);
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        scripts_.clear();
    }

    bool Contains(const std::string& script_hex) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return scripts_.count(script_hex) > 0;
    }

    std::unordered_set<std::string> Snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return scripts_;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_set<std::string> scripts_;
};

}  // namespace dinero::nodecore
