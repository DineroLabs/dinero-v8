#include "grpc/socket_wallet_server.h"
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

#include "compat/net_compat.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dinero {
namespace grpc_server {

using namespace lightning;

SocketWalletServer::SocketWalletServer(DaemonContext* daemon_ctx, const std::string& address)
    : m_daemon_ctx(daemon_ctx)
    , m_address(address)
    , m_running(false)
    , m_server_socket(-1)
{
}

SocketWalletServer::~SocketWalletServer() {
    Stop();
}

bool SocketWalletServer::Start() {
    if (m_running) {
        g_logger.warning("SocketWalletServer already running");
        return true;
    }

    m_last_error.clear();

    // Parse address (only TCP supported for now, Unix sockets in future)
    std::string host;
    int port;

    size_t colon_pos = m_address.find(':');
    if (colon_pos == std::string::npos) {
        m_last_error = "Invalid address format (expected host:port): " + m_address;
        g_logger.error("SocketWalletServer: " + m_last_error);
        return false;
    }

    host = m_address.substr(0, colon_pos);
    try {
        port = std::stoi(m_address.substr(colon_pos + 1));
    } catch (const std::exception& e) {
        m_last_error = "Invalid port number: " + m_address.substr(colon_pos + 1);
        g_logger.error("SocketWalletServer: " + m_last_error);
        return false;
    }

    // Create TCP socket
    m_server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_server_socket < 0) {
        m_last_error = "Failed to create socket: " + std::string(strerror(errno));
        g_logger.error("SocketWalletServer: " + m_last_error);
        return false;
    }

    // Set socket options (reuse address and port - needed for rapid restarts)
    int opt = 1;
    if (setsockopt(m_server_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt)) < 0) {
        g_logger.warning("SocketWalletServer: Failed to set SO_REUSEADDR: " + std::string(strerror(errno)));
    }

    // SO_REUSEPORT is needed on macOS to avoid "Address already in use" on rapid restarts
    #ifdef SO_REUSEPORT
    if (setsockopt(m_server_socket, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&opt), sizeof(opt)) < 0) {
        g_logger.warning("SocketWalletServer: Failed to set SO_REUSEPORT: " + std::string(strerror(errno)));
    }
    #endif

    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
        m_last_error = "Invalid address: " + host;
        g_logger.error("SocketWalletServer: " + m_last_error);
        COMPAT_CLOSE_SOCKET(m_server_socket);
        m_server_socket = -1;
        return false;
    }

    if (bind(m_server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        m_last_error = "Failed to bind to " + m_address + ": " + std::string(strerror(errno));
        g_logger.error("SocketWalletServer: " + m_last_error);
        COMPAT_CLOSE_SOCKET(m_server_socket);
        m_server_socket = -1;
        return false;
    }

    // Listen for connections
    if (listen(m_server_socket, 5) < 0) {
        m_last_error = "Failed to listen: " + std::string(strerror(errno));
        g_logger.error("SocketWalletServer: " + m_last_error);
        COMPAT_CLOSE_SOCKET(m_server_socket);
        m_server_socket = -1;
        return false;
    }

    m_running = true;

    // Spawn acceptor thread
    m_acceptor_thread = std::make_unique<std::thread>(&SocketWalletServer::acceptorLoop, this);

    g_logger.info("SocketWalletServer listening on " + m_address + " (socket mode)");

    return true;
}

void SocketWalletServer::Stop() {
    if (!m_running) {
        return;
    }

    m_running = false;

    // Close server socket (this will unblock accept())
    if (m_server_socket >= 0) {
        COMPAT_CLOSE_SOCKET(m_server_socket);
        m_server_socket = -1;
    }

    // Close all client sockets to unblock recv(MSG_WAITALL) in handler threads
    {
        std::lock_guard<std::mutex> lock(m_client_sockets_mutex);
        for (int fd : m_client_sockets) {
            if (fd >= 0) {
                shutdown(fd, SHUT_RDWR);
                COMPAT_CLOSE_SOCKET(fd);
            }
        }
        m_client_sockets.clear();
    }

    // Wait for acceptor thread
    if (m_acceptor_thread && m_acceptor_thread->joinable()) {
        m_acceptor_thread->join();
    }

    // Wait for all client threads (now unblocked by socket closure)
    for (auto& thread : m_client_threads) {
        if (thread && thread->joinable()) {
            thread->join();
        }
    }
    m_client_threads.clear();

    g_logger.info("SocketWalletServer stopped");
}

void SocketWalletServer::acceptorLoop() {
    g_logger.debug("SocketWalletServer acceptor thread started");

    while (m_running) {
        // Poll before accept — raw accept() may not unblock when socket is closed
        // from another thread on Linux. poll() with 1s timeout lets us re-check m_running.
        struct pollfd pfd;
        pfd.fd = m_server_socket;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ready = poll(&pfd, 1, 1000);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;  // Timeout — re-check m_running
        if (!m_running) break;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(m_server_socket, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket < 0) {
            if (m_running) {
                g_logger.warning("SocketWalletServer: accept() failed: " + std::string(strerror(errno)));
            }
            continue;
        }

        // Log client connection
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        g_logger.debug("SocketWalletServer: New client connection from " + std::string(client_ip) +
                      ":" + std::to_string(ntohs(client_addr.sin_port)));

        // Track client socket for graceful shutdown
        {
            std::lock_guard<std::mutex> lock(m_client_sockets_mutex);
            m_client_sockets.push_back(client_socket);
        }

        // Spawn client handler thread
        m_client_threads.push_back(
            std::make_unique<std::thread>(&SocketWalletServer::handleClient, this, client_socket)
        );
    }

    g_logger.debug("SocketWalletServer acceptor thread exiting");
}

void SocketWalletServer::handleClient(int client_socket) {
    g_logger.debug("SocketWalletServer: Client handler started (socket=" + std::to_string(client_socket) + ")");

    while (m_running) {
        // Receive request message (wire protocol format)
        // [4 bytes] message_type (uint32_t, network byte order)
        // [4 bytes] payload_size (uint32_t, network byte order)
        // [N bytes] payload

        uint8_t header[8];
        ssize_t n = pollRecv(client_socket, header, 8);
        if (n != 8) {
            if (m_running && n != -1) {
                g_logger.debug("SocketWalletServer: Failed to receive header (client disconnected)");
            }
            break;
        }

        // Parse header
        uint32_t message_type = ntohl(*(uint32_t*)(header));
        uint32_t payload_size = ntohl(*(uint32_t*)(header + 4));

        // Sanity check payload size
        if (payload_size > 32 * 1024 * 1024) {  // 32MB limit
            g_logger.error("SocketWalletServer: Payload size too large: " + std::to_string(payload_size));
            break;
        }

        // Receive payload
        std::vector<uint8_t> payload(payload_size);
        if (payload_size > 0) {
            n = pollRecv(client_socket, payload.data(), payload_size);
            if (n != static_cast<ssize_t>(payload_size)) {
                if (m_running) {
                    g_logger.error("SocketWalletServer: Failed to receive payload");
                }
                break;
            }
        }

        LightningMessage request(message_type, payload);

        g_logger.debug("SocketWalletServer: Received message type " + std::to_string(request.message_type) +
                      " (" + std::to_string(request.payload.size()) + " bytes)");

        // Don't start new work during shutdown
        if (!m_running) break;

        // Dispatch to handler
        LightningMessage response = dispatchMessage(request);

        // Send response (wire protocol format)
        uint32_t response_type_network = htonl(response.message_type);
        uint32_t response_size_network = htonl(static_cast<uint32_t>(response.payload.size()));

        // Send header
        ssize_t sent = send(client_socket, reinterpret_cast<const char*>(&response_type_network), static_cast<int>(4), 0);
        if (sent != 4) {
            g_logger.error("SocketWalletServer: Failed to send response header (type)");
            break;
        }

        sent = send(client_socket, reinterpret_cast<const char*>(&response_size_network), static_cast<int>(4), 0);
        if (sent != 4) {
            g_logger.error("SocketWalletServer: Failed to send response header (size)");
            break;
        }

        // Send payload
        if (response.payload.size() > 0) {
            sent = send(client_socket, reinterpret_cast<const char*>(response.payload.data()), static_cast<int>(response.payload.size()), 0);
            if (sent != static_cast<ssize_t>(response.payload.size())) {
                g_logger.error("SocketWalletServer: Failed to send response payload");
                break;
            }
        }

        g_logger.debug("SocketWalletServer: Sent response type " + std::to_string(response.message_type) +
                      " (" + std::to_string(response.payload.size()) + " bytes)");
    }

    // Remove from tracked sockets and close (if not already closed by Stop())
    {
        std::lock_guard<std::mutex> lock(m_client_sockets_mutex);
        auto it = std::find(m_client_sockets.begin(), m_client_sockets.end(), client_socket);
        if (it != m_client_sockets.end()) {
            m_client_sockets.erase(it);
            COMPAT_CLOSE_SOCKET(client_socket);
        }
        // If not found, Stop() already closed it
    }
    g_logger.debug("SocketWalletServer: Client handler exiting (socket=" + std::to_string(client_socket) + ")");
}

LightningMessage SocketWalletServer::dispatchMessage(const LightningMessage& message) {
    try {
        WalletMessageType msg_type = static_cast<WalletMessageType>(message.message_type);

        switch (msg_type) {
            case WalletMessageType::GET_NETWORK_HRP_REQUEST: {
                std::vector<uint8_t> response_payload = handleGetNetworkHRP(message.payload);
                return LightningMessage(
                    static_cast<uint32_t>(WalletMessageType::GET_NETWORK_HRP_RESPONSE),
                    response_payload
                );
            }

            case WalletMessageType::LIST_UTXOS_REQUEST: {
                std::vector<uint8_t> response_payload = handleListUnspentUTXOs(message.payload);
                return LightningMessage(
                    static_cast<uint32_t>(WalletMessageType::LIST_UTXOS_RESPONSE),
                    response_payload
                );
            }

            case WalletMessageType::DERIVE_LIGHTNING_KEY_REQUEST: {
                std::vector<uint8_t> response_payload = handleDeriveLightningKey(message.payload);
                return LightningMessage(
                    static_cast<uint32_t>(WalletMessageType::DERIVE_LIGHTNING_KEY_RESPONSE),
                    response_payload
                );
            }

            case WalletMessageType::TAPROOT_SIGHASH_REQUEST: {
                std::vector<uint8_t> response_payload = handleComputeTaprootSighash(message.payload);
                return LightningMessage(
                    static_cast<uint32_t>(WalletMessageType::TAPROOT_SIGHASH_RESPONSE),
                    response_payload
                );
            }

            case WalletMessageType::GET_CHANGE_ADDRESS_REQUEST: {
                std::vector<uint8_t> response_payload = handleGetNewChangeAddress(message.payload);
                return LightningMessage(
                    static_cast<uint32_t>(WalletMessageType::GET_CHANGE_ADDRESS_RESPONSE),
                    response_payload
                );
            }

            case WalletMessageType::DERIVE_KEY_FOR_SCRIPTPUBKEY_REQUEST: {
                std::vector<uint8_t> response_payload = handleDeriveKeyForScriptPubKey(message.payload);
                return LightningMessage(
                    static_cast<uint32_t>(WalletMessageType::DERIVE_KEY_FOR_SCRIPTPUBKEY_RESPONSE),
                    response_payload
                );
            }

            default:
                g_logger.error("SocketWalletServer: Unknown message type: " + std::to_string(message.message_type));
                return createErrorResponse("Unknown message type");
        }

    } catch (const std::exception& e) {
        g_logger.error("SocketWalletServer: Message handler failed: " + std::string(e.what()));
        return createErrorResponse(e.what());
    }
}

// ============================================================================
// Wire Protocol Message Handlers
// ============================================================================

std::vector<uint8_t> SocketWalletServer::handleGetNetworkHRP(const std::vector<uint8_t>& request_payload) {
    // Get network HRP
    std::string hrp = HrpForActiveNetworkRef();

    // Serialize response
    WireSerializer serializer;
    serializer.writeString(hrp);

    g_logger.debug("SocketWalletServer: GetNetworkHRP -> " + hrp);

    return serializer.finalize();
}

std::vector<uint8_t> SocketWalletServer::handleListUnspentUTXOs(const std::vector<uint8_t>& request_payload) {
    // Deserialize request
    WireSerializer deserializer;
    deserializer.reset(request_payload);

    uint32_t min_confirmations = deserializer.readUint32();
    uint32_t max_confirmations = deserializer.readUint32();

    // Check wallet availability
    if (!m_daemon_ctx || !m_daemon_ctx->wallet) {
        throw std::runtime_error("Wallet not available");
    }

    // Get wallet reference
    auto& wallet_service = *m_daemon_ctx->wallet;
    WalletManager& wallet_mgr = wallet_service.get();

    // Call wallet method
    auto utxos = wallet_mgr.listUnspentUTXOs(min_confirmations, max_confirmations);

    // Serialize response
    WireSerializer serializer;
    serializer.writeVarInt(utxos.size());

    for (const auto& wallet_utxo : utxos) {
        WireUTXO wire_utxo;

        // Convert txid string to bytes
        uint256 txid = uint256::FromHexUnsafe(wallet_utxo.txid);
        wire_utxo.txid.assign(txid.begin(), txid.end());

        wire_utxo.vout = wallet_utxo.vout;
        wire_utxo.value = wallet_utxo.amount_una;
        wire_utxo.scriptPubKey = {};  // TODO: Look up from chainstate if needed
        wire_utxo.confirmations = wallet_utxo.confirmations;
        wire_utxo.is_coinbase = wallet_utxo.is_coinbase;

        wire_utxo.serialize(serializer);
    }

    g_logger.debug("SocketWalletServer: ListUnspentUTXOs -> " + std::to_string(utxos.size()) +
                  " UTXOs (min_conf=" + std::to_string(min_confirmations) +
                  ", max_conf=" + std::to_string(max_confirmations) + ")");

    return serializer.finalize();
}

std::vector<uint8_t> SocketWalletServer::handleDeriveLightningKey(const std::vector<uint8_t>& request_payload) {
    // Deserialize request
    WireSerializer deserializer;
    deserializer.reset(request_payload);

    uint32_t key_type_raw = deserializer.readUint32();
    uint32_t account = deserializer.readUint32();
    uint32_t index = deserializer.readUint32();

    LightningKeyType key_type = static_cast<LightningKeyType>(key_type_raw);

    // Check wallet availability
    if (!m_daemon_ctx || !m_daemon_ctx->wallet) {
        throw std::runtime_error("Wallet not available");
    }

    // Get HD wallet reference
    auto& wallet_service = *m_daemon_ctx->wallet;
    WalletManager& wallet_mgr = wallet_service.get();
    HDWallet* hd_wallet = wallet_mgr.getHDWallet();

    if (!hd_wallet) {
        throw std::runtime_error("HD wallet not initialized");
    }

    // Get seed from HD wallet for Lightning key derivation
    auto seed = hd_wallet->GetSeed();
    if (seed.empty()) {
        throw std::runtime_error("Wallet seed not available");
    }

    // Create Lightning key deriver
    dinero::lightning::LightningKeyDeriver key_deriver(seed.data(), seed.size(), dinero::consensus::DINERO_COIN_TYPE);

    std::vector<uint8_t> private_key;

    // Derive key based on type
    switch (key_type) {
        case LightningKeyType::NODE_IDENTITY: {
            auto identity = key_deriver.GetNodeIdentity();
            private_key = identity.privkey;
            break;
        }

        case LightningKeyType::FUNDING:
            private_key = key_deriver.GetFundingKey(index);
            break;

        case LightningKeyType::REVOCATION_BASE:
            private_key = key_deriver.GetRevocationBaseKey(index);
            break;

        case LightningKeyType::PAYMENT_BASE:
            private_key = key_deriver.GetPaymentBaseKey(index);
            break;

        case LightningKeyType::DELAYED_PAYMENT_BASE:
            private_key = key_deriver.GetDelayedPaymentBaseKey(index);
            break;

        case LightningKeyType::HTLC_BASE:
            private_key = key_deriver.GetHTLCBaseKey(index);
            break;

        default:
            throw std::runtime_error("Unknown key type");
    }

    if (private_key.size() != 32) {
        throw std::runtime_error("Key derivation returned invalid key");
    }

    // Serialize response
    WireSerializer serializer;
    serializer.writeBytes(private_key);

    g_logger.debug("SocketWalletServer: DeriveLightningKey -> type=" + std::to_string(key_type_raw) +
                  ", index=" + std::to_string(index));

    return serializer.finalize();
}

std::vector<uint8_t> SocketWalletServer::handleComputeTaprootSighash(const std::vector<uint8_t>& request_payload) {
    // Deserialize request
    WireSerializer deserializer;
    deserializer.reset(request_payload);

    std::vector<uint8_t> raw_tx = deserializer.readBytes();
    uint32_t input_index = deserializer.readUint32();

    uint64_t prevout_count = deserializer.readVarInt();
    std::vector<uint64_t> prevout_values;
    for (uint64_t i = 0; i < prevout_count; i++) {
        prevout_values.push_back(deserializer.readUint64());
    }

    uint64_t script_count = deserializer.readVarInt();
    std::vector<std::vector<uint8_t>> prevout_scripts;
    for (uint64_t i = 0; i < script_count; i++) {
        prevout_scripts.push_back(deserializer.readBytes());
    }

    uint8_t sighash_type = deserializer.readUint8();
    std::vector<uint8_t> annex = deserializer.readBytes();

    // Deserialize transaction
    Transaction tx;
    if (!TransactionSerializer::Deserialize(tx, raw_tx)) {
        throw std::runtime_error("Failed to deserialize transaction");
    }

    // Compute sighash
    std::vector<uint8_t> sighash = consensus::ScriptVerifier::ComputeTaprootSighash(
        tx,
        input_index,
        prevout_values,
        prevout_scripts,
        sighash_type,
        annex
    );

    if (sighash.size() != 32) {
        throw std::runtime_error("Sighash computation returned invalid hash");
    }

    // Serialize response
    WireSerializer serializer;
    serializer.writeBytes(sighash);

    g_logger.debug("SocketWalletServer: ComputeTaprootSighash -> input_index=" + std::to_string(input_index) +
                  ", sighash_type=0x" + std::to_string(sighash_type));

    return serializer.finalize();
}

std::vector<uint8_t> SocketWalletServer::handleGetNewChangeAddress(const std::vector<uint8_t>& request_payload) {
    // Deserialize request
    WireSerializer deserializer;
    deserializer.reset(request_payload);

    std::string label = deserializer.readString();

    // Check if wallet is available
    if (!m_daemon_ctx || !m_daemon_ctx->wallet) {
        throw std::runtime_error("Wallet service not available");
    }

    if (!m_daemon_ctx->wallet->hasActiveWallet()) {
        throw std::runtime_error("No active wallet");
    }

    // Get wallet manager
    WalletManager& wallet_mgr = m_daemon_ctx->wallet->get();

    // Get new change address
    std::string address = wallet_mgr.getNewChangeAddress(label);

    // Serialize response
    WireSerializer serializer;
    serializer.writeString(address);

    g_logger.debug("SocketWalletServer: GetNewChangeAddress -> " + address + " (label: " + label + ")");

    return serializer.finalize();
}

std::vector<uint8_t> SocketWalletServer::handleDeriveKeyForScriptPubKey(const std::vector<uint8_t>& request_payload) {
    // Deserialize request
    WireSerializer deserializer;
    deserializer.reset(request_payload);

    std::string script_pubkey_hex = deserializer.readString();

    // Check if wallet is available
    if (!m_daemon_ctx || !m_daemon_ctx->wallet) {
        throw std::runtime_error("Wallet service not available");
    }

    if (!m_daemon_ctx->wallet->hasActiveWallet()) {
        throw std::runtime_error("No active wallet");
    }

    // Get wallet manager
    WalletManager& wallet_mgr = m_daemon_ctx->wallet->get();

    // Derive private key for scriptPubKey
    auto privkey_opt = wallet_mgr.deriveKeyForScriptPubKey(script_pubkey_hex);

    if (!privkey_opt.has_value()) {
        throw std::runtime_error("No private key found for scriptPubKey");
    }

    // Serialize response
    WireSerializer serializer;
    serializer.writeBytes(privkey_opt.value());

    g_logger.debug("SocketWalletServer: DeriveKeyForScriptPubKey -> Derived private key");

    return serializer.finalize();
}

// ============================================================================
// Poll-guarded recv (replaces raw recv(MSG_WAITALL) for clean shutdown)
// ============================================================================

ssize_t SocketWalletServer::pollRecv(int fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len && m_running) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ready = poll(&pfd, 1, 1000);  // 1s timeout, re-check m_running
        if (ready < 0) {
            if (errno == EINTR) continue;  // Interrupted by signal, retry
            return -1;  // Real error
        }
        if (ready == 0) continue;  // Timeout — loop and check m_running

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;

        ssize_t n = recv(fd, static_cast<char*>(buf) + total, len - total, 0);
        if (n <= 0) return (total > 0) ? static_cast<ssize_t>(total) : n;
        total += n;
    }
    return m_running ? static_cast<ssize_t>(total) : -1;
}

LightningMessage SocketWalletServer::createErrorResponse(const std::string& error_message) {
    WireSerializer serializer;
    serializer.writeString(error_message);

    return LightningMessage(
        static_cast<uint32_t>(WalletMessageType::ERROR_RESPONSE),
        serializer.finalize()
    );
}

} // namespace grpc_server
} // namespace dinero
