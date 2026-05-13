#pragma once

#include "daemon/iservice.h"
#include "p2p/headers_first_sync.h"
#include <memory>
#include <string>
#include <vector>

namespace dinero {
namespace daemon {

/**
 * Architecture V3: Service wrapper for headers-first sync
 *
 * Provides centralized access to headers-first synchronization mechanism.
 * Replaces global g_headers_sync variable with service-based architecture.
 *
 * Key Features:
 * - Headers-first blockchain synchronization
 * - Block download scheduling after header validation
 * - Sync progress tracking and metrics
 * - P2P integration for efficient sync
 *
 * Access Pattern:
 *   ctx->headers_sync->startSync(peer_id);
 *   if (ctx->headers_sync->isSyncing()) { ... }
 *   auto status = ctx->headers_sync->getStatus();
 */
class HeadersSyncService : public IService {
public:
    HeadersSyncService();
    ~HeadersSyncService() override;

    // IService interface
    std::string Name() const override { return "HeadersSyncService"; }
    bool Init(DaemonContext& ctx) override;
    bool Start() override;
    void Stop() override;

    /**
     * Core sync operations
     */
    void startSync(const std::string& peer_id);
    void stopSync();
    bool isSyncing() const;
    p2p::SyncState getCurrentState() const;

    /**
     * Header processing
     */
    bool processHeaders(const std::string& peer_id, const p2p::HeadersResponse& response);
    void requestNextHeaders(const std::string& peer_id);

    /**
     * Block requests after headers validated
     */
    void requestBlocks(const std::vector<std::string>& block_hashes);
    bool processBlock(const std::string& block_hash, const std::string& block_data);

    /**
     * Status and metrics
     */
    uint32_t getBestHeight() const;
    std::string getBestBlockHash() const;
    uint32_t getHeadersCount() const;
    uint32_t getBlocksDownloaded() const;

    /**
     * Configuration
     */
    void setMaxHeadersPerRequest(uint32_t max_headers);
    void setValidationEnabled(bool enabled);
    void setTimeout(std::chrono::seconds timeout);

    /**
     * JSON serialization for RPC/logging
     */
    din::Json getStatus() const;
    din::Json getMetrics() const;

    /**
     * Direct access to manager (for advanced use cases)
     */
    p2p::HeadersFirstSync* getManager() { return manager_.get(); }
    const p2p::HeadersFirstSync* getManager() const { return manager_.get(); }

private:
    DaemonContext* ctx_{nullptr};
    std::unique_ptr<p2p::HeadersFirstSync> manager_;
};

} // namespace daemon
} // namespace dinero
