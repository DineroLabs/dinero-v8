#pragma once

#include "wallet/dinero_wallet_api.h"  // Phase 3 Commit 2: IWalletAPI interface

// Phase 3: Conditional compilation for gRPC vs socket transport
#ifndef DISABLE_GRPC
#include "dinerod.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#else
#include "lightning/lightning_transport.h"
#include "lightning/wallet_wire_protocol.h"
#endif

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace dinero {
namespace lightning {

/**
 * LightningNodeIdentity - Lightning node keypair
 *
 * Contains private and public keys for Lightning node identity.
 */
struct LightningNodeIdentity {
    std::vector<uint8_t> privkey;  // 32-byte private key
    std::vector<uint8_t> pubkey;   // 33-byte compressed public key
};

/**
 * UTXO - Unspent transaction output for Lightning channel funding
 *
 * Contains minimal UTXO information needed for channel funding.
 */
struct WalletUTXO {
    std::vector<uint8_t> txid;           // 32-byte transaction ID
    uint32_t vout;                        // Output index
    uint64_t value;                       // Value in una
    std::vector<uint8_t> scriptPubKey;   // Locking script
    uint32_t confirmations;               // Number of confirmations
    bool is_coinbase;                     // True if coinbase output
};

/**
 * WalletClient - gRPC client for dinerod wallet operations
 *
 * Provides Lightning Network with wallet operations via gRPC:
 * - UTXO listing (for channel funding coin selection)
 * - Lightning key derivation (HD wallet)
 * - Taproot sighash computation (for revocation transactions)
 * - Network parameters (bech32 HRP)
 *
 * Phase 2: Lightning Wallet Detachment
 * This client replaces direct WalletManager/HDWallet library linking
 * with gRPC calls to dinerod, achieving full process separation.
 *
 * Phase 3 Commit 2: Implements IWalletAPI interface
 * This allows Lightning code to depend only on the interface (header),
 * not the implementation (gRPC client library).
 *
 * Architecture:
 *   lightningd (WalletClient) --[gRPC]--> dinerod (WalletServiceImpl)
 *
 * SECURITY: Connects to localhost:50051 by default (trusted connection).
 */
class WalletClient : public wallet::IWalletAPI {
public:
    /**
     * Construct WalletClient with gRPC connection
     *
     * @param server_address  gRPC server address (default: "127.0.0.1:50051")
     */
    explicit WalletClient(const std::string& server_address = "127.0.0.1:50051");

    ~WalletClient() = default;

    /**
     * Check if wallet service is available
     *
     * @return true if connected and wallet service responds, false otherwise
     */
    bool IsAvailable() const override;

    // ===== UTXO Operations =====

    /**
     * List unspent UTXOs with confirmation filter
     *
     * Used for Lightning channel funding coin selection.
     * Returns UTXOs within the specified confirmation range.
     *
     * @param min_confirmations  Minimum confirmations required (default: 1)
     * @param max_confirmations  Maximum confirmations (default: 9999999)
     * @return Vector of unspent UTXOs matching criteria (gRPC proto type)
     * @throws std::runtime_error if gRPC call fails
     *
     * Phase 3 Commit 1: Use dinerod::UTXO (proto type) to enforce gRPC boundary
     */
    std::vector<dinerod::UTXO> listUnspentUTXOs(
        int min_confirmations = 1,
        int max_confirmations = 9999999
    ) const override;

    // ===== Lightning Key Derivation =====

    /**
     * Derive Lightning node identity key
     *
     * Derives permanent node identity keypair from HD wallet.
     * Path: m/purpose'/coin_type'/account'/lightning_node_identity'/key_index'
     *
     * @param account     Account index (default: 0)
     * @param key_index   Key index (default: 0)
     * @return LightningNodeIdentity with private and public keys
     * @throws std::runtime_error if derivation fails
     */
    LightningNodeIdentity DeriveLightningNodeIdentity(
        uint32_t account = 0,
        uint32_t key_index = 0
    ) const override;

    /**
     * Derive Lightning funding key
     *
     * Derives channel funding key from HD wallet.
     * Used for 2-of-2 multisig channel funding output.
     *
     * @param index  Key index
     * @return 32-byte private key
     * @throws std::runtime_error if derivation fails
     */
    std::vector<uint8_t> GetLightningFundingKeyAt(uint32_t index) const override;

    /**
     * Derive Lightning revocation base key
     *
     * Derives revocation base key for commitment transactions.
     * Combined with per-commitment point to create revocation keys.
     *
     * @param index  Key index
     * @return 32-byte private key
     * @throws std::runtime_error if derivation fails
     */
    std::vector<uint8_t> GetLightningRevocationBaseKeyAt(uint32_t index) const override;

    /**
     * Derive Lightning payment base key
     *
     * Derives payment base key for receiving payments.
     * Combined with per-commitment point to create payment keys.
     *
     * @param index  Key index
     * @return 32-byte private key
     * @throws std::runtime_error if derivation fails
     */
    std::vector<uint8_t> GetLightningPaymentBaseKeyAt(uint32_t index) const override;

    /**
     * Derive Lightning delayed payment base key
     *
     * Derives delayed payment base key for local outputs.
     * Used for to_local outputs with CSV delay.
     *
     * @param index  Key index
     * @return 32-byte private key
     * @throws std::runtime_error if derivation fails
     */
    std::vector<uint8_t> GetLightningDelayedPaymentBaseKeyAt(uint32_t index) const override;

    /**
     * Derive Lightning HTLC base key
     *
     * Derives HTLC base key for hash time-locked contracts.
     * Combined with per-commitment point to create HTLC keys.
     *
     * @param index  Key index
     * @return 32-byte private key
     * @throws std::runtime_error if derivation fails
     */
    std::vector<uint8_t> GetLightningHTLCBaseKeyAt(uint32_t index) const override;

    /**
     * Get revocation basepoint secret for a specific channel (Phase 7)
     *
     * Retrieves the per-channel revocation basepoint secret used for
     * deriving per-commitment revocation keys in justice transactions.
     *
     * Derivation: HMAC-SHA256(wallet_seed, "dinero-lightning-revocation" || channel_id)
     *
     * Security properties:
     * - Deterministic (same channel_id → same secret, always)
     * - Unique per channel
     * - Stable across wallet restarts
     * - Never revealed to counterparty
     *
     * @param channel_id  Channel identifier (hex string)
     * @return 32-byte revocation basepoint secret
     * @throws std::runtime_error if wallet locked, channel unknown, or RPC fails
     */
    std::vector<uint8_t> GetRevocationBasepointSecret(
        const std::string& channel_id
    ) const override;

    // ===== Transaction Signing =====

    /**
     * Compute Taproot sighash for transaction
     *
     * Computes BIP-341 Taproot sighash for signing.
     * Used by Lightning watchtower for revocation transactions.
     *
     * @param raw_tx          Serialized transaction
     * @param input_index     Input index to sign
     * @param prevout_values  Previous output values (for all inputs)
     * @param prevout_scripts Previous output scripts (for all inputs)
     * @param sighash_type    Sighash type (default: 0x00 = SIGHASH_DEFAULT)
     * @param annex           Optional annex data
     * @return 32-byte sighash
     * @throws std::runtime_error if computation fails
     */
    std::vector<uint8_t> ComputeTaprootSighash(
        const std::vector<uint8_t>& raw_tx,
        uint32_t input_index,
        const std::vector<uint64_t>& prevout_values,
        const std::vector<std::vector<uint8_t>>& prevout_scripts,
        uint8_t sighash_type = 0x00,
        const std::vector<uint8_t>& annex = {}
    ) const override;

    // ===== Network Parameters =====

    /**
     * Get network HRP (Human Readable Part) for bech32 addresses
     *
     * Returns network-specific bech32 prefix:
     * - "din" for mainnet
     * - "tdin" for testnet
     * - "rdin" for regtest
     *
     * @return Network HRP string
     * @throws std::runtime_error if call fails
     */
    std::string GetNetworkHRP() const override;

    // ===== Address Generation (Phase 3 Commit 5) =====

    /**
     * Get new change address from HD wallet
     *
     * @param label  Optional label for the address
     * @return Bech32 change address
     * @throws std::runtime_error if gRPC call fails
     */
    std::string GetNewChangeAddress(const std::string& label = "") const override;

    /**
     * Derive private key for a given scriptPubKey
     *
     * @param script_pubkey_hex  scriptPubKey in hex format
     * @return 32-byte private key
     * @throws std::runtime_error if derivation fails
     */
    std::vector<uint8_t> DeriveKeyForScriptPubKey(const std::string& script_pubkey_hex) const override;

private:
#ifndef DISABLE_GRPC
    // Dev mode: gRPC transport
    std::shared_ptr<::grpc::Channel> m_channel;
    std::unique_ptr<::dinerod::Wallet::Stub> m_stub;
#else
    // Release mode: Socket transport
    std::unique_ptr<LightningTransport> m_transport;
#endif
    std::string m_server_address;

#ifndef DISABLE_GRPC
    /**
     * Helper: Derive Lightning key by type (gRPC mode)
     *
     * @param key_type  Key type enum
     * @param account   Account index (only for NODE_IDENTITY)
     * @param index     Key index
     * @return 32-byte private key
     */
    std::vector<uint8_t> deriveLightningKey(
        ::dinerod::DeriveLightningKeyRequest::KeyType key_type,
        uint32_t account,
        uint32_t index
    ) const;
#else
    /**
     * Helper: Derive Lightning key by type (socket mode)
     *
     * @param key_type  Key type enum
     * @param account   Account index (only for NODE_IDENTITY)
     * @param index     Key index
     * @return 32-byte private key
     */
    std::vector<uint8_t> deriveLightningKey(
        LightningKeyType key_type,
        uint32_t account,
        uint32_t index
    ) const;

    /**
     * Helper: Send request and receive response over socket
     *
     * @param request_type    Request message type
     * @param request_payload Serialized request payload
     * @param expected_response_type Expected response message type
     * @return Response payload
     * @throws std::runtime_error on communication error
     */
    std::vector<uint8_t> sendRequest(
        WalletMessageType request_type,
        const std::vector<uint8_t>& request_payload,
        WalletMessageType expected_response_type
    ) const;
#endif
};

} // namespace lightning
} // namespace dinero
