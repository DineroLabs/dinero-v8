// Copyright (c) 2026 The Dinero Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dinero::network {

struct TorControlReply {
    int code{0};
    std::vector<std::string> lines;
    bool ok() const { return code >= 200 && code < 300; }
};

// Parse a complete Tor control reply. Exposed so protocol edge cases can be
// tested without requiring Tor on the build host.
bool ParseTorControlReply(const std::string& wire,
                          TorControlReply* reply,
                          std::string* error);

struct TorOnionServiceConfig {
    std::string control_host{"127.0.0.1"};
    uint16_t control_port{9051};
    uint16_t virtual_port{20999};
    std::string target_host{"127.0.0.1"};
    uint16_t target_port{20999};
    std::string private_key_path;
    // Optional. Cookie authentication discovered through PROTOCOLINFO is
    // preferred. A configured password is used only if COOKIE is unavailable.
    std::string control_password;
    int timeout_seconds{5};
};

struct TorOnionServiceStatus {
    bool requested{false};
    bool active{false};
    std::string service_id;
    std::string onion_address;
    std::string authentication;
    std::string message;
};

// Explicit, lifecycle-owned Tor v3 onion service. Start authenticates to a
// user-supplied/local Tor control port, restores or creates an ED25519-V3 key,
// and installs a detached ADD_ONION mapping. Stop removes that mapping. Dinero
// never starts Tor itself and never enables this class without listenonion=1.
class TorOnionService {
public:
    explicit TorOnionService(TorOnionServiceConfig config);
    ~TorOnionService();

    TorOnionService(const TorOnionService&) = delete;
    TorOnionService& operator=(const TorOnionService&) = delete;

    bool Start();
    void Stop();
    const TorOnionServiceStatus& status() const { return status_; }

private:
    bool SetError(const std::string& message);

    TorOnionServiceConfig config_;
    TorOnionServiceStatus status_;
};

}  // namespace dinero::network
