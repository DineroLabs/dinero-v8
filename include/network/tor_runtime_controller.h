#pragma once

#include "network/tor_control.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace dinero::network {

struct TorRuntimeStatus {
    bool requested{false};
    bool active{false};
    std::string onion_address;
    std::string message;
};

// Stores exactly one non-secret preference byte (`0` or `1`). Credentials and
// onion keys remain in their existing protected stores.
class TorOptInStore {
public:
    explicit TorOptInStore(std::string path) : path_(std::move(path)) {}
    std::optional<bool> Read() const;
    bool Write(bool enabled, std::string* error) const;
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

class TorRuntimeController {
public:
    using Factory = std::function<std::unique_ptr<ITorOnionService>()>;
    using AddressCallback = std::function<void(const std::string&)>;

    TorRuntimeController(TorOptInStore store, Factory factory,
                         AddressCallback advertise,
                         AddressCallback withdraw);

    // Applies a persisted preference when present, otherwise the daemon's
    // startup configuration. Serialized with runtime transitions.
    TorRuntimeStatus Initialize(bool configured_default);
    TorRuntimeStatus SetEnabled(bool enabled);
    void Shutdown();
    TorRuntimeStatus Status() const;

private:
    TorRuntimeStatus SetEnabledLocked(bool enabled, bool persist);
    static std::string PublicFailureMessage();

    TorOptInStore store_;
    Factory factory_;
    AddressCallback advertise_;
    AddressCallback withdraw_;
    mutable std::mutex mutex_;
    std::unique_ptr<ITorOnionService> service_;
    TorRuntimeStatus status_;
};

}  // namespace dinero::network
