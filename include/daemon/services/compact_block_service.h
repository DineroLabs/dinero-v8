#pragma once

#include "daemon/iservice.h"
#include <cstdint>

namespace dinero {

// Forward declaration
class BlockRelayManager;

namespace daemon {

/**
 * Architecture V3: Service wrapper for compact block relay (BIP152)
 *
 * Phase Plan-A: Binary Wire Format Migration
 * - Removed JSON-based CompactBlockManager dependency
 * - Stats now delegated to BlockRelayManager (binary implementation)
 * - Provides 90% bandwidth reduction using short transaction IDs
 */
class CompactBlockService : public IService {
public:
    CompactBlockService();
    ~CompactBlockService() override;

    std::string Name() const override { return "CompactBlockService"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Stats (delegated to BlockRelayManager)
    uint64_t getBlocksProcessed() const;
    double getReconstructionRate() const;
    uint64_t getBandwidthSaved() const;

private:
    DaemonContext* ctx_{nullptr};
};

} // namespace daemon
} // namespace dinero
