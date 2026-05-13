#include "daemon/services/address_manager_service.h"
#include "daemon/daemon_context.h"
#include <iostream>

namespace dinero {
namespace daemon {

AddressManagerService::AddressManagerService()
    : manager_(std::make_unique<p2p::AddressManager>()) {
}

AddressManagerService::~AddressManagerService() = default;

bool AddressManagerService::Init(DaemonContext& ctx) {
    ctx_ = &ctx;
    std::cout << "[AddressManagerService] Initialized (peer discovery & selection)" << std::endl;
    return true;
}

bool AddressManagerService::Start() {
    std::cout << "[AddressManagerService] Started (address book active)" << std::endl;
    return true;
}

void AddressManagerService::Stop() {
    std::cout << "[AddressManagerService] Stopped" << std::endl;
}

} // namespace daemon
} // namespace dinero
