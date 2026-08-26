#include "network/tor_runtime_controller.h"

#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define NOMINMAX
#include <windows.h>
#endif

namespace dinero::network {

std::optional<bool> TorOptInStore::Read() const {
#ifndef _WIN32
    const int fd = ::open(path_.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return std::nullopt;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        ::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    char contents[4]{};
    const ssize_t size = ::read(fd, contents, sizeof(contents));
    ::close(fd);
    if (size < 1 || size > 3 || (contents[0] != '0' && contents[0] != '1')) {
        return std::nullopt;
    }
    for (ssize_t i = 1; i < size; ++i) {
        if (contents[i] != '\n' && contents[i] != '\r') return std::nullopt;
    }
    return contents[0] == '1';
#else
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path_, status_error);
    if (status_error || !std::filesystem::is_regular_file(status)) {
        return std::nullopt;
    }
    std::ifstream input(path_, std::ios::binary);
    char value = '\0';
    if (!input.get(value) || (value != '0' && value != '1')) return std::nullopt;
    char extra = '\0';
    while (input.get(extra)) {
        if (extra != '\n' && extra != '\r') return std::nullopt;
    }
    return value == '1';
#endif
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
    std::filesystem::path tmp = path;
    tmp += ".tmp";
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
    const int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd < 0 || ::fsync(fd) != 0) {
        if (fd >= 0) ::close(fd);
        std::filesystem::remove(tmp, ec);
        if (error) *error = "could not sync Tor preference";
        return false;
    }
    ::close(fd);
#endif
#ifdef _WIN32
    if (!::MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec = std::error_code(static_cast<int>(::GetLastError()),
                             std::system_category());
    }
#else
    std::filesystem::rename(tmp, path, ec);
#endif
    if (ec) {
        std::filesystem::remove(tmp, ec);
        if (error) *error = "could not install Tor preference";
        return false;
    }
#ifndef _WIN32
    const int dir_fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        (void)::fsync(dir_fd);
        ::close(dir_fd);
    }
#endif
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
        const TorRuntimeStatus previous = status_;
        std::string address;
        if (service_ && service_->status().active) {
            address = service_->status().onion_address;
            if (!service_->Stop()) {
                status_.requested = true;
                status_.active = true;
                status_.onion_address = address;
                status_.message = "Could not disable Tor onion reachability; check the local daemon log";
                return status_;
            }
        }
        std::string error;
        if (persist && !store_.Write(false, &error)) {
            // The durable preference remains on. Restore runtime service when
            // possible so runtime and restart behavior agree. Advertisement
            // was deliberately retained until the preference committed.
            const bool restored = previous.requested && service_ && service_->Start();
            status_.requested = previous.requested;
            status_.active = restored;
            status_.onion_address = restored
                ? service_->status().onion_address : std::string{};
            if (restored && status_.onion_address != address) {
                if (withdraw_ && !address.empty()) withdraw_(address);
                if (advertise_ && !status_.onion_address.empty()) {
                    advertise_(status_.onion_address);
                }
            } else if (!restored && withdraw_ && !address.empty()) {
                withdraw_(address);
            }
            status_.message = "Tor preference could not be saved";
            return status_;
        }
        if (withdraw_ && !address.empty()) withdraw_(address);
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
