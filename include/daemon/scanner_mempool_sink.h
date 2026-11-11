#pragma once
#include "daemon/mempool_scan_sink.h"
#include "privacy/silent_scanner_manager.h"

namespace dinero {
// Node-owned implementation: preserves ScannerManager's parse and lock scope
// without pulling its wallet/discovery dependencies into the mempool library.
class ScannerMempoolSink final : public IMempoolScanSink {
    struct RegistryGuard final : Guard {
        explicit RegistryGuard(std::unique_ptr<din::sp::ScannerManager::EmptyRegistry> value)
            : registry(std::move(value)) {}
        std::unique_ptr<din::sp::ScannerManager::EmptyRegistry> registry;
    };
public:
    std::unique_ptr<Guard> AcquireEmptyRegistry() override {
        auto registry = manager_.acquireEmptyRegistry();
        if (!registry) return {};
        return std::make_unique<RegistryGuard>(std::move(registry));
    }
    void ScanMempoolTransaction(const std::string& tx_hex) override {
        manager_.scanMempoolTransaction(tx_hex);
    }
private:
    din::sp::ScannerManager manager_;
};
} // namespace dinero
