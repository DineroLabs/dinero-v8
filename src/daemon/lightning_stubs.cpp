// Lightning stubs for genesis build (Layer 3 off-chain - not consensus-critical)
// These stubs allow building without Lightning during genesis verification

#include "daemon/iservice.h"
#include <iostream>

// Forward declarations
struct DaemonContext;

namespace dinero {
    class WalletManager;  // Forward declaration
}

namespace dinero {
namespace lightning {

// Minimal LightningService stub class that implements IService interface
class LightningService : public dinero::IService {
public:
    LightningService();
    ~LightningService() override;

    // IService interface implementation
    std::string Name() const override;
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Lightning-specific methods (wallet-level)
    bool InitForWallet(dinero::WalletManager*);
    void StopForWallet();
};

// Method implementations (outside class for proper linking)
LightningService::LightningService() {
    // No-op: Lightning disabled for genesis
}

LightningService::~LightningService() {
    // No-op: Lightning disabled for genesis
}

std::string LightningService::Name() const {
    return "Lightning";
}

bool LightningService::Init(DaemonContext& ctx) {
    std::cout << "⚡ LightningService: Daemon-level initialization complete (Lightning will initialize per-wallet)" << std::endl;
    return true;  // Always succeed - Lightning is optional
}

bool LightningService::Start() {
    return true;  // Always succeed - Lightning is optional
}

void LightningService::Stop() {
    // No-op: Lightning disabled for genesis
}

bool LightningService::InitForWallet(dinero::WalletManager*) {
    // No-op: Lightning disabled for genesis
    return true;  // Always succeed - Lightning is optional
}

void LightningService::StopForWallet() {
    // No-op: Lightning disabled for genesis
}

} // namespace lightning
} // namespace dinero

// RPC registration stubs (C++ linkage - must match declarations)
void register_lightning_methods() {
    // No-op: Lightning RPC disabled for genesis
}

void registerLightningWebSocketRPC() {
    // No-op: Lightning WebSocket RPC disabled for genesis
}
