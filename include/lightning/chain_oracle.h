#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Lightning Chain Query Interface (L1↔L2 Boundary)
// ═══════════════════════════════════════════════════════════════════════════
// Defines the interface through which Lightning (L2) queries blockchain data.
//
// ARCHITECTURE:
// - Lightning MUST NOT include daemon/chainstate/mempool headers
// - Lightning communicates with L1 ONLY through this interface
// - Production implementation uses RPC calls
// - Test implementation uses mocks
//
// This enforces architectural boundary: Lightning depends on interface, not implementation.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lightning {

/**
 * Interface for Lightning to query blockchain state.
 *
 * Production implementation: Uses JSON-RPC calls to dinerod
 * Test implementation: MockChainOracle with controlled responses
 *
 * Replaces direct access to:
 * - DaemonContext::chainstate
 * - DaemonContext::mempool
 */
class IChainOracle {
public:
    virtual ~IChainOracle() = default;

    // ═══════════════════════════════════════════════════════════════════════
    // Blockchain Queries (replaces chainstate direct access)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Get current blockchain height.
     * RPC equivalent: getblockcount
     */
    virtual uint64_t getBlockHeight() const = 0;

    /**
     * Get block hash at given height.
     * RPC equivalent: getblockhash <height>
     */
    virtual std::optional<std::string> getBlockHash(uint64_t height) const = 0;

    /**
     * Check if transaction output is unspent.
     * RPC equivalent: gettxout <txid> <vout>
     * Returns: true if UTXO exists, false if spent/doesn't exist
     */
    virtual bool isUnspent(const std::string& txid, uint32_t vout) const = 0;

    // ═══════════════════════════════════════════════════════════════════════
    // Transaction Broadcasting (replaces mempool direct access)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Broadcast transaction to network.
     * RPC equivalent: sendrawtransaction <hex>
     * Returns: true if accepted, false if rejected
     */
    virtual bool broadcastTransaction(const std::string& tx_hex) = 0;

    /**
     * Check if transaction is in mempool.
     * RPC equivalent: getmempoolentry <txid>
     * Returns: true if in mempool, false otherwise
     */
    virtual bool isInMempool(const std::string& txid) const = 0;

    // ═══════════════════════════════════════════════════════════════════════
    // Transaction Queries (Phase 7B/7C/7D)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Get transaction by txid (from confirmed blocks).
     * RPC equivalent: getrawtransaction <txid> 1
     * Returns: Transaction hex if found, nullopt if not found
     *
     * Phase 7C/7D: Used by justice oracle to fetch revoked commitment TX
     */
    virtual std::optional<std::string> getTransaction(const std::string& txid) const = 0;

    /**
     * Get block height where transaction was confirmed.
     * RPC equivalent: getrawtransaction <txid> 1 → blockheight field
     * Returns: Block height if confirmed, nullopt if unconfirmed
     *
     * Phase 7B/7C/7D: Used to check sweep/justice confirmation
     */
    virtual std::optional<uint64_t> getTransactionHeight(const std::string& txid) const = 0;
};

/**
 * Mock implementation for testing.
 * Allows tests to control blockchain state without running full node.
 */
class MockChainOracle : public IChainOracle {
public:
    MockChainOracle() = default;

    // Test configuration
    void setBlockHeight(uint64_t height) { m_block_height = height; }
    void setUnspent(const std::string& txid, uint32_t vout, bool unspent) {
        m_unspent_outputs[txid + ":" + std::to_string(vout)] = unspent;
    }
    void setInMempool(const std::string& txid, bool in_mempool) {
        m_mempool[txid] = in_mempool;
    }
    void setTransaction(const std::string& txid, const std::string& tx_hex, uint64_t height) {
        m_transactions[txid] = tx_hex;
        m_tx_heights[txid] = height;
    }

    // IChainOracle implementation
    uint64_t getBlockHeight() const override { return m_block_height; }

    std::optional<std::string> getBlockHash(uint64_t height) const override {
        if (height > m_block_height) return std::nullopt;
        return "mock_block_hash_" + std::to_string(height);
    }

    bool isUnspent(const std::string& txid, uint32_t vout) const override {
        auto it = m_unspent_outputs.find(txid + ":" + std::to_string(vout));
        return it != m_unspent_outputs.end() && it->second;
    }

    bool broadcastTransaction(const std::string&) override {
        return true; // Mock always succeeds
    }

    bool isInMempool(const std::string& txid) const override {
        auto it = m_mempool.find(txid);
        return it != m_mempool.end() && it->second;
    }

    std::optional<std::string> getTransaction(const std::string& txid) const override {
        auto it = m_transactions.find(txid);
        if (it != m_transactions.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<uint64_t> getTransactionHeight(const std::string& txid) const override {
        auto it = m_tx_heights.find(txid);
        if (it != m_tx_heights.end()) {
            return it->second;
        }
        return std::nullopt;
    }

private:
    uint64_t m_block_height = 0;
    std::map<std::string, bool> m_unspent_outputs;
    std::map<std::string, bool> m_mempool;
    std::map<std::string, std::string> m_transactions;  // txid -> tx_hex
    std::map<std::string, uint64_t> m_tx_heights;       // txid -> block height
};

} // namespace lightning
