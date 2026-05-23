// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/relay_registry.h"

#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace dinero::network {

namespace {

std::string HexNodeId(const std::array<uint8_t, 20>& id) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : id) {
        oss << std::setw(2) << static_cast<unsigned int>(b);
    }
    return oss.str();
}

bool IsGracePending(const RelayRegistration& reg,
                    std::chrono::steady_clock::time_point now) {
    return now < reg.expires_at &&
           reg.grace_expires_at !=
               std::chrono::steady_clock::time_point::max() &&
           now < reg.grace_expires_at;
}

bool IsExpiredOrGraceExpired(const RelayRegistration& reg,
                             std::chrono::steady_clock::time_point now) {
    if (now >= reg.expires_at) {
        return true;
    }
    if (reg.grace_expires_at !=
            std::chrono::steady_clock::time_point::max() &&
        now >= reg.grace_expires_at) {
        return true;
    }
    return false;
}

bool IsUsable(const RelayRegistration& reg,
              std::chrono::steady_clock::time_point now) {
    return !IsExpiredOrGraceExpired(reg, now) && !IsGracePending(reg, now);
}

}  // namespace

bool RelayRegistry::Register(const RelayRegistration& reg) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = HexNodeId(reg.node_id);
    RelayRegistration normalized = reg;
    normalized.grace_expires_at =
        std::chrono::steady_clock::time_point::max();
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        if (entries_.size() >= kMaxRegistrations) {
            return false;  // full — refuse new entries
        }
        entries_.emplace(key, std::move(normalized));
    } else {
        // Refresh expiry / replace peer_address / etc. — existing
        // entry can always be updated regardless of cap.
        it->second = std::move(normalized);
    }
    return true;
}

std::optional<RelayRegistration> RelayRegistry::Lookup(
    const std::array<uint8_t, 20>& node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(HexNodeId(node_id));
    if (it == entries_.end()) return std::nullopt;
    if (!IsUsable(it->second, std::chrono::steady_clock::now())) {
        return std::nullopt;  // stale or grace-pending; sweeper will reap later
    }
    return it->second;
}

std::vector<RelayRegistration> RelayRegistry::SnapshotValid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    std::vector<RelayRegistration> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        if (IsUsable(entry.second, now)) {
            out.push_back(entry.second);
        }
    }
    return out;
}

size_t RelayRegistry::MarkGracePendingByPeerAddress(
    const std::string& peer_address,
    std::chrono::steady_clock::time_point grace_until) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    size_t marked = 0;
    for (auto& [_, reg] : entries_) {
        if (reg.peer_address != peer_address) {
            continue;
        }
        if (now >= reg.expires_at) {
            continue;
        }
        reg.grace_expires_at = grace_until;
        ++marked;
    }
    return marked;
}

bool RelayRegistry::MarkGracePending(
    const std::array<uint8_t, 20>& node_id,
    std::chrono::steady_clock::time_point grace_until) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(HexNodeId(node_id));
    if (it == entries_.end()) {
        return false;
    }
    if (std::chrono::steady_clock::now() >= it->second.expires_at) {
        return false;
    }
    it->second.grace_expires_at = grace_until;
    return true;
}

void RelayRegistry::UnregisterByPeerAddress(const std::string& peer_address) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Linear scan — registry is capped at 100 entries so this is
    // bounded and rare (per-connection-close path).
    std::vector<std::string> to_remove;
    for (const auto& [key, reg] : entries_) {
        if (reg.peer_address == peer_address) {
            to_remove.push_back(key);
        }
    }
    for (const auto& key : to_remove) {
        entries_.erase(key);
    }
}

size_t RelayRegistry::Sweep() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    size_t removed = 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (IsExpiredOrGraceExpired(it->second, now)) {
            it = entries_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

size_t RelayRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

size_t RelayRegistry::grace_pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    size_t count = 0;
    for (const auto& [_, reg] : entries_) {
        if (IsGracePending(reg, now)) {
            ++count;
        }
    }
    return count;
}

}  // namespace dinero::network
