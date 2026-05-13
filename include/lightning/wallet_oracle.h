#pragma once
// ═══════════════════════════════════════════════════════════════════════════
// Lightning Wallet Query Interface (L1↔L2 Boundary)
// ═══════════════════════════════════════════════════════════════════════════
// Defines the interface through which Lightning (L2) queries wallet data.
//
// ARCHITECTURE:
// - Lightning MUST NOT include wallet/wallet_manager headers
// - Lightning communicates with wallet ONLY through this interface
// - Production implementation uses RPC calls or wallet manager
// - Test implementation uses mocks
//
// This enforces architectural boundary: Lightning depends on interface, not implementation.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace lightning {

/**
 * Interface for Lightning to query wallet state.
 *
 * Production implementation: Uses wallet_manager or JSON-RPC calls
 * Test implementation: MockWalletOracle with controlled responses
 *
 * Replaces direct access to:
 * - WalletManager
 * - Wallet::listUTXOs()
 * - Wallet::getBalance()
 */
class IWalletOracle {
public:
    virtual ~IWalletOracle() = default;

    /**
     * UTXO data structure for wallet queries
     */
    struct UTXO {
        std::string txid;              // Transaction ID (hex)
        uint32_t vout = 0;             // Output index
        uint64_t amount_sats = 0;      // Amount in una (una)
        std::string address;           // Address controlling this UTXO
        int confirmations = 0;         // Number of confirmations

        UTXO() = default;
        UTXO(const std::string& t, uint32_t v, uint64_t amt, const std::string& addr, int conf = 0)
            : txid(t), vout(v), amount_sats(amt), address(addr), confirmations(conf) {}
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Wallet Availability
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Check if wallet is available and unlocked.
     * Returns: true if wallet can be queried, false otherwise
     */
    virtual bool isAvailable() const = 0;

    // ═══════════════════════════════════════════════════════════════════════
    // Balance Queries
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Get total wallet balance (confirmed + unconfirmed).
     * RPC equivalent: getbalance
     * Returns: Balance in una (una)
     */
    virtual uint64_t getBalance() const = 0;

    /**
     * Get confirmed wallet balance only.
     * RPC equivalent: getbalance 1 (with min confirmations)
     * Returns: Confirmed balance in una (una)
     */
    virtual uint64_t getConfirmedBalance() const = 0;

    // ═══════════════════════════════════════════════════════════════════════
    // UTXO Queries
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * List available UTXOs for spending.
     * RPC equivalent: listunspent <minconf>
     * @param min_confirmations Minimum confirmations required (default 1)
     * Returns: Vector of available UTXOs
     */
    virtual std::vector<UTXO> listUTXOs(int min_confirmations = 1) const = 0;

    /**
     * Get specific UTXO by txid:vout.
     * Returns: UTXO if found and spendable, std::nullopt otherwise
     */
    virtual std::optional<UTXO> getUTXO(const std::string& txid, uint32_t vout) const = 0;

    // ═══════════════════════════════════════════════════════════════════════
    // Phase 7: Lightning Revocation Secrets (Justice Transaction Support)
    // ═══════════════════════════════════════════════════════════════════════

    /**
     * Get revocation basepoint secret for a specific channel.
     *
     * Phase 7C/7D: Used to derive per-commitment revocation private keys for
     * justice transactions (claiming outputs from revoked commitments).
     *
     * Security properties:
     * - Deterministically derived from wallet seed + channel_id
     * - Unique per channel
     * - Stable across wallet restarts
     * - Never exposed to counterparty
     *
     * @param channel_id Channel identifier (hex string)
     * @return 32-byte revocation basepoint secret
     * @throws std::runtime_error if channel unknown or wallet unavailable
     */
    virtual std::vector<uint8_t> getRevocationBasepointSecret(
        const std::string& channel_id
    ) const = 0;
};

/**
 * Mock implementation for testing.
 * Allows tests to control wallet state without running full wallet.
 */
class MockWalletOracle : public IWalletOracle {
public:
    MockWalletOracle() = default;

    // Test configuration
    void setAvailable(bool available) { m_available = available; }
    void setBalance(uint64_t balance) { m_balance = balance; }
    void setConfirmedBalance(uint64_t confirmed) { m_confirmed_balance = confirmed; }

    void addUTXO(const std::string& txid, uint32_t vout, uint64_t amount_sats,
                 const std::string& address, int confirmations = 1) {
        m_utxos.push_back(UTXO(txid, vout, amount_sats, address, confirmations));
    }

    void clearUTXOs() { m_utxos.clear(); }

    // Phase 7: Revocation secret configuration
    void setRevocationSecret(const std::string& channel_id, const std::vector<uint8_t>& secret) {
        if (secret.size() != 32) {
            throw std::runtime_error("Revocation secret must be exactly 32 bytes");
        }
        m_revocation_secrets[channel_id] = secret;
    }

    void clearRevocationSecrets() { m_revocation_secrets.clear(); }

    // Test helpers
    size_t utxoCount() const { return m_utxos.size(); }
    bool hasRevocationSecret(const std::string& channel_id) const {
        return m_revocation_secrets.find(channel_id) != m_revocation_secrets.end();
    }

    // IWalletOracle implementation
    bool isAvailable() const override {
        return m_available;
    }

    uint64_t getBalance() const override {
        return m_balance;
    }

    uint64_t getConfirmedBalance() const override {
        return m_confirmed_balance;
    }

    std::vector<UTXO> listUTXOs(int min_confirmations = 1) const override {
        std::vector<UTXO> result;
        for (const auto& utxo : m_utxos) {
            if (utxo.confirmations >= min_confirmations) {
                result.push_back(utxo);
            }
        }
        return result;
    }

    std::optional<UTXO> getUTXO(const std::string& txid, uint32_t vout) const override {
        for (const auto& utxo : m_utxos) {
            if (utxo.txid == txid && utxo.vout == vout) {
                return utxo;
            }
        }
        return std::nullopt;
    }

    std::vector<uint8_t> getRevocationBasepointSecret(
        const std::string& channel_id
    ) const override {
        auto it = m_revocation_secrets.find(channel_id);
        if (it == m_revocation_secrets.end()) {
            throw std::runtime_error("Unknown channel: " + channel_id);
        }
        return it->second;
    }

private:
    bool m_available = true;
    uint64_t m_balance = 0;
    uint64_t m_confirmed_balance = 0;
    std::vector<UTXO> m_utxos;
    std::map<std::string, std::vector<uint8_t>> m_revocation_secrets;  // Phase 7: channel_id → secret
};

} // namespace lightning
