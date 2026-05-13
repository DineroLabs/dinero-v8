#pragma once

#include "daemon/iservice.h"
#include "p2p/compact_proof_blocks.h"
#include <memory>

namespace dinero {
namespace daemon {

/**
 * @brief Phase 34.7: Compact-Proof Block Service
 *
 * Service wrapper for CompactProofBlockManager.
 * Combines BIP152 compact blocks with Utreexo proofs for:
 * - 90%+ bandwidth reduction
 * - Stateless block validation
 * - Instant sync capability
 *
 * Integrates with:
 * - CompactBlockService: For BIP152 compact blocks
 * - P2PService: For blocktxnproofs message handling
 * - FastSyncService: For stateless validation
 */
class CompactProofBlockService : public IService {
public:
    CompactProofBlockService();
    ~CompactProofBlockService() override;

    std::string Name() const override { return "CompactProofBlockService"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    // Access to manager
    p2p::CompactProofBlockManager* getManager() { return manager_.get(); }
    const p2p::CompactProofBlockManager* getManager() const { return manager_.get(); }

    // Configuration
    void setStatelessMode(bool enabled);
    bool isStatelessMode() const;

private:
    DaemonContext* ctx_{nullptr};
    std::unique_ptr<p2p::CompactProofBlockManager> manager_;
};

} // namespace daemon
} // namespace dinero
