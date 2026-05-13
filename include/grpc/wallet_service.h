#pragma once

#include "dinerod.grpc.pb.h"
#include <grpcpp/grpcpp.h>

// Forward declarations
struct DaemonContext;

namespace dinero {
namespace grpc_server {

/**
 * WalletServiceImpl - gRPC service for wallet operations
 *
 * Implements the Wallet service defined in proto/dinerod.proto.
 * Provides Lightning Network with wallet operations:
 * - UTXO listing (for channel funding)
 * - Lightning key derivation (HD wallet)
 * - Taproot sighash computation (for revocation txs)
 * - Network parameters (bech32 HRP)
 *
 * Phase 2: Lightning Wallet Detachment
 * This service allows lightningd to access wallet operations via gRPC
 * instead of linking the wallet library directly.
 *
 * SECURITY: These methods require wallet access and should only be
 * exposed to trusted clients (localhost by default).
 */
class WalletServiceImpl final : public dinerod::Wallet::Service {
public:
    /**
     * Construct WalletService with DaemonContext
     *
     * @param daemon_ctx  DaemonContext with wallet and network state
     */
    explicit WalletServiceImpl(DaemonContext* daemon_ctx);
    ~WalletServiceImpl() override = default;

    // Existing wallet service methods

    /**
     * Get new address from wallet
     */
    ::grpc::Status GetNewAddress(
        ::grpc::ServerContext* context,
        const ::dinerod::AddressRequest* request,
        ::dinerod::AddressResponse* response
    ) override;

    /**
     * Sign raw transaction with wallet keys
     */
    ::grpc::Status SignRawTransaction(
        ::grpc::ServerContext* context,
        const ::dinerod::SignRequest* request,
        ::dinerod::SignResponse* response
    ) override;

    /**
     * Get wallet balance
     */
    ::grpc::Status GetBalance(
        ::grpc::ServerContext* context,
        const ::dinerod::EmptyRequest* request,
        ::dinerod::BalanceResponse* response
    ) override;

    // Phase 2: Lightning Wallet Operations

    /**
     * List unspent UTXOs with confirmation filter
     *
     * Used by Lightning for channel funding coin selection.
     * Returns UTXOs within the specified confirmation range.
     */
    ::grpc::Status ListUnspentUTXOs(
        ::grpc::ServerContext* context,
        const ::dinerod::ListUTXOsRequest* request,
        ::dinerod::ListUTXOsResponse* response
    ) override;

    /**
     * Derive Lightning-specific keys from HD wallet
     *
     * Supports all Lightning key types:
     * - NODE_IDENTITY: Permanent node identity
     * - FUNDING: Channel funding keys
     * - REVOCATION_BASE: Revocation base keys
     * - PAYMENT_BASE: Payment base keys
     * - DELAYED_PAYMENT_BASE: Delayed payment base keys
     * - HTLC_BASE: HTLC base keys
     *
     * Returns both private and public keys.
     */
    ::grpc::Status DeriveLightningKey(
        ::grpc::ServerContext* context,
        const ::dinerod::DeriveLightningKeyRequest* request,
        ::dinerod::DeriveLightningKeyResponse* response
    ) override;

    /**
     * Compute Taproot sighash for transaction
     *
     * Used by Lightning watchtower for revocation transactions.
     * Implements BIP-341 Taproot sighash computation.
     */
    ::grpc::Status ComputeTaprootSighash(
        ::grpc::ServerContext* context,
        const ::dinerod::TaprootSighashRequest* request,
        ::dinerod::TaprootSighashResponse* response
    ) override;

    /**
     * Get network HRP (Human Readable Part) for bech32 addresses
     *
     * Returns:
     * - "din" for mainnet
     * - "tdin" for testnet
     * - "rdin" for regtest
     */
    ::grpc::Status GetNetworkHRP(
        ::grpc::ServerContext* context,
        const ::dinerod::EmptyRequest* request,
        ::dinerod::NetworkHRPResponse* response
    ) override;

    // Phase 3 Commit 5: Additional wallet operations

    /**
     * Get new change address from HD wallet
     *
     * Used by Lightning for channel funding change outputs.
     */
    ::grpc::Status GetNewChangeAddress(
        ::grpc::ServerContext* context,
        const ::dinerod::ChangeAddressRequest* request,
        ::dinerod::AddressResponse* response
    ) override;

    /**
     * Derive private key for a given scriptPubKey
     *
     * Used by Lightning for signing funding transactions.
     */
    ::grpc::Status DeriveKeyForScriptPubKey(
        ::grpc::ServerContext* context,
        const ::dinerod::ScriptPubKeyRequest* request,
        ::dinerod::PrivateKeyResponse* response
    ) override;

    // Phase 7: Justice Transaction Support

    /**
     * Get revocation basepoint secret for a specific channel
     *
     * Used by Lightning for deriving per-commitment revocation keys
     * in justice transactions (claiming outputs from revoked commitments).
     *
     * Returns deterministic 32-byte secret derived from:
     * HMAC-SHA256(wallet_master_seed, "dinero-lightning-revocation" || channel_id)
     *
     * Security properties:
     * - Deterministic (same channel_id → same secret)
     * - Unique per channel
     * - Stable across wallet restarts
     * - Never revealed to counterparty
     */
    ::grpc::Status GetRevocationBasepointSecret(
        ::grpc::ServerContext* context,
        const ::dinerod::GetRevocationBasepointSecretRequest* request,
        ::dinerod::GetRevocationBasepointSecretResponse* response
    ) override;

private:
    DaemonContext* m_daemon_ctx;  // Access to wallet and network state
};

} // namespace grpc_server
} // namespace dinero
