#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace dinero::network {

enum class PortMappingMode {
    Disabled,
    Upnp,
    NatPmp,
    Auto,
};

struct PortMappingConfig {
    PortMappingMode mode{PortMappingMode::Disabled};
    uint16_t internal_port{0};
    uint16_t external_port{0};
    int lifetime_seconds{7200};
    std::string description{"Dinero P2P"};
    // Optional caller-provided cancellation/deadline probe. Checked between
    // blocking phases (SSDP discovery, IGD SOAP, AddPortMapping; NAT-PMP
    // request/response). A truly-hung router still can't be interrupted
    // mid-syscall, but we'll exit cleanly between phases instead of looping
    // forever in ReadNatPmpResponse or re-entering NAT-PMP after a stalled
    // UPnP attempt.
    std::function<bool()> should_abort{};

    bool enabled() const {
        return mode != PortMappingMode::Disabled && internal_port != 0 && external_port != 0;
    }
};

struct PortMappingResult {
    bool success{false};
    std::string protocol;
    std::string external_address;
    std::string message;
};

PortMappingMode ParsePortMappingMode(const std::string& value,
                                     bool upnp_enabled,
                                     bool natpmp_enabled);

std::string PortMappingModeName(PortMappingMode mode);

class PortMappingSession {
public:
    PortMappingSession() = default;
    ~PortMappingSession();

    PortMappingSession(const PortMappingSession&) = delete;
    PortMappingSession& operator=(const PortMappingSession&) = delete;

    PortMappingResult Start(const PortMappingConfig& config);
    void Stop();

    bool active() const { return active_; }
    std::string protocol() const { return protocol_; }

private:
    bool TryUpnp(const PortMappingConfig& config, PortMappingResult* result);
    bool TryNatPmp(const PortMappingConfig& config, PortMappingResult* result);
    void StopUpnp();
    void StopNatPmp();

    bool active_{false};
    PortMappingMode mode_{PortMappingMode::Disabled};
    uint16_t internal_port_{0};
    uint16_t external_port_{0};
    std::string protocol_;
};

}  // namespace dinero::network
