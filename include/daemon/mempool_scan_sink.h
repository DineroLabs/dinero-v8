#pragma once
#include <memory>
#include <string>

namespace dinero {
class IMempoolScanSink {
public:
    // Retains the scanner-registry lock across unsupported-family admission.
    struct Guard { virtual ~Guard() = default; };
    virtual ~IMempoolScanSink() = default;
    virtual std::unique_ptr<Guard> AcquireEmptyRegistry() = 0;
    virtual void ScanMempoolTransaction(const std::string& tx_hex) = 0;
};
} // namespace dinero
