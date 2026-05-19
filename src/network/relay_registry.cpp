// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "network/relay_registry.h"

#include <iomanip>
#include <sstream>
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

}  // namespace

bool RelayRegistry::Register(const RelayRegistration& reg) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = HexNodeId(reg.node_id);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        if (entries_.size() >= kMaxRegistrations) {
            return false;  // full — refuse new entries
        }
        entries_.emplace(key, reg);
    } else {
        // Refresh expiry / replace peer_address / etc. — existing
        // entry can always be updated regardless of cap.
        it->second = reg;
    }
    return true;
}

std::optional<RelayRegistration> RelayRegistry::Lookup(
    const std::array<uint8_t, 20>& node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(HexNodeId(node_id));
    if (it == entries_.end()) return std::nullopt;
    if (std::chrono::steady_clock::now() >= it->second.expires_at) {
        return std::nullopt;  // expired; sweeper will reap in due course
    }
    return it->second;
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
        if (now >= it->second.expires_at) {
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

}  // namespace dinero::network
