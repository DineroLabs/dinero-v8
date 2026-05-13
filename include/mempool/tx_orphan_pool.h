#pragma once

#include "primitives/uint256.h"
#include "wallet/transaction.h"
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace dinero {

struct OrphanEntry {
    Transaction tx;
    uint256 txid;
    std::string peer_id;
    std::chrono::steady_clock::time_point expiry;
    size_t tx_size;
};

class TxOrphanPool {
public:
    TxOrphanPool() = default;

    // Add an orphan transaction. Returns true if added, false if rejected.
    bool addOrphan(const Transaction& tx, const std::string& peer_id);

    // Remove a specific orphan by txid.
    void eraseOrphan(const uint256& txid);

    // Remove all orphans sent by a specific peer.
    void eraseOrphansForPeer(const std::string& peer_id);

    // Get orphan transactions whose missing parent matches parent_txid.
    std::vector<Transaction> getOrphansForParent(const uint256& parent_txid);

    // Check if a txid is already in the orphan pool.
    bool hasOrphan(const uint256& txid) const;

    // Remove expired orphans. Call periodically. Returns count removed.
    size_t expireOldOrphans();

    // Pool size.
    size_t size() const;

    // Get all orphan txids (for RPC).
    std::vector<uint256> getOrphanTxIds() const;

    // Per-peer counts (for RPC diagnostics).
    std::unordered_map<std::string, size_t> getPeerOrphanCounts() const;

    // Limits
    static constexpr size_t MAX_ORPHAN_TRANSACTIONS = 100;
    static constexpr size_t MAX_ORPHAN_TX_SIZE = 100000;  // 100 KB
    static constexpr size_t MAX_ORPHANS_PER_PEER = 5;
    static constexpr auto ORPHAN_TX_EXPIRE_TIME = std::chrono::minutes(20);

private:
    // Evict a random orphan to make room. Returns true if one was evicted.
    bool evictRandom();

    // Remove orphan internal (caller holds lock).
    void eraseOrphanLocked(const uint256& txid);

    mutable std::mutex m_mutex;
    std::unordered_map<uint256, OrphanEntry> m_orphans;
    // Maps a prevout txid to the set of orphan txids that spend it
    std::unordered_map<uint256, std::set<uint256>> m_outpoint_to_orphans;
    std::unordered_map<std::string, size_t> m_peer_orphan_count;
};

} // namespace dinero
