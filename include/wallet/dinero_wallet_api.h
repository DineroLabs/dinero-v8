#pragma once

#include <vector>
#include <string>
#include <cstdint>

#ifndef DISABLE_GRPC
#include "dinerod.pb.h"  // gRPC proto types
#else
// DINERO_RELEASE mode: Provide minimal stub types for socket-based IPC
namespace dinerod {
    struct UTXO {
        std::string _txid;
        uint32_t _vout;
        uint64_t _value;
        std::string _script_pubkey;
        uint32_t _confirmations;
        bool _is_coinbase = false;

        // Proto-compatible getters
        const std::string& txid() const { return _txid; }
        uint32_t vout() const { return _vout; }
        uint64_t value() const { return _value; }
        const std::string& script_pubkey() const { return _script_pubkey; }
        uint32_t confirmations() const { return _confirmations; }
        bool is_coinbase() const { return _is_coinbase; }

        // Setters for construction
        void set_txid(const std::string& v) { _txid = v; }
        void set_vout(uint32_t v) { _vout = v; }
        void set_value(uint64_t v) { _value = v; }
        void set_script_pubkey(const std::string& v) { _script_pubkey = v; }
        void set_confirmations(uint32_t v) { _confirmations = v; }
        void set_is_coinbase(bool v) { _is_coinbase = v; }
    };
}
#endif

namespace dinero {
namespace lightning {

// Forward declaration (defined in wallet_client.h)
struct LightningNodeIdentity;

} // namespace lightning

namespace wallet {

/**
 * @interface IWalletAPI
 * @brief Header-only wallet boundary for Lightning Network
 *
 * Phase 3 Commit 2: Establishes compile-time boundary between Lightning and wallet.
 *
 * Design principles:
 * - Header-only (no linking dependencies)
 * - Uses proto types only (dinerod::UTXO, not internal wallet types)
 * - Minimal surface area (only what Lightning needs)
 * - Implemented by WalletClient (gRPC) for process separation
 *
 * This interface enables:
 * - Lightning library to compile without linking dinero_wallet
 * - Dependency injection (pass implementation at runtime)
 * - Future lightningd binary to use WalletClient via gRPC
 */
class IWalletAPI {
public:
    virtual ~IWalletAPI() = default;

    // ═══════════════════════════════════════════════════════════════════════════
    // Wallet Availability
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Check if wallet service is available and ready
     * @return true if wallet can process requests
     */
    virtual bool IsAvailable() const = 0;

    // ═══════════════════════════════════════════════════════════════════════════
    // UTXO Operations
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * List unspent UTXOs for Lightning channel funding
     *
     * @param min_confirmations Minimum confirmations required
     * @param max_confirmations Maximum confirmations
     * @return Vector of UTXOs (gRPC proto type)
     */
    virtual std::vector<dinerod::UTXO> listUnspentUTXOs(
        int min_confirmations = 1,
        int max_confirmations = 9999999
    ) const = 0;

    // ═══════════════════════════════════════════════════════════════════════════
    // Lightning Key Derivation
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Derive Lightning node identity keypair
     *
     * @param account Account index (default: 0)
     * @param key_index Key index (default: 0)
     * @return Lightning node identity with private and public keys
     */
    virtual lightning::LightningNodeIdentity DeriveLightningNodeIdentity(
        uint32_t account = 0,
        uint32_t key_index = 0
    ) const = 0;

    /**
     * Derive Lightning funding key for channel
     * @param index Key index
     * @return 32-byte private key
     */
    virtual std::vector<uint8_t> GetLightningFundingKeyAt(uint32_t index) const = 0;

    /**
     * Derive Lightning revocation base key
     * @param index Key index
     * @return 32-byte private key
     */
    virtual std::vector<uint8_t> GetLightningRevocationBaseKeyAt(uint32_t index) const = 0;

    /**
     * Derive Lightning payment base key
     * @param index Key index
     * @return 32-byte private key
     */
    virtual std::vector<uint8_t> GetLightningPaymentBaseKeyAt(uint32_t index) const = 0;

    /**
     * Derive Lightning delayed payment base key
     * @param index Key index
     * @return 32-byte private key
     */
    virtual std::vector<uint8_t> GetLightningDelayedPaymentBaseKeyAt(uint32_t index) const = 0;

    /**
     * Derive Lightning HTLC base key
     * @param index Key index
     * @return 32-byte private key
     */
    virtual std::vector<uint8_t> GetLightningHTLCBaseKeyAt(uint32_t index) const = 0;

    /**
     * Get revocation basepoint secret for a specific channel (Phase 7)
     *
     * Used for justice transactions: deriving per-commitment revocation keys
     * to claim outputs from revoked commitment transactions.
     *
     * Derivation: HMAC-SHA256(wallet_master_seed, "dinero-lightning-revocation" || channel_id)
     *
     * Security properties:
     * - Deterministic (same channel_id always returns same secret)
     * - Unique per channel
     * - Stable across wallet restarts (no random component)
     * - Never revealed to counterparty
     *
     * @param channel_id Channel identifier (hex string)
     * @return 32-byte revocation basepoint secret
     * @throws std::runtime_error if wallet locked or channel unknown
     */
    virtual std::vector<uint8_t> GetRevocationBasepointSecret(
        const std::string& channel_id
    ) const = 0;

    // ═══════════════════════════════════════════════════════════════════════════
    // Transaction Signing
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Compute Taproot sighash for transaction input
     *
     * @param raw_tx Serialized transaction
     * @param input_index Input index to sign
     * @param prevout_values Previous output values (all inputs)
     * @param prevout_scripts Previous output scripts (all inputs)
     * @param sighash_type Sighash type (default: SIGHASH_DEFAULT)
     * @param annex Optional annex data
     * @return 32-byte sighash
     */
    virtual std::vector<uint8_t> ComputeTaprootSighash(
        const std::vector<uint8_t>& raw_tx,
        uint32_t input_index,
        const std::vector<uint64_t>& prevout_values,
        const std::vector<std::vector<uint8_t>>& prevout_scripts,
        uint8_t sighash_type = 0x00,
        const std::vector<uint8_t>& annex = {}
    ) const = 0;

    // ═══════════════════════════════════════════════════════════════════════════
    // Network Parameters
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Get network HRP (Human Readable Part) for bech32 addresses
     * @return Network HRP ("din", "tdin", "rdin")
     */
    virtual std::string GetNetworkHRP() const = 0;

    // ═══════════════════════════════════════════════════════════════════════════
    // Address Generation (Phase 3 Commit 5)
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * Get new change address from HD wallet
     * @param label Optional label for the address
     * @return Bech32 change address
     */
    virtual std::string GetNewChangeAddress(const std::string& label = "") const = 0;

    /**
     * Derive private key for a given scriptPubKey
     * @param script_pubkey_hex scriptPubKey in hex format
     * @return 32-byte private key
     */
    virtual std::vector<uint8_t> DeriveKeyForScriptPubKey(const std::string& script_pubkey_hex) const = 0;
};

} // namespace wallet
} // namespace dinero
