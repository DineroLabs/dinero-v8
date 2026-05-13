#include "mempool/tx_orphan_pool.h"
#include "common/logger.h"
#include <algorithm>
#include <random>

namespace dinero {

bool TxOrphanPool::addOrphan(const Transaction& tx, const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    uint256 txid = tx.GetTxid().AsUint256();

    // Already have this orphan
    if (m_orphans.count(txid)) {
        return false;
    }

    // Check TX size limit
    size_t tx_size = tx.Serialize().size() / 2;  // Hex string / 2 = bytes
    if (tx_size > MAX_ORPHAN_TX_SIZE) {
        g_logger.debug("[OrphanPool] Rejected oversized orphan " + txid.GetHex().substr(0, 16) +
                      "... (" + std::to_string(tx_size) + " bytes)");
        return false;
    }

    // Per-peer limit
    if (m_peer_orphan_count[peer_id] >= MAX_ORPHANS_PER_PEER) {
        g_logger.debug("[OrphanPool] Per-peer limit reached for " + peer_id);
        return false;
    }

    // Pool full — evict a random orphan
    if (m_orphans.size() >= MAX_ORPHAN_TRANSACTIONS) {
        evictRandom();
    }

    // Build orphan entry
    OrphanEntry entry;
    entry.tx = tx;
    entry.txid = txid;
    entry.peer_id = peer_id;
    entry.expiry = std::chrono::steady_clock::now() + ORPHAN_TX_EXPIRE_TIME;
    entry.tx_size = tx_size;

    // Index by missing parent prevout txids
    for (const auto& input : tx.vin) {
        uint256 prevout_txid = input.prevout.txid.AsUint256();
        m_outpoint_to_orphans[prevout_txid].insert(txid);
    }

    m_orphans[txid] = std::move(entry);
    m_peer_orphan_count[peer_id]++;

    g_logger.debug("[OrphanPool] Added orphan " + txid.GetHex().substr(0, 16) +
                  "... from " + peer_id + " (pool size: " + std::to_string(m_orphans.size()) + ")");
    return true;
}

void TxOrphanPool::eraseOrphanLocked(const uint256& txid) {
    auto it = m_orphans.find(txid);
    if (it == m_orphans.end()) return;

    const auto& entry = it->second;

    // Remove from parent index
    for (const auto& input : entry.tx.vin) {
        uint256 prevout_txid = input.prevout.txid.AsUint256();
        auto parent_it = m_outpoint_to_orphans.find(prevout_txid);
        if (parent_it != m_outpoint_to_orphans.end()) {
            parent_it->second.erase(txid);
            if (parent_it->second.empty()) {
                m_outpoint_to_orphans.erase(parent_it);
            }
        }
    }

    // Decrement per-peer count
    auto peer_it = m_peer_orphan_count.find(entry.peer_id);
    if (peer_it != m_peer_orphan_count.end()) {
        if (peer_it->second > 0) peer_it->second--;
        if (peer_it->second == 0) m_peer_orphan_count.erase(peer_it);
    }

    m_orphans.erase(it);
}

void TxOrphanPool::eraseOrphan(const uint256& txid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    eraseOrphanLocked(txid);
}

void TxOrphanPool::eraseOrphansForPeer(const std::string& peer_id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<uint256> to_erase;
    for (const auto& [txid, entry] : m_orphans) {
        if (entry.peer_id == peer_id) {
            to_erase.push_back(txid);
        }
    }
    for (const auto& txid : to_erase) {
        eraseOrphanLocked(txid);
    }
}

std::vector<Transaction> TxOrphanPool::getOrphansForParent(const uint256& parent_txid) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<Transaction> result;
    auto it = m_outpoint_to_orphans.find(parent_txid);
    if (it == m_outpoint_to_orphans.end()) {
        return result;
    }

    // Copy the set since we'll be modifying it during erasure
    auto orphan_txids = it->second;
    for (const auto& orphan_txid : orphan_txids) {
        auto orphan_it = m_orphans.find(orphan_txid);
        if (orphan_it != m_orphans.end()) {
            result.push_back(orphan_it->second.tx);
        }
    }

    return result;
}

bool TxOrphanPool::hasOrphan(const uint256& txid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_orphans.count(txid) > 0;
}

size_t TxOrphanPool::expireOldOrphans() {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::steady_clock::now();
    std::vector<uint256> expired;

    for (const auto& [txid, entry] : m_orphans) {
        if (now >= entry.expiry) {
            expired.push_back(txid);
        }
    }

    for (const auto& txid : expired) {
        eraseOrphanLocked(txid);
    }

    if (!expired.empty()) {
        g_logger.debug("[OrphanPool] Expired " + std::to_string(expired.size()) +
                      " orphans (pool size: " + std::to_string(m_orphans.size()) + ")");
    }

    return expired.size();
}

size_t TxOrphanPool::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_orphans.size();
}

std::vector<uint256> TxOrphanPool::getOrphanTxIds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<uint256> result;
    result.reserve(m_orphans.size());
    for (const auto& [txid, _] : m_orphans) {
        result.push_back(txid);
    }
    return result;
}

std::unordered_map<std::string, size_t> TxOrphanPool::getPeerOrphanCounts() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_peer_orphan_count;
}

bool TxOrphanPool::evictRandom() {
    if (m_orphans.empty()) return false;

    // Random eviction prevents fingerprinting attacks
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, m_orphans.size() - 1);
    auto it = m_orphans.begin();
    std::advance(it, dist(rng));

    g_logger.debug("[OrphanPool] Evicting random orphan " + it->first.GetHex().substr(0, 16) + "...");
    eraseOrphanLocked(it->first);
    return true;
}

} // namespace dinero
