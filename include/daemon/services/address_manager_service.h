#pragma once

#include "daemon/iservice.h"
#include "p2p/addrman.h"
#include <memory>

namespace dinero {
namespace daemon {

/**
 * Architecture V3: Service wrapper for address manager (peer discovery & selection)
 * Manages peer address book with connection statistics and selection algorithms
 */
class AddressManagerService : public IService {
public:
    AddressManagerService();
    ~AddressManagerService() override;

    std::string Name() const override { return "AddressManagerService"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Direct access to manager
    p2p::AddressManager* getManager() { return manager_.get(); }
    const p2p::AddressManager* getManager() const { return manager_.get(); }

private:
    DaemonContext* ctx_{nullptr};
    std::unique_ptr<p2p::AddressManager> manager_;
};

} // namespace daemon
} // namespace dinero
