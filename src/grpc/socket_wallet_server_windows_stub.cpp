// socket_wallet_server_windows_stub.cpp
//
// Windows-only stub for SocketWalletServer. The real implementation
// (socket_wallet_server.cpp) uses POSIX poll() with struct pollfd and is
// built only on POSIX. dinero-qt's bundled Windows daemon doesn't ship
// Lightning support, so the wallet socket server is a no-op on Windows;
// the stubs here exist purely to satisfy the linker for the symbols
// referenced by daemon_app.cpp.
//
// To enable the real implementation on Windows, swap the poll()/pollfd
// usage in src/grpc/socket_wallet_server.cpp for WSAPoll()/WSAPOLLFD and
// remove the WIN32 exclusion in CMakeLists.txt's SOCKET_SERVER_SOURCES.

#include "grpc/socket_wallet_server.h"

#include <iostream>

namespace dinero {
namespace grpc_server {

SocketWalletServer::SocketWalletServer(DaemonContext* daemon_ctx,
                                       const std::string& address)
    : m_daemon_ctx(daemon_ctx),
      m_address(address),
      m_running(false),
      m_server_socket(-1) {
    // Stub: no socket bound, no acceptor thread spawned.
}

SocketWalletServer::~SocketWalletServer() {
    // Stub: nothing to tear down.
}

bool SocketWalletServer::Start() {
    m_last_error =
        "SocketWalletServer is not supported on Windows in this build "
        "(Lightning wallet socket server is POSIX-only).";
    std::cerr << "[SocketWalletServer/Windows-stub] " << m_last_error << "\n";
    return false;
}

void SocketWalletServer::Stop() {
    // Stub: no-op.
}

void SocketWalletServer::acceptorLoop() {
    // Stub: never invoked because Start() returns false.
}

void SocketWalletServer::handleClient(int /*client_socket*/) {
    // Stub.
}

ssize_t SocketWalletServer::pollRecv(int /*fd*/,
                                      void* /*buf*/,
                                      size_t /*len*/) {
    return -1;
}

lightning::LightningMessage
SocketWalletServer::dispatchMessage(const lightning::LightningMessage& message) {
    return createErrorResponse(
        "SocketWalletServer not available on Windows (stub build).");
}

lightning::LightningMessage
SocketWalletServer::createErrorResponse(const std::string& /*error_message*/) {
    return lightning::LightningMessage{};
}

std::vector<uint8_t> SocketWalletServer::handleGetNetworkHRP(
    const std::vector<uint8_t>& /*request_payload*/) {
    return {};
}

std::vector<uint8_t> SocketWalletServer::handleListUnspentUTXOs(
    const std::vector<uint8_t>& /*request_payload*/) {
    return {};
}

std::vector<uint8_t> SocketWalletServer::handleDeriveLightningKey(
    const std::vector<uint8_t>& /*request_payload*/) {
    return {};
}

std::vector<uint8_t> SocketWalletServer::handleComputeTaprootSighash(
    const std::vector<uint8_t>& /*request_payload*/) {
    return {};
}

std::vector<uint8_t> SocketWalletServer::handleGetNewChangeAddress(
    const std::vector<uint8_t>& /*request_payload*/) {
    return {};
}

std::vector<uint8_t> SocketWalletServer::handleDeriveKeyForScriptPubKey(
    const std::vector<uint8_t>& /*request_payload*/) {
    return {};
}

}  // namespace grpc_server
}  // namespace dinero
