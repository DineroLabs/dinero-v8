#include "network/tor_runtime_controller.h"

#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace dinero::network {

std::optional<bool> TorOptInStore::Read() const {
    std::ifstream input(path_, std::ios::binary);
    char value = '\0';
    if (!input.get(value) || (value != '0' && value != '1')) return std::nullopt;
    char extra = '\0';
    while (input.get(extra)) {
        if (extra != '\n' && extra != '\r') return std::nullopt;
    }
    return value == '1';
}

bool TorOptInStore::Write(bool enabled, std::string* error) const {
    if (path_.empty()) {
        if (error) *error = "Tor preference path is empty";
        return false;
    }
    std::error_code ec;
    const auto path = std::filesystem::path(path_);
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "could not create Tor preference directory";
        return false;
    }
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
        if (!output || !(output << (enabled ? "1\n" : "0\n"))) {
            if (error) *error = "could not write Tor preference";
            return false;
        }
        output.flush();
        if (!output) {
            if (error) *error = "could not flush Tor preference";
            return false;
        }
    }
#ifndef _WIN32
    if (chmod(tmp.c_str(), S_IRUSR | S_IWUSR) != 0) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "could not protect Tor preference";
        return false;
    }
#endif
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "could not install Tor preference";
        return false;
    }
    return true;
}

TorRuntimeController::TorRuntimeController(
    TorOptInStore store, Factory factory, AddressCallback advertise,
    AddressCallback withdraw)
    : store_(std::move(store)), factory_(std::move(factory)),
      advertise_(std::move(advertise)), withdraw_(std::move(withdraw)) {}

TorRuntimeStatus TorRuntimeController::Initialize(bool configured_default) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto persisted = store_.Read();
    return SetEnabledLocked(persisted.value_or(configured_default), false);
}

TorRuntimeStatus TorRuntimeController::SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    return SetEnabledLocked(enabled, true);
}

TorRuntimeStatus TorRuntimeController::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void TorRuntimeController::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!service_ || !service_->status().active) return;
    const std::string address = service_->status().onion_address;
    if (service_->Stop() && withdraw_ && !address.empty()) withdraw_(address);
    service_.reset();
}

std::string TorRuntimeController::PublicFailureMessage() {
    return "Tor onion reachability is unavailable; check the local daemon log";
}

TorRuntimeStatus TorRuntimeController::SetEnabledLocked(bool enabled,
                                                        bool persist) {
    if (!enabled) {
        std::string error;
        if (persist && !store_.Write(false, &error)) {
            status_.message = "Tor preference could not be saved";
            return status_;
        }
        if (service_ && service_->status().active) {
            const std::string address = service_->status().onion_address;
            if (!service_->Stop()) {
                // Removal was not confirmed, so roll the preference back to
                // on. A restart will retry with the same protected identity.
                if (persist) (void)store_.Write(true, nullptr);
                status_.requested = true;
                status_.active = true;
                status_.onion_address = address;
                status_.message = "Could not disable Tor onion reachability; check the local daemon log";
                return status_;
            }
            if (withdraw_ && !address.empty()) withdraw_(address);
        }
        service_.reset();
        status_ = {};
        status_.message = "Tor onion reachability is off";
        return status_;
    }

    std::string error;
    if (persist && !store_.Write(true, &error)) {
        status_.message = "Tor preference could not be saved";
        return status_;
    }
    status_.requested = true;
    if (service_ && service_->status().active) return status_;
    service_ = factory_ ? factory_() : nullptr;
    if (!service_ || !service_->Start()) {
        status_.active = false;
        status_.onion_address.clear();
        status_.message = PublicFailureMessage();
        return status_;
    }
    status_.active = true;
    status_.onion_address = service_->status().onion_address;
    status_.message = "Tor onion reachability is active";
    if (advertise_ && !status_.onion_address.empty()) advertise_(status_.onion_address);
    return status_;
}

}  // namespace dinero::network
