#include "daemon/block_relay_manager.h"
#include "daemon/services/chainstate_service.h"

namespace dinero {
// Keep the daemon adapter separate from the transport implementation so callers
// that use only the historical relay interface do not acquire chainstate linkage.
void BlockRelayManager::SetFullBlockSource(const std::shared_ptr<ChainstateService>& source) {
    const std::weak_ptr<ChainstateService> owner = source;
    full_block_source_ = [owner](const uint256& hash) -> std::optional<FullBlockResponse> {
        const auto selected = owner.lock();
        if (!selected) return std::nullopt;
        auto captured = selected->getBlockRpcSnapshot(hash);
        if (!captured.ok()) return std::nullopt;
        return FullBlockResponse{captured->header, captured->height, std::move(captured->bytes)};
    };
    full_block_source_configured_ = true;
}
} // namespace dinero
