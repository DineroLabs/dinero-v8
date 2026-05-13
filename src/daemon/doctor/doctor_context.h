// doctor_context.h - Read-only environment context for doctor checks
// Provides checks with access to data directory, network, and environment
// without requiring a running daemon.
#pragma once

#include "daemon/doctor/doctor_types.h"
#include <string>

namespace dinero {
namespace doctor {

class DoctorContext {
public:
    DoctorContext(const std::string& datadir, const std::string& network);

    // Data directory (resolved, absolute)
    const std::string& DataDir() const { return datadir_; }

    // Network name ("mainnet", "testnet", "regtest")
    const std::string& Network() const { return network_; }

    // Run mode (set by command before execution)
    RunMode Mode() const { return mode_; }
    void SetMode(RunMode m) { mode_ = m; }

    // Node version string (set from build macros)
    const std::string& NodeVersion() const { return node_version_; }
    void SetNodeVersion(const std::string& v) { node_version_ = v; }

    // RPC port (for checks that probe the running daemon)
    uint16_t RpcPort() const { return rpc_port_; }
    void SetRpcPort(uint16_t p) { rpc_port_ = p; }

    // P2P port
    uint16_t P2pPort() const { return p2p_port_; }
    void SetP2pPort(uint16_t p) { p2p_port_ = p; }

private:
    std::string datadir_;
    std::string network_;
    RunMode mode_ = RunMode::QUICK;
    std::string node_version_;
    uint16_t rpc_port_ = 20998;
    uint16_t p2p_port_ = 20999;
};

} // namespace doctor
} // namespace dinero
