#include "grpc/wallet_service.h"
#include "daemon/daemon_context.h"
#include "daemon/services/wallet_service.h"
#include "wallet/wallet_manager.h"
#include "wallet/hd_wallet.h"
#include "lightning/keys/lightning_key_deriver.h"  // Lightning key derivation
#include "consensus/coin_type.h"                   // DINERO_COIN_TYPE
#include "consensus/script_verify.h"
#include "wallet/transaction.h"
#include "primitives/uint256.h"
#include "common/logger.h"
#include "address/addr_codec.h"
#include <cstring>

namespace dinero {
namespace grpc_server {

WalletServiceImpl::WalletServiceImpl(DaemonContext* daemon_ctx)
    : m_daemon_ctx(daemon_ctx)
{
    g_logger.info("WalletService (gRPC) initialized");
}

// ============================================================================
// Existing Wallet Methods (Stubs for now - can implement later if needed)
// ============================================================================

::grpc::Status WalletServiceImpl::GetNewAddress(
    ::grpc::ServerContext* context,
    const ::dinerod::AddressRequest* request,
    ::dinerod::AddressResponse* response
) {
    // TODO: Implement if Lightning needs it
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "GetNewAddress not yet implemented");
}

::grpc::Status WalletServiceImpl::SignRawTransaction(
    ::grpc::ServerContext* context,
    const ::dinerod::SignRequest* request,
    ::dinerod::SignResponse* response
) {
    // TODO: Implement if Lightning needs it
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "SignRawTransaction not yet implemented");
}

::grpc::Status WalletServiceImpl::GetBalance(
    ::grpc::ServerContext* context,
    const ::dinerod::EmptyRequest* request,
    ::dinerod::BalanceResponse* response
) {
    // TODO: Implement if Lightning needs it
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "GetBalance not yet implemented");
}

// ============================================================================
// Phase 2: Lightning Wallet Operations
// ============================================================================

::grpc::Status WalletServiceImpl::ListUnspentUTXOs(
    ::grpc::ServerContext* context,
    const ::dinerod::ListUTXOsRequest* request,
    ::dinerod::ListUTXOsResponse* response
) {
    try {
        // Check wallet availability
        if (!m_daemon_ctx || !m_daemon_ctx->wallet) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "Wallet not available");
        }

        // Get wallet reference
        auto& wallet_service = *m_daemon_ctx->wallet;
        WalletManager& wallet_mgr = wallet_service.get();

        // Call wallet method
        int min_conf = request->min_confirmations();
        int max_conf = request->max_confirmations();

        auto utxos = wallet_mgr.listUnspentUTXOs(min_conf, max_conf);

        // Convert WalletUTXO to proto UTXO
        for (const auto& wallet_utxo : utxos) {
            auto* proto_utxo = response->add_utxos();

            // Convert txid string to bytes
            uint256 txid = uint256::FromHexUnsafe(wallet_utxo.txid);
            proto_utxo->set_txid(txid.begin(), 32);

            proto_utxo->set_vout(wallet_utxo.vout);
            proto_utxo->set_value(wallet_utxo.amount_una);

            // Get scriptPubKey from global UTXO (wallet doesn't store it)
            // For now, we'll leave it empty and populate if needed
            // TODO: Look up scriptPubKey from chainstate if Lightning needs it

            proto_utxo->set_confirmations(wallet_utxo.confirmations);
            proto_utxo->set_is_coinbase(wallet_utxo.is_coinbase);
        }

        g_logger.debug("ListUnspentUTXOs: Returned " + std::to_string(response->utxos_size()) +
                      " UTXOs (min_conf=" + std::to_string(min_conf) +
                      ", max_conf=" + std::to_string(max_conf) + ")");

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("ListUnspentUTXOs failed: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status WalletServiceImpl::DeriveLightningKey(
    ::grpc::ServerContext* context,
    const ::dinerod::DeriveLightningKeyRequest* request,
    ::dinerod::DeriveLightningKeyResponse* response
) {
    try {
        // Check wallet availability
        if (!m_daemon_ctx || !m_daemon_ctx->wallet) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "Wallet not available");
        }

        // Get HD wallet reference
        auto& wallet_service = *m_daemon_ctx->wallet;
        WalletManager& wallet_mgr = wallet_service.get();
        HDWallet* hd_wallet = wallet_mgr.getHDWallet();

        if (!hd_wallet) {
            return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION,
                                  "HD wallet not initialized");
        }

        // Get seed from HD wallet for Lightning key derivation
        auto seed = hd_wallet->GetSeed();
        if (seed.empty()) {
            return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION,
                                  "Wallet seed not available");
        }

        // Create Lightning key deriver
        dinero::lightning::LightningKeyDeriver key_deriver(seed.data(), seed.size(), dinero::consensus::DINERO_COIN_TYPE);

        uint32_t index = request->index();
        std::vector<uint8_t> private_key;

        // Derive key based on type
        switch (request->key_type()) {
            case ::dinerod::DeriveLightningKeyRequest::NODE_IDENTITY: {
                auto identity = key_deriver.GetNodeIdentity();
                private_key = identity.privkey;
                break;
            }

            case ::dinerod::DeriveLightningKeyRequest::FUNDING:
                private_key = key_deriver.GetFundingKey(index);
                break;

            case ::dinerod::DeriveLightningKeyRequest::REVOCATION_BASE:
                private_key = key_deriver.GetRevocationBaseKey(index);
                break;

            case ::dinerod::DeriveLightningKeyRequest::PAYMENT_BASE:
                private_key = key_deriver.GetPaymentBaseKey(index);
                break;

            case ::dinerod::DeriveLightningKeyRequest::DELAYED_PAYMENT_BASE:
                private_key = key_deriver.GetDelayedPaymentBaseKey(index);
                break;

            case ::dinerod::DeriveLightningKeyRequest::HTLC_BASE:
                private_key = key_deriver.GetHTLCBaseKey(index);
                break;

            default:
                return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                      "Unknown key type");
        }

        if (private_key.size() != 32) {
            return ::grpc::Status(::grpc::StatusCode::INTERNAL,
                                  "Key derivation returned invalid key");
        }

        // Set private key
        response->set_private_key(private_key.data(), private_key.size());

        // Derive public key from private key using secp256k1
        // TODO: Add public key derivation if Lightning needs it
        // For now, Lightning can derive it locally from private key

        g_logger.debug("DeriveLightningKey: type=" + std::to_string(request->key_type()) +
                      ", index=" + std::to_string(index));

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("DeriveLightningKey failed: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status WalletServiceImpl::ComputeTaprootSighash(
    ::grpc::ServerContext* context,
    const ::dinerod::TaprootSighashRequest* request,
    ::dinerod::TaprootSighashResponse* response
) {
    try {
        // Deserialize transaction
        std::vector<uint8_t> raw_tx(request->raw_tx().begin(), request->raw_tx().end());
        Transaction tx;
        if (!TransactionSerializer::Deserialize(tx, raw_tx)) {
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "Failed to deserialize transaction");
        }

        // Convert prevout values
        std::vector<uint64_t> prevout_values;
        for (int i = 0; i < request->prevout_values_size(); i++) {
            prevout_values.push_back(request->prevout_values(i));
        }

        // Convert prevout scripts
        std::vector<std::vector<uint8_t>> prevout_scripts;
        for (int i = 0; i < request->prevout_scripts_size(); i++) {
            const auto& script_bytes = request->prevout_scripts(i);
            prevout_scripts.emplace_back(script_bytes.begin(), script_bytes.end());
        }

        // Get annex if provided
        std::vector<uint8_t> annex;
        if (!request->annex().empty()) {
            annex.assign(request->annex().begin(), request->annex().end());
        }

        // Compute sighash
        std::vector<uint8_t> sighash = consensus::ScriptVerifier::ComputeTaprootSighash(
            tx,
            request->input_index(),
            prevout_values,
            prevout_scripts,
            request->sighash_type(),
            annex
        );

        if (sighash.size() != 32) {
            return ::grpc::Status(::grpc::StatusCode::INTERNAL,
                                  "Sighash computation returned invalid hash");
        }

        response->set_sighash(sighash.data(), sighash.size());

        g_logger.debug("ComputeTaprootSighash: input_index=" + std::to_string(request->input_index()) +
                      ", sighash_type=0x" + std::to_string(request->sighash_type()));

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("ComputeTaprootSighash failed: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status WalletServiceImpl::GetNetworkHRP(
    ::grpc::ServerContext* context,
    const ::dinerod::EmptyRequest* request,
    ::dinerod::NetworkHRPResponse* response
) {
    try {
        // Get network HRP
        std::string hrp = HrpForActiveNetworkRef();
        response->set_hrp(hrp);

        g_logger.debug("GetNetworkHRP: " + hrp);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetNetworkHRP failed: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

// Phase 3 Commit 5: Additional wallet operations

::grpc::Status WalletServiceImpl::GetNewChangeAddress(
    ::grpc::ServerContext* context,
    const ::dinerod::ChangeAddressRequest* request,
    ::dinerod::AddressResponse* response
) {
    try {
        // Check if wallet is available
        if (!m_daemon_ctx || !m_daemon_ctx->wallet) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "Wallet service not available");
        }

        if (!m_daemon_ctx->wallet->hasActiveWallet()) {
            return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "No active wallet");
        }

        // Get wallet manager
        WalletManager& wallet_mgr = m_daemon_ctx->wallet->get();

        // Get new change address
        std::string label = request->label();
        std::string address = wallet_mgr.getNewChangeAddress(label);

        // Set response
        response->set_address(address);

        g_logger.debug("GetNewChangeAddress: " + address + " (label: " + label + ")");

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetNewChangeAddress failed: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

::grpc::Status WalletServiceImpl::DeriveKeyForScriptPubKey(
    ::grpc::ServerContext* context,
    const ::dinerod::ScriptPubKeyRequest* request,
    ::dinerod::PrivateKeyResponse* response
) {
    try {
        // Check if wallet is available
        if (!m_daemon_ctx || !m_daemon_ctx->wallet) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "Wallet service not available");
        }

        if (!m_daemon_ctx->wallet->hasActiveWallet()) {
            return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "No active wallet");
        }

        // Get wallet manager
        WalletManager& wallet_mgr = m_daemon_ctx->wallet->get();

        // Derive private key for scriptPubKey
        std::string script_pubkey_hex = request->script_pubkey_hex();
        auto privkey_opt = wallet_mgr.deriveKeyForScriptPubKey(script_pubkey_hex);

        if (!privkey_opt.has_value()) {
            return ::grpc::Status(::grpc::StatusCode::NOT_FOUND,
                                 "No private key found for scriptPubKey");
        }

        // Set response
        const auto& privkey = privkey_opt.value();
        response->set_private_key(privkey.data(), privkey.size());

        g_logger.debug("DeriveKeyForScriptPubKey: Derived private key for scriptPubKey");

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("DeriveKeyForScriptPubKey failed: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

// ============================================================================
// Phase 7: Justice Transaction Support
// ============================================================================

::grpc::Status WalletServiceImpl::GetRevocationBasepointSecret(
    ::grpc::ServerContext* context,
    const ::dinerod::GetRevocationBasepointSecretRequest* request,
    ::dinerod::GetRevocationBasepointSecretResponse* response
) {
    try {
        // Check if wallet is available
        if (!m_daemon_ctx || !m_daemon_ctx->wallet) {
            return ::grpc::Status(::grpc::StatusCode::UNAVAILABLE, "Wallet service not available");
        }

        if (!m_daemon_ctx->wallet->hasActiveWallet()) {
            return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "No active wallet");
        }

        // Get wallet manager
        WalletManager& wallet_mgr = m_daemon_ctx->wallet->get();

        // Get HD wallet
        HDWallet* hd_wallet = wallet_mgr.getHDWallet();
        if (!hd_wallet) {
            return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "HD wallet not initialized");
        }

        // Get seed from HD wallet for Lightning key derivation
        auto seed = hd_wallet->GetSeed();
        if (seed.empty()) {
            return ::grpc::Status(::grpc::StatusCode::FAILED_PRECONDITION, "Wallet seed not available");
        }

        // Create Lightning key deriver
        dinero::lightning::LightningKeyDeriver key_deriver(seed.data(), seed.size(), dinero::consensus::DINERO_COIN_TYPE);

        // Get channel ID from request
        std::string channel_id = request->channel_id();
        if (channel_id.empty()) {
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "channel_id cannot be empty");
        }

        // Derive revocation basepoint secret deterministically
        // HMAC-SHA256(wallet_master_seed, "dinero-lightning-revocation" || channel_id)
        auto secret = key_deriver.GetRevocationBasepointSecret(channel_id);

        // Absolute safety check
        if (secret.size() != 32) {
            g_logger.error("GetRevocationBasepointSecret: Invalid secret size " +
                          std::to_string(secret.size()) + " (expected 32)");
            return ::grpc::Status(::grpc::StatusCode::INTERNAL, "Invalid secret size");
        }

        // Set response
        response->set_secret(secret.data(), 32);

        g_logger.debug("GetRevocationBasepointSecret: Returned secret for channel " + channel_id);

        return ::grpc::Status::OK;

    } catch (const std::exception& e) {
        g_logger.error("GetRevocationBasepointSecret failed: " + std::string(e.what()));
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }
}

} // namespace grpc_server
} // namespace dinero
